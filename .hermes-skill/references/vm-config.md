# VM 888 Configuration, EFI Disk, Secure Boot, and NVIDIA Driver

---

## VM 888 Current Config

```
agent: 1
bios: ovmf
boot: order=scsi0;ide2;net0
cores: 4
cpu: host
efidisk0: local-zfs:vm-888-disk-0,efitype=4m,ms-cert=2023k,pre-enrolled-keys=0,size=1M
hostpci0: 0000:01:00.0,pcie=1,rombar=0
hostpci1: 0000:01:00.1,pcie=1
ide2: none,media=cdrom
machine: q35
memory: 32768
name: ai
net0: virtio=BC:24:11:D5:04:DD,bridge=vmbr0
numa: 0
onboot: 1
ostype: l26
scsi0: local-zfs:vm-888-disk-1,iothread=1,size=400G
scsihw: virtio-scsi-single
sockets: 1
vga: virtio
```

---

## Required VM Settings

- **Machine type:** `q35` — required for PCIe passthrough
- **BIOS:** `OVMF` (UEFI) — required for modern GPU passthrough
- **hostpci lines:**
  ```
  hostpci0: 0000:01:00.0,pcie=1,rombar=0
  hostpci1: 0000:01:00.1,pcie=1
  ```
  - `pcie=1` — present as PCIe, required for Blackwell
  - `rombar=0` on function 0 — suppresses ROM BAR which can cause boot issues
  - Pass BOTH functions (video + audio) together

---

## EFI Disk and 2023 Microsoft UEFI Certs

The `efidisk0` (1 MB) stores the VM's UEFI firmware state: Secure Boot certificates and boot variables.

### How it was created
```bash
qm set 888 --efidisk0 local-zfs:0,efitype=4m,pre-enrolled-keys=1
```

### 2023 cert enrollment (required — 2011 certs expired June 2026)
```bash
qm stop 888
qm enroll-efi-keys 888   # writes 2023 certs into EFI disk, adds ms-cert=2023k to config
qm start 888
```

Enrolled certs: `MicrosoftUEFICA2023.pem`, `WindowsUEFICA2023.pem`, `MicrosoftCorporationKEK2KCA2023.pem`

**If the warning `EFI disk without 'ms-cert=2023k' option` reappears:**
```bash
qm stop 888 && qm enroll-efi-keys 888 && qm start 888
```

Note: `pre-enrolled-keys` changes to `0` after `qm enroll-efi-keys` — this is expected (2023 certs replaced originals).

---

## Secure Boot — Host vs VM

**Host Secure Boot and VM Secure Boot are completely independent.**
Disabling it in the host BIOS has no effect on the VM. OVMF enables Secure Boot by default
when keys are enrolled. NVIDIA drivers fail to install with Secure Boot enabled inside the VM.

### Disable Secure Boot inside VM 888

1. Proxmox web UI → VM 888 → Console (noVNC)
2. Restart VM, press **Escape** during TianoCore splash to enter UEFI setup
3. Device Manager → Secure Boot Configuration → set **"Attempt Secure Boot"** to disabled
4. F10 to save → Y → Continue

Verify inside VM:
```bash
mokutil --sb-state
# Expect: SecureBoot disabled
```

This is persisted in the EFI disk and survives reboots.

---

## NVIDIA Driver Installation (inside VM 888)

All commands below run **inside VM 888**, not on the Proxmox host.

### Step 1 — Verify GPU is visible
```bash
lspci | grep -i nvidia
# 01:00.0 VGA compatible controller: NVIDIA Corporation Device 2c05 (rev a1)
# 02:00.0 Audio device: NVIDIA Corporation Device 22e9 (rev a1)
```

**Note:** BDF addresses inside the VM differ from the host (`01:00.1` on host → `02:00.0` in VM). Normal.

### Step 2 — Verify Secure Boot is disabled
```bash
mokutil --sb-state
# Expect: SecureBoot disabled
```

### Step 3 — Check kernel messages
```bash
sudo dmesg | grep -i nvidia
```

On Blackwell you will see:
```
NVRM: The NVIDIA GPU 0000:01:00.0 (PCI ID: 10de:2c05)
NVRM: installed in this system requires use of the NVIDIA open kernel modules.
```

**Blackwell (GB20x) requires the open kernel modules — proprietary driver does NOT support it.**

### Step 4 — Install the open kernel module driver
```bash
# Remove proprietary driver if already installed
sudo apt remove nvidia-dkms-595

# Install the open kernel module variant
sudo apt install nvidia-dkms-595-open
```

The `-open` suffix is NVIDIA's open-source kernel module (published 2022). Mandatory for all
Blackwell GPUs (RTX 5070, 5070 Ti, 5080, 5090). Replace `595` with the current available version:
```bash
apt search nvidia-dkms | grep -i open
```

### Step 5 — Load driver and reboot
```bash
sudo modprobe -r nvidia
sudo modprobe nvidia-open 2>/dev/null; sudo modprobe nvidia
sudo reboot
```

### Step 6 — Verify driver
```bash
nvidia-smi
# Expect: table showing RTX 5070 Ti, driver version, temperature, memory
```

If `nvidia-smi` works, the entire chain is confirmed:
host IOMMU → vfio-pci → `require_direct` cleared → VM → open kernel module → GPU functional.

### Step 7 — Verify CUDA
```bash
nvidia-smi | grep CUDA
nvidia-smi -L
# If PyTorch installed:
python3 -c "import torch; print(torch.cuda.is_available()); print(torch.cuda.get_device_name(0))"
# Expect: True
# Expect: NVIDIA GeForce RTX 5070 Ti
```

### Common Failures

| Symptom | Cause | Fix |
|---|---|---|
| `requires use of the NVIDIA open kernel modules` in dmesg | Proprietary driver installed | Switch to `-open` variant |
| Driver installs but GPU not detected | Secure Boot still enabled | Disable via OVMF setup |
| `lspci` shows no NVIDIA device | Passthrough not working | Check host verification checklist |
| `nvidia-smi: command not found` | Driver not installed or not in PATH | Check `apt install nvidia-dkms-595-open` completed |
| `nvidia-smi` fails with no devices | Module loaded but GPU inaccessible | Check `dmesg | grep -i nvidia` for errors |
