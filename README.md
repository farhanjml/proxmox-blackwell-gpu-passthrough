# Proxmox Blackwell GPU Passthrough Fix

This repo fixes GPU passthrough for NVIDIA Blackwell cards (RTX 5070,
5070 Ti, 5080, 5090 — GB20x chips) on Proxmox VE.

Blackwell GPUs set a firmware flag. The Linux kernel checks this flag
before it lets `vfio-pci` claim the device for a VM. The check blocks
passthrough. This repo has the kernel module that clears the flag, the
systemd service that loads the module at boot, and a full walkthrough.

Tested on Proxmox VE 9.2, kernel `7.0.6-2-pve`, RTX 5070 Ti.

---

## What This Fixes

Proxmox bug [#7374](https://bugzilla.proxmox.com/show_bug.cgi?id=7374).

Two separate problems block Blackwell GPU passthrough. You must fix both.

### Problem 1 — IOMMU group type: `identity`

On some AMD systems, the IOMMU driver puts the GPU's IOMMU group in
`identity` mode. The `vfio_iommu_type1` driver cannot manage a group in
that mode. Attaching the GPU to a VM fails with this error:

```
vfio 0000:01:00.0: failed to setup container for group 13: Failed to set group container: Invalid argument
```

**Fix:** switch the group to DMA mode before the VM starts:
```bash
echo DMA > /sys/kernel/iommu_groups/13/type
```

### Problem 2 — the `require_direct` firmware flag (Blackwell-specific)

Blackwell GPUs (GB202/GB203/GB205 dies — RTX 5070, 5070 Ti, 5080, 5090)
carry a firmware flag. The flag tells the kernel's IOMMU core that the
device needs a 1:1 ("direct") memory mapping. When the flag is set,
`drivers/iommu/iommu.c` in the Linux kernel refuses to attach the device
to any other kind of domain. `vfio-pci` needs exactly that kind of attach
to hand the GPU to a VM. The refusal blocks passthrough.

This check lives in upstream kernel code, not in the NVIDIA driver. It
affects every current Proxmox kernel. Older NVIDIA generations (Ada
Lovelace / RTX 4000 and earlier) do not set this flag. This problem is
Blackwell-only.

**Fix:** a small kernel module clears `require_direct` back to 0 for the
GPU's PCI devices, right after boot and before the VM starts.

---

## How the Fix Works

`scripts/vfio_blackwell_fix.c` is a small standalone kernel module. It
runs once at load time, then goes idle:

1. At `module_init`, it looks up the GPU's two PCI functions (the GPU
   itself, and its HDMI audio device) by PCI bus address.
2. For each function, it reaches into the kernel's own `struct dev_iommu`
   — a public, stable kernel structure, not a private or reverse-engineered
   one — and sets `require_direct = 0`.
3. It logs what it did. It then goes idle until shutdown. `rmmod` unloads
   it cleanly at any time.

**Why the timing matters:** the module must run *after* the kernel's IOMMU
core attaches `dev->iommu` to the GPU. This attach happens during early
boot. The module must also run *before* `vfio-pci` claims the device. That
claim happens when the VM starts. `systemd/vfio-blackwell-fix.service`
enforces this order:

```ini
After=systemd-modules-load.service
Before=pve-guests.service
```

`systemd-modules-load.service` loads the base VFIO modules (`vfio`,
`vfio_pci`, `vfio_iommu_type1`) at boot. `pve-guests.service` auto-starts
VMs. This fix's service runs between the two. By the time any VM starts,
the IOMMU group is already in DMA mode and `require_direct` is already
clear.

---

## How the Fix Loads on Boot

The fix runs on the **Proxmox host**, not inside the VM. There is no
Python script and no daemon. The host runs a one-shot kernel-level fix,
loaded two ways for redundancy:

1. **`systemd/vfio-blackwell-fix.service`** — a `oneshot` systemd unit,
   enabled so it runs on every boot:
   - `ExecStart` — flips the GPU's IOMMU group to DMA mode
     (`echo DMA > /sys/kernel/iommu_groups/<N>/type`)
   - `ExecStartPost` — loads the kernel module with
     `modprobe vfio_blackwell_fix`, which clears `require_direct` for both
     GPU PCI functions
   - `RemainAfterExit=yes` — systemd marks the unit "active" once the
     one-shot work finishes. This is correct for a unit of this type.

2. **`/etc/modules-load.d/vfio-fix.conf`** also lists
   `vfio_blackwell_fix`, as a second, independent load path. If the
   systemd unit is ever disabled by accident, this file still loads the
   module. Either path loading the module is enough on its own.

Confirm the fix ran, after any boot:
```bash
systemctl status vfio-blackwell-fix.service   # oneshot: "inactive (dead)"
                                               # is normal AFTER it runs
lsmod | grep vfio_blackwell_fix               # module should be listed
dmesg | grep vfio_fix
# vfio_fix: 0000:01:00.0 require_direct 1 -> 0
# vfio_fix: Blackwell passthrough unblocked
```

**Important:** this module only unlocks the GPU on the host side. The VM
still needs its own NVIDIA driver inside the guest OS. See Part 3 below.

---

## Repo Layout

```
scripts/vfio_blackwell_fix.c       kernel module source
scripts/Makefile                   builds the module against the running kernel
systemd/vfio-blackwell-fix.service loads the fix at boot, before VMs start
docs/                              detailed reference docs — host setup, VM
                                    config, verification checklist, kernel
                                    update procedure, from-source kernel-patch
                                    alternative
```

---

## Full Walkthrough — Fresh Proxmox Host to Working GPU in a VM

This section is the complete path from a stock Proxmox install to a
working NVIDIA GPU inside a VM. Adjust IDs and addresses in every command
to match your own hardware — see the note at the top of each step for how
to find your value.

### Part 1 — Prepare the Proxmox host

**Step 1.1 — Confirm your GPU and note its PCI IDs**
```bash
lspci -nn | grep -i nvidia
```
Example output:
```
01:00.0 VGA compatible controller [0300]: NVIDIA Corporation GB203 [GeForce RTX 5070 Ti] [10de:2c05]
01:00.1 Audio device [0403]: NVIDIA Corporation GB203 High Definition Audio Controller [10de:22e9]
```
Note the two PCI addresses (`01:00.0`, `01:00.1`) and the two PCI IDs
(`10de:2c05`, `10de:22e9`). You need all four values in later steps.

**Step 1.2 — Enable IOMMU in GRUB**

Edit `/etc/default/grub`. Set this line (use `intel_iommu=on` instead of
`amd_iommu=on` on Intel hardware):
```
GRUB_CMDLINE_LINUX_DEFAULT="quiet amd_iommu=on iommu=pt"
```
Apply the change:
```bash
update-grub
```

**Step 1.3 — Load VFIO modules at boot**

Create `/etc/modules-load.d/vfio-passthrough.conf`:
```
vfio
vfio_iommu_type1
vfio_pci
```

**Step 1.4 — Bind the GPU to vfio-pci**

Create `/etc/modprobe.d/vfio.conf`. Use the PCI IDs from Step 1.1:
```
options vfio-pci ids=10de:2c05,10de:22e9
```

**Step 1.5 — Blacklist host NVIDIA drivers**

Create `/etc/modprobe.d/blacklist-nvidia.conf`:
```
blacklist nouveau
blacklist nvidia
blacklist nvidiafb
blacklist snd_hda_intel
```
This keeps the Proxmox host itself from ever loading a driver for the
card. `snd_hda_intel` covers the card's audio function.

**Step 1.6 — Rebuild initramfs and reboot**
```bash
update-initramfs -u -k all
reboot
```

**Step 1.7 — Verify the GPU bound to vfio-pci**
```bash
lspci -k | grep -A3 "01:00"
```
Expect `Kernel driver in use: vfio-pci` for both functions. If you instead
see `nvidia` or `nouveau`, recheck Steps 1.4–1.6.

**Step 1.8 — Find the GPU's IOMMU group number**
```bash
find /sys/kernel/iommu_groups/*/devices/ -name "0000:01:00.*"
```
The path contains the group number, for example
`/sys/kernel/iommu_groups/13/devices/0000:01:00.0` means group `13`. You
need this number in Part 2.

### Part 2 — Build and install the Blackwell fix

**Step 2.1 — Install kernel headers**
```bash
apt install build-essential proxmox-headers-$(uname -r)
ls /lib/modules/$(uname -r)/build   # confirms headers installed correctly
```

**Step 2.2 — Get the module source onto the host**
```bash
mkdir -p /root/vfio-fix
cp scripts/vfio_blackwell_fix.c scripts/Makefile /root/vfio-fix/
```

**Step 2.3 — Edit the module source for your hardware**

Open `/root/vfio-fix/vfio_blackwell_fix.c`. Find these two lines near the
bottom:
```c
clear_device("0000:01:00.0");
clear_device("0000:01:00.1");
```
Replace both PCI addresses with your own values from Step 1.1.

**Step 2.4 — Build the module**
```bash
cd /root/vfio-fix
make clean && make
```
This produces `vfio_blackwell_fix.ko`.

**Step 2.5 — Install the module**
```bash
mkdir -p /lib/modules/$(uname -r)/extra
cp vfio_blackwell_fix.ko /lib/modules/$(uname -r)/extra/
depmod -a
```

**Step 2.6 — Install and enable the boot service**

Copy `systemd/vfio-blackwell-fix.service` from this repo to
`/etc/systemd/system/vfio-blackwell-fix.service`. Edit the `ExecStart`
line to use your IOMMU group number from Step 1.8:
```ini
ExecStart=/bin/bash -c 'echo DMA > /sys/kernel/iommu_groups/13/type'
```
Enable the service:
```bash
systemctl daemon-reload
systemctl enable vfio-blackwell-fix.service
```

**Step 2.7 — Test the fix without rebooting**
```bash
echo DMA > /sys/kernel/iommu_groups/13/type   # use your group number
modprobe vfio_blackwell_fix
dmesg | grep vfio_fix
```
Expect:
```
vfio_fix: 0000:01:00.0 require_direct 1 -> 0
vfio_fix: 0000:01:00.1 require_direct 1 -> 0
vfio_fix: Blackwell passthrough unblocked
```

**Step 2.8 — Reboot and confirm the fix runs automatically**
```bash
reboot
```
After the host comes back up:
```bash
lsmod | grep vfio_blackwell_fix
dmesg | grep vfio_fix
cat /sys/kernel/iommu_groups/13/type   # use your group number, expect: DMA
```

### Part 3 — Create the VM and pass through the GPU

**Step 3.1 — Create a VM with the required settings**

In the Proxmox web UI, or with `qm create`, set:
- **Machine type:** `q35` — required for PCIe passthrough
- **BIOS:** `OVMF` (UEFI) — required for a Blackwell card to work correctly

**Step 3.2 — Add the GPU to the VM config**

Add these two lines to the VM's config (replace the VM ID and PCI
addresses with your own):
```
hostpci0: 0000:01:00.0,pcie=1,rombar=0
hostpci1: 0000:01:00.1,pcie=1
```
`pcie=1` presents the device as PCIe — required for Blackwell.
`rombar=0` on the video function suppresses the ROM BAR, which can
otherwise cause the VM to fail to boot. Pass both functions (video and
audio) together.

**Step 3.3 — Enroll 2023 UEFI certificates on the EFI disk**

Recent Windows guests need the 2023 Microsoft UEFI certificates, since the
2011 certificates expired in June 2026. Skip this step for a Linux guest
unless it also checks Secure Boot certificates.
```bash
qm stop <vmid>
qm enroll-efi-keys <vmid>
qm start <vmid>
```
This writes the 2023 certificates to the VM's `efidisk0` and adds
`ms-cert=2023k` to the config automatically.

**Step 3.4 — Disable Secure Boot inside the VM**

NVIDIA drivers fail to install with Secure Boot enabled inside the guest.
Host Secure Boot and VM Secure Boot are independent — disabling it on the
host has no effect on the VM.
1. Open the VM's console in the Proxmox web UI.
2. Restart the VM. Press **Escape** during the TianoCore splash screen.
3. In the UEFI setup menu: Device Manager → Secure Boot Configuration →
   set "Attempt Secure Boot" to disabled.
4. Press F10 to save, then confirm.

This setting persists on the EFI disk across reboots.

### Part 4 — Install the NVIDIA driver inside the VM

Run every command in this part inside the VM's guest OS, not on the
Proxmox host.

**Step 4.1 — Confirm the GPU is visible inside the VM**
```bash
lspci | grep -i nvidia
```
Note: PCI addresses inside the VM can differ from the host. For example,
the audio function may show as `02:00.0` in the guest even though it was
`01:00.1` on the host. This is normal.

**Step 4.2 — Confirm Secure Boot is off**
```bash
mokutil --sb-state
```
Expect `SecureBoot disabled`. If it still reports enabled, redo Step 3.4.

**Step 4.3 — Confirm the kernel sees the card**
```bash
sudo dmesg | grep -i nvidia
```
On a Blackwell card, you should see a line like:
```
NVRM: installed in this system requires use of the NVIDIA open kernel modules.
```

**Step 4.4 — Install the open kernel module driver**

Blackwell cards require NVIDIA's open-source kernel module. The
proprietary driver does not support this GPU generation.
```bash
sudo apt remove nvidia-dkms-595        # remove the proprietary driver, if installed
sudo apt install nvidia-dkms-595-open  # install the open-module variant
```
Replace `595` with whatever driver version is current:
```bash
apt search nvidia-dkms | grep -i open
```

**Step 4.5 — Load the driver and reboot**
```bash
sudo modprobe -r nvidia
sudo modprobe nvidia-open 2>/dev/null; sudo modprobe nvidia
sudo reboot
```

**Step 4.6 — Verify the driver**
```bash
nvidia-smi
```
Expect a table showing the card model, driver version, temperature, and
memory. A working `nvidia-smi` confirms the whole chain: host IOMMU →
vfio-pci → `require_direct` cleared → VM → open kernel module → working
GPU.

**Step 4.7 — Verify CUDA, if needed**
```bash
nvidia-smi -L
python3 -c "import torch; print(torch.cuda.is_available()); print(torch.cuda.get_device_name(0))"
# Expect: True
# Expect: your GPU's name
```

### Common Failures

| Symptom | Cause | Fix |
|---|---|---|
| `Failed to set group container: Invalid argument` | IOMMU group still in identity mode | Redo Step 1.8 and Step 2.7 |
| `requires use of the NVIDIA open kernel modules` in dmesg | Proprietary driver installed inside VM | Switch to the `-open` package (Step 4.4) |
| Driver installs but GPU not detected in VM | Secure Boot still enabled inside VM | Redo Step 3.4 |
| `lspci` in the VM shows no NVIDIA device | Passthrough not working | Recheck Part 1 and Part 2 on the host |
| `nvidia-smi: command not found` | Driver did not install, or is not in PATH | Recheck Step 4.4 completed without errors |
| `nvidia-smi` runs but reports no devices | Driver loaded but cannot reach the GPU | Check `dmesg \| grep -i nvidia` inside the VM for errors |

---

## After a Proxmox Kernel Update

The module is compiled against one specific kernel version. Rebuild it
after every kernel update, before you boot into the new kernel:

```bash
apt install proxmox-headers-$(uname -r)
cd /root/vfio-fix
make clean && make
mkdir -p /lib/modules/$(uname -r)/extra
cp vfio_blackwell_fix.ko /lib/modules/$(uname -r)/extra/
depmod -a
insmod /root/vfio-fix/vfio_blackwell_fix.ko
dmesg | grep vfio_fix
```

For the full kernel-pin and test-boot procedure, and a from-source
kernel-patch approach that removes the need to rebuild the module at all,
see `docs/kernel-build.md`.

---

## More Detail

`docs/` has expanded reference material for each part above:
- `docs/host-setup.md` — full host VFIO config file listing
- `docs/kernel-module.md` — full module source walkthrough, build steps
- `docs/vm-config.md` — full VM config reference, EFI/Secure Boot detail
- `docs/verification.md` — full verification checklist, revert procedure
- `docs/kernel-build.md` — kernel update procedure, from-source kernel patch

---

## License

MIT — see `LICENSE`.
