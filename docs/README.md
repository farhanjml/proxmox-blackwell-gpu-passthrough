# Detailed Reference Docs

Deep-dive material behind the top-level [README](../README.md). The
top-level README is a complete step-by-step walkthrough on its own — start
there. Come here when you need more detail on one specific part.

**This deployment's specifics** (adapt to your own hardware):
- GPU: `0000:01:00.0` (video, `10de:2c05`) + `0000:01:00.1` (audio, `10de:22e9`)
- IOMMU group: 13
- VM: ID 888, 32 GB RAM, 4 cores, q35 machine type, OVMF (UEFI) BIOS
- Kernel: `7.0.6-2-pve` (pinned)
- Kernel pin file: `/etc/kernel/proxmox-boot-pin`

## Root Cause — Two Blockers

**Both must be fixed for passthrough to work.**

### Blocker 1 — IOMMU group type: identity (generic AMD issue)
AMD IOMMU assigns some groups to `identity` mode. `vfio_iommu_type1` cannot manage identity-mapped groups:
```
vfio 0000:01:00.0: failed to setup container for group 13: Failed to set group container: Invalid argument
```
**Fix:** `echo DMA > /sys/kernel/iommu_groups/13/type`

### Blocker 2 — `require_direct` firmware flag (Blackwell-specific)
Blackwell GPUs (GB202/GB203/GB205 — RTX 5070, 5070 Ti, 5080, 5090) set a firmware flag that
tells the kernel IOMMU core to block non-identity domain attachment. This lives in
`drivers/iommu/iommu.c` (upstream kernel, not NVIDIA code). All current Proxmox kernels are affected.
Tracked as **Proxmox bug [#7374](https://bugzilla.proxmox.com/show_bug.cgi?id=7374)**. Ada Lovelace (RTX 4000) and earlier do NOT have this flag.
**Fix:** Clear `dev->iommu->require_direct = 0` for both GPU PCI functions via a kernel module.

---

## Files In This Folder

- `host-setup.md` — Host VFIO config files, GRUB, modprobe, initramfs
- `kernel-module.md` — Full module source, Makefile, build/install/test steps
- `vm-config.md` — VM config, EFI disk, Secure Boot, NVIDIA driver install
- `verification.md` — Verification checklist, revert procedure, dead ends
- `kernel-build.md` — Building a patched Proxmox kernel (permanent alternative)

---

## Diagnose Your Hardware

```bash
# Confirm IOMMU is active
dmesg | grep -E "IOMMU|AMD-Vi|amd_iommu" | head -20

# Find GPU PCI addresses and IDs
lspci -nn | grep -i nvidia
# e.g.: 01:00.0 [0300]: NVIDIA Corporation GB203 [GeForce RTX 5070 Ti] [10de:2c05]

# Find IOMMU group
find /sys/kernel/iommu_groups/*/devices/ -name "0000:01:00.*" 2>/dev/null

# Check current group type (should be DMA after fix, identity = Blocker 1)
cat /sys/kernel/iommu_groups/13/type

# Check for require_direct (Blocker 2)
dmesg | grep -i "require_direct"
```

## Quick Reference — After Every Kernel Update

```bash
apt install proxmox-headers-$(uname -r)
cd /root/vfio-fix
make clean && make
mkdir -p /lib/modules/$(uname -r)/extra
cp vfio_blackwell_fix.ko /lib/modules/$(uname -r)/extra/
depmod -a
# Test:
insmod /root/vfio-fix/vfio_blackwell_fix.ko
dmesg | grep vfio_fix
# Expect: vfio_fix: 0000:01:00.0 require_direct 1 -> 0
# Expect: vfio_fix: Blackwell passthrough unblocked
```
