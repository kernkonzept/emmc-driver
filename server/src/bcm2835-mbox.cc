/*
 * Copyright (C) 2024 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
/**
 * \file
 * Simple mailbox driver for bcm2835 firmware.
 */

#include <cstring>
#include <sstream>
#include <iomanip>

#include <l4/cxx/utils>
#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/vbus/vbus>

#include "bcm2835-mbox.h"
#include "debug.h"
#include "mmio.h"
#include "util.h"

static Dbg warn(Dbg::Warn, "mbox");
static Dbg trace2(Dbg::Trace2, "mbox");

// Register offsets
enum
{
  Mbox0_read           = 0x0,
  Mbox0_peak           = 0x10,
  Mbox0_sender         = 0x14,
  Mbox0_status         = 0x18,
  Mbox0_configuration  = 0x1c,
  Mbox0_write          = 0x20,
};

enum
{
  Mbox_status_bit_read_wait = 30,
  Mbox_status_bit_send_wait = 31,
};

void
Bcm2835_mbox::status_wait_bit(unsigned bit)
{
  l4_uint32_t status;
  do
    {
      status = _regs[Mbox0_status];
      Util::busy_wait_us(200);
    }
  while (status & (1 << bit));
}

Bcm2835_mbox::Bcm2835_mbox(L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma)
: _data("bcm2835_mbox_mem", 4096, dma,
        L4Re::Dma_space::Direction::Bidirectional,
        L4Re::Rm::F::Cache_uncached)
{
  if (_data.pget() > 0x3fffffff || _data.pget() + _data.size() > 0x40000000)
    L4Re::throw_error_fmt(-L4_EINVAL,
                          "bcm2835 mbox DMA memory at %08llx-%08llx beyond 1GB",
                          _data.pget(), _data.pget() + _data.size());
  if (_data.pget() & 0xf)
    L4Re::throw_error(-L4_ENOMEM, "bcm2835 mbox DMA memory not aligned");

  auto vbus = L4Re::chkcap(L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus"),
                           "Get 'vbus' capability.", -L4_ENOENT);

  L4vbus::Device mbox;
  l4vbus_device_t devinfo;
  L4Re::chksys(vbus->root().device_by_hid(&mbox, "BCM2835_mbox",
                                          L4VBUS_MAX_DEPTH, &devinfo),
               "Locate BCM2835_mbox device on vbus.");

  for (unsigned i = 0; i < devinfo.num_resources; ++i)
    {
      l4vbus_resource_t res;
      L4Re::chksys(mbox.get_resource(i, &res), "Get mbox device info.");
      if (res.type == L4VBUS_RESOURCE_MEM)
        {
          _regs = new Hw::Mmio_map_register_block<32>(mbox.bus_cap(), res.start);
          _data_virt = _data.get<void *>();
          _data_phys = _data.pget(); // no DMA offset!
          return;
        }
    }

  L4Re::throw_error(-L4_ENOENT, "Invalid resources for mbox device.");
}

void
Bcm2835_mbox::send_mail(l4_uint32_t letter, Chan channel)
{
  if (letter & 0xf)
    L4Re::throw_error(-L4_EINVAL, "Send_mail: No place for channel in `letter`");
  if (channel > Chan::Max)
    L4Re::throw_error(-L4_EINVAL, "Send_mail: Wrong channel");

  status_wait_bit(Mbox_status_bit_send_wait);
  _regs[Mbox0_write] = letter | static_cast<l4_uint32_t>(channel);
}

l4_uint32_t
Bcm2835_mbox::read_mail(Chan channel)
{
  if (channel > Chan::Max)
    return 0;

  l4_uint32_t letter;
  do
    {
      status_wait_bit(Mbox_status_bit_read_wait);
      letter = _regs[Mbox0_read];
    }
  while (static_cast<Chan>(letter & 0xf) != channel);
  return letter & ~0xf;
}

/**
 * Send message to mailbox channel `Chan::Property`.
 *
 * \param[in,out] msg  The message to send. The first word contains the size.
 */
void
Bcm2835_mbox::send(l4_uint32_t *msg)
{
  if (busy)
    L4Re::throw_error(-L4_EBUSY, "bcm2835 mbox busy");
  l4_uint32_t size = msg[0];
  if (size < (2 + 3) * 4)
    L4Re::throw_error(-L4_EINVAL, "bcm2835 mbox message too short");

  // protect against concurrency
  busy = true;

  // For 0xc0000000 see
  // https://lore.kernel.org/linux-arm-kernel/87zj584boh.fsf@eliezer.anholt.net/T/
  // Actually it's dma-range.
  if (trace2.is_active())
    {
      using namespace std;
      ostringstream oss;
      for (unsigned i = 0; i < size/4; ++i)
        oss << hex << setw(8) << setfill('0') << msg[i] << " ";
      printf("Mailbox: Send %s\n", oss.str().c_str());
    }

  memcpy(_data_virt, msg, size);
  send_mail(_data_phys, Chan::Property);
  read_mail(Chan::Property);
  memcpy(msg, _data_virt, size);
  busy = false;

  if (Hdr::Status{msg[1]} != Hdr::Status::Success)
    {
      using namespace std;
      ostringstream oss;
      for (unsigned i = 0; i < size/4; ++i)
        oss << hex << setw(8) << setfill('0') << msg[i] << " ";
      warn.printf("Send: Got %s\n", oss.str().c_str());
      L4Re::throw_error(-L4_EINVAL, "Couldn't get board revision at FW");
    }
}
