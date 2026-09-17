# Verification Checklist, Revert Procedure, and Dead Ends

---

## Verification Checklist

Run these after any change or reboot to confirm everything is working.

```bash
# 1. IOMMU is active
dmesg | grep -E "AMD-Vi|IOMMU" | head -5

# 2. GPU is bound to vfio-pci (not nvidia)
lspci -k | grep -A3 "01:00"
# Expect: Kernel driver in use: vfio-pci

# 3. VFIO modules are loaded
lsmod | grep vfio
# Expect: vfio, vfio_iommu_type1, vfio_pci, vfio_blackwell_fix

# 4. IOMMU group type is DMA (not identity)
cat /sys/kernel/iommu_groups/13/type
# Expect: DMA

# 5. require_direct was cleared
dmesg | grep vfio_fix
# Expect: vfio_fix: 0000:01:00.0 require_direct 1 -> 0
# Expect: vfio_fix: Blackwell passthrough unblocked

# 6. VM starts and GPU resets
qm start 888
dmesg | grep "vfio-pci.*reset"
# Expect: vfio-pci 0000:01:00.0: reset done
# Expect: vfio-pci 0000:01:00.1: reset done

# 7. Kernel pin is set
proxmox-boot-tool kernel list
# Expect: Pinned kernel: 7.0.6-2-pve
```

---

## Revert / Disable Passthrough

### Disable passthrough (keep VM, remove GPU)

```bash
qm stop 888
systemctl disable --now vfio-blackwell-fix.service
rmmod vfio_blackwell_fix

# Remove hostpci lines from VM config
nano /etc/pve/qemu-server/888.conf
# Delete: hostpci0 and hostpci1 lines
```

### Remove the module entirely

```bash
rm /lib/modules/$(uname -r)/extra/vfio_blackwell_fix.ko
depmod -a
```

### Revert kernel pin (allow auto-updates again)

```bash
proxmox-boot-tool kernel unpin
proxmox-boot-tool refresh
```

### Revert GRUB (disable IOMMU entirely)

```bash
nano /etc/default/grub
# Remove amd_iommu=on iommu=pt from GRUB_CMDLINE_LINUX_DEFAULT
update-grub
reboot
```

---

## Dead Ends — Approaches That Do NOT Work

| Approach | Why it fails |
|---|---|
| Install NVIDIA driver on Proxmox host | Not needed. Driver must be inside the VM |
| `vfio_iommu_type1` with identity group | Type1 backend cannot manage identity-mapped groups |
| `iommufd` QEMU args (`-object iommufd`) | Requires `CONFIG_AMD_IOMMU_IOMMUFD=y`, not set in Proxmox kernel |
| Newer Proxmox kernel (6.17, 7.x) | Same `require_direct` check present, same missing config |
| `pcie_acs_override=downstream,multifunction` | Not needed when GPU is already isolated in its own group |
| Direct EFI variable write for MOK | Shim rejects unauthorized MokList writes (MokListTrusted) |
| Binary patch vmlinuz in memory | String in built-in kernel, exact instruction offset unclear |

---

## Notes

- No NVIDIA drivers on the host. GPU is exclusively passed through to VM 888.
- The fix is a runtime flag clear in memory, not a kernel patch. Safe and reversible.
- The module must be rebuilt against new kernel headers after every kernel update (see kernel-module.md).
- IOMMU group type change (`echo DMA`) is volatile and is re-applied by the systemd service on each boot.
