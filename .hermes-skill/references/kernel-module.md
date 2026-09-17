# Blackwell Kernel Module — vfio_blackwell_fix

Source lives at `/root/vfio-fix/`. **Keep the source forever — must rebuild after every kernel update.**

---

## How the Module Works

`vfio_blackwell_fix` is a standalone one-shot kernel module. At load time (`module_init`):

1. Calls `pci_get_domain_bus_and_slot()` to get a pointer to `struct pci_dev` for each BDF
2. Walks `pdev->dev.iommu` (a `struct dev_iommu *` attached by the IOMMU core at boot)
3. Sets `require_direct = 0` — then goes dormant forever, just sitting in `lsmod`

**Timing:** Must run after IOMMU core has attached `dev->iommu` (boot) but before `vfio-pci`
calls `iommu_group_claim_dma_owner()` (when VM starts). The systemd service ordering enforces this.

`struct dev_iommu` is a public kernel header (`include/linux/iommu.h`) — accessing it from a module is legal and stable.

---

## Module Source — `/root/vfio-fix/vfio_blackwell_fix.c`

```c
// Clears the require_direct IOMMU flag for Blackwell GPU passthrough.
// Handles both GPU function 0 (video) and function 1 (audio).
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include <linux/device.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Clear IOMMU require_direct for Blackwell GPU passthrough");

static void clear_device(const char *bdf)
{
    struct pci_dev *pdev;
    unsigned int domain, bus, slot, func;

    if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4)
        return;

    pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
    if (!pdev) {
        pr_warn("vfio_fix: %s not found\n", bdf);
        return;
    }

    if (pdev->dev.iommu) {
        pr_info("vfio_fix: %s require_direct %u -> 0\n",
                bdf, pdev->dev.iommu->require_direct);
        pdev->dev.iommu->require_direct = 0;
    } else {
        pr_warn("vfio_fix: %s has no iommu data\n", bdf);
    }
    pci_dev_put(pdev);
}

static int __init vfio_fix_init(void)
{
    clear_device("0000:01:00.0");
    clear_device("0000:01:00.1");
    pr_info("vfio_fix: Blackwell passthrough unblocked\n");
    return 0;
}

static void __exit vfio_fix_exit(void)
{
    pr_info("vfio_fix: unloaded\n");
}

module_init(vfio_fix_init);
module_exit(vfio_fix_exit);
```

**If your GPU is at a different BDF**, change the two `clear_device("0000:01:00.x")` calls to match.

---

## Makefile — `/root/vfio-fix/Makefile`

```makefile
obj-m += vfio_blackwell_fix.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

---

## Build and Install

```bash
# 1. Install headers for the current kernel
apt install proxmox-headers-$(uname -r)

# 2. Build
cd /root/vfio-fix
make clean && make
# Produces: vfio_blackwell_fix.ko

# 3. Install
mkdir -p /lib/modules/$(uname -r)/extra
cp vfio_blackwell_fix.ko /lib/modules/$(uname -r)/extra/
depmod -a

# 4. Verify it was registered
modinfo vfio_blackwell_fix
```

---

## Test After Build (before relying on the service)

```bash
# Unload current instance if already loaded
rmmod vfio_blackwell_fix 2>/dev/null || true

# Load directly from compiled path (bypasses depmod — good for one-off testing)
insmod /root/vfio-fix/vfio_blackwell_fix.ko

# Verify it cleared the flags
dmesg | tail -5
# Expect: vfio_fix: 0000:01:00.0 require_direct 1 -> 0
# Expect: vfio_fix: Blackwell passthrough unblocked

# Confirm VM starts and GPU resets
qm start 888
dmesg | grep "vfio-pci.*reset"
# Expect: vfio-pci 0000:01:00.0: reset done
# Expect: vfio-pci 0000:01:00.1: reset done
```

`insmod` loads from a file path. `modprobe` loads by name from the installed location. Use `insmod` when testing a freshly built `.ko` before proper installation.

---

## Systemd Service — `/etc/systemd/system/vfio-blackwell-fix.service`

```ini
[Unit]
Description=VFIO Blackwell GPU passthrough fix
After=systemd-modules-load.service
Before=pve-guests.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'echo DMA > /sys/kernel/iommu_groups/13/type'
ExecStartPost=/sbin/modprobe vfio_blackwell_fix

[Install]
WantedBy=multi-user.target
```

Change `13` to your GPU's IOMMU group number.

Enable:
```bash
systemctl daemon-reload
systemctl enable vfio-blackwell-fix.service
```

**Note:** Service may show `Active: inactive (dead)` after reboot — this is normal for `Type=oneshot`.
It means the task completed. Confirm the module is loaded with:
```bash
lsmod | grep vfio_blackwell_fix
```

---

## Load Fix for Current Boot (without rebooting)

```bash
echo DMA > /sys/kernel/iommu_groups/13/type
modprobe vfio_blackwell_fix
dmesg | grep vfio_fix
```

---

## Files on This Server

| Path | Purpose |
|---|---|
| `/root/vfio-fix/vfio_blackwell_fix.c` | Module source — **keep forever, rebuild against new kernels** |
| `/root/vfio-fix/Makefile` | Module build file |
| `/root/vfio-fix/vfio_blackwell_fix.ko` | Compiled module (current kernel) |
| `/lib/modules/7.0.6-2-pve/extra/vfio_blackwell_fix.ko` | Installed module |
| `/etc/systemd/system/vfio-blackwell-fix.service` | Persistent boot fix service |
