# Host VFIO Setup (one-time)

Gets IOMMU enabled and the GPU bound to vfio-pci before any nvidia driver can claim it.

## Prerequisites

```bash
# Build tools (one-time)
apt install build-essential

# Kernel headers (per kernel version)
apt install proxmox-headers-$(uname -r)

# Verify headers present before building:
ls /lib/modules/$(uname -r)/build
# Must show kernel header files (Kconfig, Makefile, include/, etc.)
```

---

## Step 1 — Enable IOMMU in GRUB

`/etc/default/grub`:
```
GRUB_CMDLINE_LINUX_DEFAULT="quiet amd_iommu=on iommu=pt"
```

- `amd_iommu=on` — enables AMD IOMMU (use `intel_iommu=on` for Intel)
- `iommu=pt` — passthrough mode; reduces overhead
- `pcie_acs_override=downstream,multifunction` — NOT needed, removed (GPU is already isolated)

Apply:
```bash
update-grub
```

---

## Step 2 — Load VFIO modules at boot

`/etc/modules-load.d/vfio-passthrough.conf`:
```
vfio
vfio_iommu_type1
vfio_pci
```

Note: `vfio_virqfd` is built into the kernel in 7.x — not needed as a separate module.

Also: `/etc/modules-load.d/vfio-fix.conf`:
```
vfio_blackwell_fix
```
(loads the Blackwell fix module at boot, separate from the systemd service)

Legacy `/etc/modules` has duplicate entries — harmless but messy, canonical location is `modules-load.d/`.

---

## Step 3 — Bind GPU to vfio-pci

`/etc/modprobe.d/vfio.conf`:
```
options vfio-pci ids=10de:2c05,10de:22e9
```
Replace IDs with your GPU's PCI IDs from `lspci -nn`.

---

## Step 4 — Blacklist nvidia drivers on host

`/etc/modprobe.d/blacklist-nvidia.conf`:
```
blacklist nouveau
blacklist nvidia
blacklist nvidiafb
blacklist snd_hda_intel
```
`snd_hda_intel` covers the GPU's audio function.

---

## Step 5 — Update initramfs and reboot

```bash
update-initramfs -u -k all
reboot
```

After reboot, verify:
```bash
dmesg | grep -E "AMD-Vi|IOMMU" | head -5
lspci -k | grep -A3 "01:00"
# Expect: Kernel driver in use: vfio-pci
```

---

## Kernel Pin — Safe Fallback

```bash
# Pin current working kernel
proxmox-boot-tool kernel pin 7.0.6-2-pve
proxmox-boot-tool refresh

# List kernels and see pin status
proxmox-boot-tool kernel list

# Test-boot a new kernel without permanently pinning
proxmox-boot-tool kernel pin <new-kernel> --next-boot
reboot

# Remove pin (allow auto-updates)
proxmox-boot-tool kernel unpin
proxmox-boot-tool refresh
```

---

## Files Changed / Created on This Server

| Path | Purpose |
|---|---|
| `/etc/default/grub` | GRUB cmdline: `amd_iommu=on iommu=pt` |
| `/etc/modprobe.d/vfio.conf` | Binds GPU PCI IDs to vfio-pci |
| `/etc/modprobe.d/blacklist-nvidia.conf` | Blacklists nvidia/nouveau/snd_hda_intel on host |
| `/etc/modules` | VFIO modules (legacy, has duplicate entries — harmless) |
| `/etc/modules-load.d/vfio-fix.conf` | Loads `vfio_blackwell_fix` module at boot |
| `/etc/modules-load.d/vfio-passthrough.conf` | Loads vfio modules at boot |
| `/etc/kernel/proxmox-boot-pin` | Kernel pin (7.0.6-2-pve) |
