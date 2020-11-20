# eMMC driver (#l4re_servers_emmc_driver}

The eMMC driver is a driver for PCI Express eMMC controllers.

## Starting the service

The eMMC driver can be started with Lua like this:

    local emmc_bus = L4.default_loader:new_channel();
    L4.default_loader:start({
      caps = {
        vbus = vbus_emmc,
        svr = emmc_bus:svr(),
      },
    },
    "rom/emmc-drv");

First, an IPC gate (`emmc_bus`) is created which is used between the eMMC
driver and a client to request access to a particular disk or partition. The
server side is assigned to the mandatory `svr` capability of the eMMC driver.
See the sector below on how to configure access to a disk or partition.

The eMMC driver needs access to a virtual bus capability (`vbus`). On the
virtual bus the eMMC driver searches for eMMC compliant storage controllers.
Please see io's documentation about how to setup a virtual bus.

### Options

In the example above the eMMC driver is started in its default configuration.
To customize the configuration of the eMMC driver it accepts the following
command line options:

* `-v`

  Enable verbose mode. You can repeat this option up to three times to increase
  verbosity up to trace level.

* `-q`

  This option enables the quiet mode. All output is silenced.

* `--client <cap_name>`

  This option starts a new static client option context. The following
  `device`, `ds-max` and `readonly` options belong to this context until a new
  client option context is created.

  The option parameter is the name of a local IPC gate capability with server
  rights.

* `--device <UUID>`

  This option denotes the partition UUID of the partition to be exported for
  the client specified in the preceding `client` option.

* `--ds-max <max>`

  This option sets the upper limit of the number of dataspaces the client is
  able to register with the eMMC driver for virtio DMA.

* `--readonly`

  This option sets the access to disks or partitions to read only for the
  preceding `client` option.

* `--disable-mode <mode>`

  This option allows to disable certain eMMC modes from autodetection. The
  modes `hs26`, `hs52`, `hs52_ddr`, `hs200`, and `hs400` are determined.


## Connecting a client

Prior to connecting a client to a virtual block session it has to be created
using the following Lua function. It has to be called on the client side of the
IPC gate capability whose server side is bound to the eMMC driver.

    create(obj_type, "device=<UUID>", "ds-max=<max>")

* `obj_type`

  The type of object that should be created by the driver. The type must be a
  positive integer. Currently the following objects are supported:
  * `0`: Virtio block host

* `"device=<UUID>"`

  This string denotes a partition UUID the client wants to be exported via the
  Virtio block interface.

* `"ds-max=<max>"`

  Specifies the upper limit of the number of dataspaces the client is allowed
  to register with the eMMC driver for virtio DMA.

If the `create()` call is successful a new capability which references an eMMC
virtio driver is returned. A client uses this capability to communicate with
the eMMC driver using the Virtio block protocol.


## Examples

A couple of examples on how to request different disks or partitions are listed
below.

* Request a partition with the given UUID

      vda1 = emmc_bus:create(0, "ds-max=5", "device=AFFA05B0-9379-480E-B9C6-5FF57FB1D194")

* A more elaborate example with a static client. The client uses the client
  side of the `emmc_cl1` capability to communicate with the eMMC driver.

      local emmc_cl1 = L4.default_loader:new_channel();
      local emmc_bus = L4.default_loader:new_channel();
      L4.default_loader:start({
        caps = {
          vbus = vbus_emmc,
          svr = emmc_bus:svr(),
          cl1 = emmc_cl1:svr(),
        },
      },
      "rom/emmc-drv --client cl1 --device 88E59675-4DC8-469A-98E4-B7B021DC7FBE --ds-max 5");

* Accessing a device from QEMU:

The file `pcie-ecam.io` contains an IO config file which is able to use the
QEMU PCI controller to search for attached eMMC devices.

