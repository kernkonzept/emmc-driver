/*
 * Copyright (C) 2020-2021 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *            Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *            Jakub Jermar <jakub.jermar@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <getopt.h>
#include <map>

#include <l4/sys/factory>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci>
#include <l4/vbus/vbus_interfaces.h>
#include <l4/libblock-device/block_device_mgr.h>
#include <terminate_handler-l4>

#include "cpg.h"
#include "device.h"
#include "debug.h"
#include "mmc.h"
#include "util.h"

namespace {

static Dbg warn(Dbg::Warn, "main");
static Dbg info(Dbg::Info, "main");
static Dbg trace(Dbg::Trace, "main");

static Emmc::Mmc::Reg_ecsd::Ec196_device_type device_type_disable(0);
static int max_seg = 64;

// Don't specify the partition number when creating a client. The partition is
// already specified by setting `device` to the GUID of the corresponding GPT
// partition. To access the entire device, use the PSN (product serial number)
// of the device.
//
// See Device::match_hid() for matching the whole device. This function is
// invoked if libblock-device couldn't match the device name to any GUID.
//
// Specifying PSN:partition would work as well.
enum { No_partno = -1 };

static char const *usage_str =
"Usage: %s [-vq] --client CAP <client parameters>]\n"
"\n"
"Options:\n"
" -v                   Verbose mode\n"
" -q                   Be quiet\n"
" --disable-mode MODE  Disable a certain eMMC mode (can be used more than once)\n"
"                      (MODE is hs26|hs52|hs200|hs400)\n"
" --client CAP         Add a static client via the CAP capability\n"
" --ds-max NUM         Specify maximum number of dataspaces the client can register\n"
" --max-seg NUM        Specify maximum number of segments one vio request can have\n"
" --readonly           Only allow read-only access to the device\n"
" --dma-map-all        Map the entire client dataspace permanently\n";

using Emmc_client_type = Block_device::Virtio_client<Emmc::Base_device>;

struct Emmc_device_factory
{
  using Device_type = Emmc::Base_device;
  using Client_type = Emmc_client_type;
  using Part_device = Emmc::Part_device;

  static cxx::unique_ptr<Client_type>
  create_client(cxx::Ref_ptr<Device_type> const &dev, unsigned numds,
                bool readonly)
  {
    return cxx::make_unique<Client_type>(dev, numds, readonly);
  }

  static cxx::Ref_ptr<Device_type>
  create_partition(cxx::Ref_ptr<Device_type> const &dev, unsigned partition_id,
                   Block_device::Partition_info const &pi)
  {
    return cxx::Ref_ptr<Device_type>(new Part_device(dev, partition_id, pi));
  }
};

using Base_device_mgr = Block_device::Device_mgr<Emmc::Base_device,
                                                 Emmc_device_factory>;

class Blk_mgr
: public Base_device_mgr,
  public L4::Epiface_t<Blk_mgr, L4::Factory>
{
  class Deletion_irq : public L4::Irqep_t<Deletion_irq>
  {
  public:
    void handle_irq()
    { _parent->check_clients(); }
    Deletion_irq(Blk_mgr *parent) : _parent{parent} {}
  private:
    Blk_mgr *_parent;
  };

public:
  Blk_mgr(L4Re::Util::Object_registry *registry)
  : Base_device_mgr(registry),
    _del_irq(this)
  {
    auto c = L4Re::chkcap(registry->register_irq_obj(&_del_irq),
                          "Creating IRQ for IPC gate deletion notifications.");
    L4Re::chksys(L4Re::Env::env()->main_thread()->register_del_irq(c),
                 "Registering deletion IRQ at the thread.");
  }

  long op_create(L4::Factory::Rights, L4::Ipc::Cap<void> &res, l4_umword_t,
                 L4::Ipc::Varg_list_ref valist)
  {
    trace.printf("Client requests connection.\n");

    // default values
    std::string device;
    int num_ds = 2;
    bool readonly = false;
    bool dma_map_all = false;

    for (L4::Ipc::Varg p: valist)
      {
        if (!p.is_of<char const *>())
          {
            warn.printf("String parameter expected.\n");
            return -L4_EINVAL;
          }

        if (parse_string_param(p, "device=", &device))
          {
            std::transform(device.begin(), device.end(), device.begin(),
                           [](unsigned char c){ return std::toupper(c); });
            continue;
          }
        if (parse_int_param(p, "ds-max=", &num_ds))
          {
            if (num_ds <= 0 || num_ds > 256) // sanity check with arbitrary limit
              {
                warn.printf("Invalid range for parameter 'ds-max'. "
                            "Number must be between 1 and 256.\n");
                return -L4_EINVAL;
              }
            continue;
          }
        if (strncmp(p.value<char const *>(), "readonly", p.length()) == 0)
          readonly = true;
        if (strncmp(p.value<char const *>(), "dma-map-all", p.length()) == 0)
          dma_map_all = true;
      }

    if (device.empty())
      {
        warn.printf("Parameter 'device=' not found. Device UUID is required.\n");
        return -L4_EINVAL;
      }

    L4::Cap<void> cap;
    int ret = create_dynamic_client(device, No_partno, num_ds, &cap, readonly,
                                    [dma_map_all, device](Emmc::Base_device *b)
      {
        Dbg(Dbg::Warn).printf("%s for device '%s'.\033[m\n",
                              dma_map_all ? "\033[31;1mDMA-map-all enabled"
                                          : "\033[32mDMA-map-all disabled",
                              device.c_str());
        if (auto *pd = dynamic_cast<Emmc::Part_device *>(b))
          pd->set_dma_map_all(dma_map_all);
        else
          b->set_dma_map_all(dma_map_all);
      });
    if (ret >= 0)
      {
        res = L4::Ipc::make_cap(cap, L4_CAP_FPAGE_RWSD);
        L4::cap_cast<L4::Kobject>(cap)->dec_refcnt(1);
      }

    return (ret == -L4_ENODEV && _scan_in_progress) ? -L4_EAGAIN : ret;
  }

  void scan_finished()
  { _scan_in_progress = false; }


private:
  static bool parse_string_param(L4::Ipc::Varg const &param, char const *prefix,
                                 std::string *out)
  {
    l4_size_t headlen = strlen(prefix);

    if (param.length() < headlen)
      return false;

    char const *pstr = param.value<char const *>();

    if (strncmp(pstr, prefix, headlen) != 0)
      return false;

    *out = std::string(pstr + headlen, strnlen(pstr, param.length()) - headlen);

    return true;
  }

  static bool parse_int_param(L4::Ipc::Varg const &param, char const *prefix,
                              int *out)
  {
    l4_size_t headlen = strlen(prefix);

    if (param.length() < headlen)
      return false;

    char const *pstr = param.value<char const *>();

    if (strncmp(pstr, prefix, headlen) != 0)
      return false;

    std::string tail(pstr + headlen, param.length() - headlen);

    char *endp;
    long num = strtol(tail.c_str(), &endp, 10);

    if (num < INT_MIN || num > INT_MAX || *endp != '\0')
      {
        warn.printf("Bad parameter '%s'. Number required.\n", prefix);
        L4Re::throw_error(-L4_EINVAL, "Parsing integer");
      }

    *out = num;

    return true;
  }

  Deletion_irq _del_irq;
  bool _scan_in_progress = true;
};

struct Client_opts
{
  bool add_client(Blk_mgr *blk_mgr)
  {
    if (capname)
      {
        if (!device)
          {
            Err().printf("No device for client '%s' given. "
                         "Please specify a device.\n", capname);
            return false;
          }

        auto cap = L4Re::Env::env()->get_cap<L4::Rcv_endpoint>(capname);
        if (!cap.is_valid())
          {
            Err().printf("Client capability '%s' no found.\n", capname);
            return false;
          }

        // Copy parameters for lambda capture. The object itself is ephemeral!
        const char *dev = device;
        bool map_all = dma_map_all;
        blk_mgr->add_static_client(cap, device, No_partno, ds_max, readonly,
                                   [dev, map_all](Emmc::Base_device *b)
         {
           Dbg(Dbg::Warn).printf("%s for device '%s'\033[m\n",
                                 map_all ? "\033[31;1mDMA-map-all enabled"
                                         : "\033[32mDMA-map-all disabled",
                                 dev);
           if (auto *pd = dynamic_cast<Emmc::Part_device *>(b))
             pd->set_dma_map_all(map_all);
           else
             b->set_dma_map_all(map_all);
         });
      }

    return true;
  }

  const char *capname = nullptr;
  const char *device = nullptr;
  int ds_max = 2;
  bool readonly = false;
  bool dma_map_all = false;
};

static Block_device::Errand::Errand_server server;
static Blk_mgr drv(server.registry());
static unsigned devices_in_scan = 0;
static unsigned device_nr = 0;

static Rcar3_cpg *cpg;

static int
parse_args(int argc, char *const *argv)
{
  int debug_level = 1;

  enum
  {
    OPT_MAX_SEG,

    OPT_CLIENT,
    OPT_DEVICE,
    OPT_DS_MAX,
    OPT_READONLY,
    OPT_DMA_MAP_ALL,
    OPT_DISABLE_MODE,
  };

  static struct option const loptions[] =
  {
    // global options
    { "verbose",        no_argument,            NULL,   'v' },
    { "quiet",          no_argument,            NULL,   'q' },
    { "disable-mode",   required_argument,      NULL,   OPT_DISABLE_MODE },
    { "max-seg",        required_argument,      NULL,   OPT_MAX_SEG },

    // per-client options
    { "client",         required_argument,      NULL,   OPT_CLIENT },
    { "device",         required_argument,      NULL,   OPT_DEVICE },
    { "ds-max",         required_argument,      NULL,   OPT_DS_MAX },
    { "readonly",       no_argument,            NULL,   OPT_READONLY },
    { "dma-map-all",    no_argument,            NULL,   OPT_DMA_MAP_ALL },
    { 0,                0,                      NULL,   0, },
  };

  Client_opts opts;
  for (;;)
    {
      int opt = getopt_long(argc, argv, "vq", loptions, NULL);
      if (opt == -1)
        {
          if (optind < argc)
            {
              warn.printf("Unknown parameter '%s'\n", argv[optind]);
              warn.printf(usage_str, argv[0]);
              return -1;
            }
          break;
        }

      switch (opt)
        {
        case 'v':
          debug_level <<= 1;
          ++debug_level;
          break;
        case 'q':
          debug_level = 0;
          break;
        case OPT_DISABLE_MODE:
          if (!strcmp(optarg, "hs26"))
            device_type_disable.hs26() = 1;
          else if (!strcmp(optarg, "hs52"))
            device_type_disable.hs52() = 1;
          else if (!strcmp(optarg, "hs52_ddr"))
            {
              device_type_disable.hs52_ddr_18() = 1;
              device_type_disable.hs52_ddr_12() = 1;
            }
          else if (!strcmp(optarg, "hs200"))
            {
              device_type_disable.hs200_sdr_18() = 1;
              device_type_disable.hs200_sdr_12() = 1;
            }
          else if (!strcmp(optarg, "hs400"))
            {
              device_type_disable.hs400_ddr_18() = 1;
              device_type_disable.hs400_ddr_12() = 1;
            }
          else
            {
              warn.printf("Invalid parameter\n\n");
              warn.printf(usage_str, argv[0]);
            }
          break;
        case OPT_MAX_SEG:
          max_seg = atoi(optarg);
          break;

        case OPT_CLIENT:
          if (!opts.add_client(&drv))
            return 1;
          opts = Client_opts();
          opts.capname = optarg;
          break;
        case OPT_DEVICE:
          opts.device = optarg;
          break;
        case OPT_DS_MAX:
          opts.ds_max = atoi(optarg);
          break;
        case OPT_READONLY:
          opts.readonly = true;
          break;
        case OPT_DMA_MAP_ALL:
          opts.dma_map_all = true;
          break;
        default:
          warn.printf(usage_str, argv[0]);
          return -1;
        }
    }

  if (!opts.add_client(&drv))
    return -1;

  Dbg::set_level(debug_level);
  return optind;
}

static void
device_scan_finished()
{
  if (--devices_in_scan > 0)
    return;

  drv.scan_finished();
  if (!server.registry()->register_obj(&drv, "svr").is_valid())
    warn.printf("Capability 'svr' not found. No dynamic clients accepted.\n");
  else
    trace.printf("Device now accepts new clients.\n");
}

static L4Re::Util::Shared_cap<L4Re::Dma_space>
create_dma_space(L4::Cap<L4vbus::Vbus> bus, long unsigned id)
{
  static std::map<long unsigned, L4Re::Util::Shared_cap<L4Re::Dma_space>> spaces;

  auto ires = spaces.find(id);
  if (ires != spaces.end())
    return ires->second;

  auto dma = L4Re::chkcap(L4Re::Util::make_shared_cap<L4Re::Dma_space>(),
                          "Allocate capability for DMA space.");
  L4Re::chksys(L4Re::Env::env()->user_factory()->create(dma.get()),
               "Create DMA space.");
  L4Re::chksys(
    bus->assign_dma_domain(id, L4VBUS_DMAD_BIND | L4VBUS_DMAD_L4RE_DMA_SPACE,
                           dma.get()),
    "Assignment of DMA domain.");
  spaces[id] = dma;
  return dma;
}

static void
scan_device(L4vbus::Pci_dev const &dev, l4vbus_device_t const &dev_info,
            L4::Cap<L4vbus::Vbus> bus, L4::Cap<L4::Icu> icu)
{
  l4_addr_t mmio_addr = 0;
  int irq_num = 0;
  bool is_irq_level;

  enum Dev_type
  {
    Dev_unknown = 0,
    Dev_qemu_sdhci,     // QEMU SDHCI (PCI)
    Dev_usdhc,          // i.MX8 uSDHC
    Dev_sdhi_emu,       // RCar3 SDHI -- emulator
    Dev_sdhi_rcar3,     // RCar3 SDHI -- bare metal
    Dev_bcm2711,        // Broadcom Bcm2711-emmc2
  };
  Dev_type dev_type = Dev_unknown;

  if (l4vbus_subinterface_supported(dev_info.type, L4VBUS_INTERFACE_PCIDEV))
    {
      l4_uint32_t vendor_device = 0;
      if (dev.cfg_read(0, &vendor_device, 32) != L4_EOK)
        return;

      l4_uint32_t class_code;
      L4Re::chksys(dev.cfg_read(8, &class_code, 32));
      class_code >>= 8;

      info.printf("Found PCI device %04x:%04x (class=%06x).\n",
                  vendor_device & 0xffff, (vendor_device & 0xffff0000) >> 16,
                  class_code);

      // class     = 08 (generic system peripherals)
      // subclass  = 04 (SD host controller)
      // interface = 01 (according to QEMU)
      if (class_code != 0x80501)
        return;

      l4_uint32_t addr;
      L4Re::chksys(dev.cfg_read(0x10, &addr, 32), "Read PCI cfg BAR0.");
      mmio_addr = addr;

      l4_uint32_t cmd;
      L4Re::chksys(dev.cfg_read(0x04, &cmd, 16), "Read PCI cfg command.");
      if (!(cmd & 4))
        {
          trace.printf("Enable PCI bus master.\n");
          cmd |= 4;
          L4Re::chksys(dev.cfg_write(0x04, cmd, 16), "Write PCI cfg command.");
        }

      unsigned char polarity;
      unsigned char trigger;
      irq_num = L4Re::chksys(dev.irq_enable(&trigger, &polarity),
                             "Enable interrupt.");

      is_irq_level = trigger == 0;

      dev_type = Dev_qemu_sdhci;
    }
  else
    {
      if (   dev.is_compatible("fsl,imx8mq-usdhc") == 1
          || dev.is_compatible("fsl,imx8qm-usdhc") == 1
          || dev.is_compatible("fsl,imx7d-usdhc") == 1)
        dev_type = Dev_usdhc;
      else if (dev.is_compatible("renesas,sdhi-r8a7795") == 1)
        dev_type = Dev_sdhi_rcar3;
      else if (dev.is_compatible("renesas,sdhi-r8a7796") == 1)
        dev_type = Dev_sdhi_emu;
      else if (dev.is_compatible("brcm,bcm2711-emmc2") == 1)
        dev_type = Dev_bcm2711;
      else
        return; // no match

      for (unsigned i = 0;
           i < dev_info.num_resources && (!mmio_addr || !irq_num); ++i)
        {
          l4vbus_resource_t res;
          L4Re::chksys(dev.get_resource(i, &res));
          if (res.type == L4VBUS_RESOURCE_MEM)
            {
              if (!mmio_addr)
                mmio_addr = res.start;
            }
          else if (res.type == L4VBUS_RESOURCE_IRQ)
            {
              if (!irq_num)
                irq_num = res.start;
            }
        }

      if (!mmio_addr)
        {
          info.printf("Device '%s' has no MMIO resource.\n", dev_info.name);
          return;
        }

      if (!irq_num)
        {
          info.printf("Device '%s' has no IRQ resource.\n", dev_info.name);
          return;
        }

      is_irq_level = false;
    }

  unsigned long id = -1UL;
  for (auto i = 0u; i < dev_info.num_resources; ++i)
    {
      l4vbus_resource_t res;
      L4Re::chksys(dev.get_resource(i, &res), "Getting resource.");
      if (res.type == L4VBUS_RESOURCE_DMA_DOMAIN)
        {
          id = res.start;
          Dbg::trace().printf("Using device's DMA domain %lu.\n", res.start);
          break;
        }
    }

  if (id == -1UL)
    Dbg::trace().printf("Using VBUS global DMA domain.\n");

  info.printf("Device @ %08lx: %sinterrupt: %d, %s-triggered.\n",
              mmio_addr, dev_type == Dev_qemu_sdhci ? "PCI " : "", irq_num,
              is_irq_level ? "level" : "edge");

  // XXX
  l4_uint32_t host_clock = 400000;
  switch (mmio_addr)
    {
    case 0x30b40000: host_clock = 400000000; break;
    case 0x30b50000: host_clock = 200000000; break;
    case 0x5b010000: host_clock = 396000000; break;
    case 0x5b020000: host_clock = 198000000; break;
    case 0x5b030000: host_clock = 198000000; break;
    case 0xfe340000: host_clock = 100000000; break;
    default:
         if (dev_type == Dev_usdhc)
           L4Re::throw_error(-L4_EINVAL, "Unknown host clock");
         break;
    }
  warn.printf("\033[33mAssuming host clock of %s.\033[m\n",
              Util::readable_freq(host_clock).c_str());

  ++devices_in_scan;
  try
    {
      // Here we can select the proper device type.
      auto iocap = dev.bus_cap();
      L4::Cap<L4Re::Mmio_space> mmio_space = L4::Cap<L4Re::Mmio_space>::Invalid;
      auto dma = create_dma_space(bus, id);
      switch (dev_type)
        {
        case Dev_qemu_sdhci:
        case Dev_usdhc:
          {
            using Type = Emmc::Sdhci::Type;
            Type const type = (dev_type == Dev_usdhc)
                                ? Type::Usdhc
                                : Type::Sdhci;
            drv.add_disk(cxx::make_ref_obj<Emmc::Device<Emmc::Sdhci>>(
                           device_nr++, mmio_addr, iocap, mmio_space, irq_num,
                           is_irq_level, icu, dma, server.registry(),
                           type, host_clock, max_seg, device_type_disable),
                         device_scan_finished);
          }
          break;

        case Dev_bcm2711:
          drv.add_disk(cxx::make_ref_obj<Emmc::Device<Emmc::Sdhci>>(
                         device_nr++, mmio_addr, iocap, mmio_space, irq_num,
                         is_irq_level, icu, dma, server.registry(),
                         Emmc::Sdhci::Type::Iproc, host_clock, max_seg,
                         device_type_disable),
                       device_scan_finished);
          break;

        case Dev_sdhi_emu:
          mmio_space = L4::cap_dynamic_cast<L4Re::Mmio_space>(iocap);
          if (!cpg)
            cpg = new Rcar3_cpg(bus);
          cpg->enable_clock(3, 12);
          cpg->enable_register(Rcar3_cpg::Sd2ckcr, 0x201);
          drv.add_disk(cxx::make_ref_obj<Emmc::Device<Emmc::Sdhi>>(
                         device_nr++, mmio_addr, iocap, mmio_space, irq_num,
                         is_irq_level, icu, dma, server.registry(),
                         Emmc::Sdhi::Type::Sdhi, host_clock, max_seg,
                         device_type_disable),
                       device_scan_finished);
          break;

        case Dev_sdhi_rcar3:
          if (!cpg)
            cpg = new Rcar3_cpg(bus);
          cpg->enable_clock(3, 12);
          cpg->enable_register(Rcar3_cpg::Sd2ckcr, 0x201);
          drv.add_disk(cxx::make_ref_obj<Emmc::Device<Emmc::Sdhi>>(
                         device_nr++, mmio_addr, iocap, mmio_space, irq_num,
                         is_irq_level, icu, dma, server.registry(),
                         Emmc::Sdhi::Type::Sdhi, host_clock, max_seg,
                         device_type_disable),
                       device_scan_finished);
          break;

        default:
          L4Re::throw_error(-L4_EINVAL, "Unhandled switch case");
        }
    }
  catch (L4::Runtime_error const &e)
    {
      warn.printf("%s: %s. Skipping.\n", e.str(), e.extra_str());
    }
}

static void
device_discovery(L4::Cap<L4vbus::Vbus> bus, L4::Cap<L4::Icu> icu)
{
  info.printf("Starting device discovery.\n");

  L4vbus::Pci_dev child;
  l4vbus_device_t di;
  auto root = bus->root();

  // make sure that we don't finish device scan before the while loop is done
  ++devices_in_scan;

  while (root.next_device(&child, L4VBUS_MAX_DEPTH, &di) == L4_EOK)
    {
      trace.printf("Scanning child 0x%lx (%s).\n", child.dev_handle(), di.name);
      scan_device(child, di, bus, icu);
    }

  // marks the end of the device detection loop
  device_scan_finished();

  info.printf("All devices scanned.\n");
}

static void
setup_hardware()
{
  auto vbus = L4Re::chkcap(L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus"),
                           "Get 'vbus' capability.", -L4_ENOENT);

  L4vbus::Icu icudev;
  L4Re::chksys(vbus->root().device_by_hid(&icudev, "L40009"),
               "Look for ICU device.");
  auto icu = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Icu>(),
                          "Allocate ICU capability.");
  L4Re::chksys(icudev.vicu(icu), "Request ICU capability.");

  device_discovery(vbus, icu);
}

} // namespace

int
main(int argc, char *const *argv)
{
  Dbg::set_level(3);

  int arg_idx = parse_args(argc, argv);
  if (arg_idx < 0)
    return arg_idx;

  info.printf("Emmc driver says hello.\n");

  info.printf("TSC frequency of %lluHz\n", Util::freq_tsc());

  Block_device::Errand::set_server_iface(&server);
  setup_hardware();

  trace.printf("Entering server loop.\n");
  server.loop();
}
