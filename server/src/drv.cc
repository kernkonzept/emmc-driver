/*
 * Copyright (C) 2024 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include "drv.h"

namespace Emmc {

void
Drv_base::delay(unsigned ms)
{
  stats_wait_start();
  l4_ipc_sleep_ms(ms);
  stats_wait_done();
}

void
Drv_base::setup_bounce_buffer(L4::Cap<L4Re::Dataspace> cap,
                              L4Re::Util::Shared_cap<L4Re::Dma_space> dma,
                              unsigned max_seg, Dbg const &dbg)
{
  l4_size_t size = cap->size();
  if (size < (64 << 10))
    L4Re::throw_error(-L4_EINVAL, "Bounce buffer smaller than 64K");

  L4Re::Dma_space::Dma_addr phys;
  L4Re::chksys(dma->map(L4::Ipc::make_cap_rw(cap), 0, &size,
                        L4Re::Dma_space::Attributes::None,
                        L4Re::Dma_space::Direction::Bidirectional,
                        &phys),
               "Resolve physical address of bounce buffer");

  if (size != cap->size())
    L4Re::throw_error(-L4_EINVAL, "Bounce buffer contiguous");

  auto rm = L4Re::Env::env()->rm();
  L4Re::chksys(rm->attach(&_bb_region, size,
                          L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW
                          | L4Re::Rm::F::Cache_normal,
                          L4::Ipc::make_cap_rw(cap), 0, L4_PAGESHIFT),
               "Attach bounce buffer");

  // We should have at least one page per segment
  if (size / max_seg < L4_PAGESIZE)
    L4Re::throw_error(-L4_EINVAL, "Bounce buffer is too small for max seg count");

  _bb_size = size;
  _bb_phys = phys;
  _bb_virt = _bb_region.get();

  if (!dma_accessible(phys, size))
    L4Re::throw_error_fmt(-L4_EINVAL,
                          "Bounce buffer at %08llx-%08llx not accessible by DMA",
                          phys, phys + size);

  dbg.printf("\033[31;1mUsing bounce buffer of %s @ %08llx if required.\033[m\n",
             Util::readable_size(size).c_str(), phys);
}

}
