# Proxmox Blackwell GPU Passthrough Fix

A fix for passing an NVIDIA Blackwell-generation GPU (RTX 5070 / 5070 Ti / 5080 /
5090, GB20x chips) through to a Proxmox VE virtual machine.

Blackwell GPUs set a firmware flag that the Linux kernel's IOMMU code checks
before it lets `vfio-pci` claim the device. The check blocks passthrough.
This repo has the kernel module that clears the flag, the systemd service
that loads it at boot, and the full write-up of how it works.

Tested on Proxmox VE 9.2, kernel `7.0.6-2-pve`, RTX 5070 Ti.

---

## What This Fixes

Proxmox bug [#7374](https://bugzilla.proxmox.com/show_bug.cgi?id=7374).

Two separate problems block Blackwell GPU passthrough. Both need a fix.

### Problem 1 — IOMMU group type: `identity`

On some AMD systems, the IOMMU driver puts the GPU's IOMMU group in
`identity` mode. The `vfio_iommu_type1` driver cannot manage a group in that
mode, so attaching the GPU to a VM fails with:

```
vfio 0000:01:00.0: failed to setup container for group 13: Failed to set group container: Invalid argument
```

**Fix:** switch the group to DMA mode before the VM starts:
```bash
echo DMA > /sys/kernel/iommu_groups/13/type
```

### Problem 2 — the `require_direct` firmware flag (Blackwell-specific)

Blackwell GPUs (GB202/GB203/GB205 dies — RTX 5070, 5070 Ti, 5080, 5090) carry
a firmware flag. The flag tells the kernel's IOMMU core that the device
needs a 1:1 ("direct") memory mapping. When the flag is set,
`drivers/iommu/iommu.c` in the Linux kernel refuses to attach the device to
any other kind of domain. `vfio-pci` needs exactly that kind of attach to
hand the GPU to a VM, so the refusal blocks passthrough.

This check lives in upstream kernel code, not in the NVIDIA driver. It
affects every current Proxmox kernel. Older NVIDIA generations (Ada
Lovelace / RTX 4000 and earlier) do not set this flag. This problem is
Blackwell-only.

**Fix:** a small kernel module clears `require_direct` back to 0 for the
GPU's PCI devices, right after boot and before the VM starts.

---

## How the Fix Works

`scripts/vfio_blackwell_fix.c` is a tiny standalone kernel module. It does
one thing and then goes idle:

1. At `module_init`, it looks up the GPU's two PCI functions (GPU + its HDMI
   audio device) by bus address.
2. For each one, it reaches into the kernel's own `struct dev_iommu` — a
   public, stable kernel structure, not a private or reverse-engineered one —
   and sets `require_direct = 0`.
3. It logs what it did. It then goes idle until the system shuts down.
   `rmmod` unloads it cleanly.

**Why the timing matters:** the module must run *after* the kernel's IOMMU
core has attached `dev->iommu` to the GPU (this happens during early boot)
but *before* `vfio-pci` tries to claim the device (this happens when the VM
starts). `systemd/vfio-blackwell-fix.service` enforces that order:

```ini
After=systemd-modules-load.service
Before=pve-guests.service
```

`systemd-modules-load.service` is what loads the base VFIO modules
(`vfio`, `vfio_pci`, `vfio_iommu_type1`) at boot. `pve-guests.service` is
what auto-starts VMs. This fix's service sits between them, so by the time
any VM tries to start, both the IOMMU group has been switched to DMA mode
and `require_direct` has already been cleared.

---

## How It Loads on Boot

No Python script, no daemon — this is a one-shot kernel-level fix, done twice:

1. **`systemd/vfio-blackwell-fix.service`** — a `oneshot` systemd unit,
   `enabled` so it runs on every boot:
   - `ExecStart` — flips the GPU's IOMMU group to DMA mode
     (`echo DMA > /sys/kernel/iommu_groups/<N>/type`)
   - `ExecStartPost` — loads the kernel module with `modprobe vfio_blackwell_fix`,
     which clears `require_direct` for both GPU PCI functions
   - `RemainAfterExit=yes` — systemd shows it as "active" once the one-shot
     work is done, which is correct for this unit type

2. **`/etc/modules-load.d/vfio-fix.conf`** also lists `vfio_blackwell_fix`,
   as a second, redundant load path — belt and suspenders in case the
   systemd unit is ever disabled by accident. Either one loading the module
   is enough.

Confirm it ran, after any boot:
```bash
systemctl status vfio-blackwell-fix.service   # oneshot: "inactive (dead)" is
                                               # normal AFTER it completes
lsmod | grep vfio_blackwell_fix               # module should be listed
dmesg | grep vfio_fix
# vfio_fix: 0000:01:00.0 require_direct 1 -> 0
# vfio_fix: Blackwell passthrough unblocked
```

---

## Repo Layout

```
scripts/vfio_blackwell_fix.c       kernel module source
scripts/Makefile                   builds the module against the running kernel
systemd/vfio-blackwell-fix.service loads the fix at boot, before VMs start
.hermes-skill/                     full step-by-step knowledge base (Hermes AI
                                    agent skill format) — host setup, build/
                                    install steps, VM config, verification
                                    checklist, and a from-source kernel-patch
                                    alternative
```

For the full walkthrough (installing kernel headers, editing GRUB, VM
config for passthrough, NVIDIA driver install inside the guest,
verification steps, and how to revert), see the files under
`.hermes-skill/references/`.

---

## Quick Start

```bash
# 1. Adjust the two BDF addresses in scripts/vfio_blackwell_fix.c
#    and the IOMMU group number in systemd/vfio-blackwell-fix.service
#    to match your hardware (see .hermes-skill/references/host-setup.md
#    for how to find these).

# 2. Build
apt install proxmox-headers-$(uname -r)
cd scripts && make

# 3. Install
mkdir -p /lib/modules/$(uname -r)/extra
cp vfio_blackwell_fix.ko /lib/modules/$(uname -r)/extra/
depmod -a

# 4. Install the boot service
cp ../systemd/vfio-blackwell-fix.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable vfio-blackwell-fix.service

# 5. Test now, without rebooting
echo DMA > /sys/kernel/iommu_groups/<N>/type
modprobe vfio_blackwell_fix
dmesg | grep vfio_fix
```

**After every Proxmox kernel update**, the module must be rebuilt against
the new kernel headers — see `.hermes-skill/references/kernel-build.md` for
the exact steps and a from-source kernel-patch alternative that removes the
need to rebuild at all.

---

## License

MIT — see `LICENSE`.
