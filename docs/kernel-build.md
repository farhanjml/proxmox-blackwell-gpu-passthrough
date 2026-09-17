# Kernel Update Procedure and Patched Kernel Build

---

## Kernel Update Procedure

The `.ko` module is tied to an exact kernel version. When Proxmox updates the kernel,
rebuild and reinstall the module before switching.

### Step 1 — Install the new kernel and headers

```bash
apt update
apt install proxmox-kernel-7.0.x-y-pve proxmox-headers-7.0.x-y-pve
# Replace 7.0.x-y-pve with the actual new version
```

### Step 2 — Rebuild the module

```bash
NEW_KVER=7.0.x-y-pve   # replace with actual version

make -C /lib/modules/${NEW_KVER}/build M=/root/vfio-fix clean
make -C /lib/modules/${NEW_KVER}/build M=/root/vfio-fix modules
```

### Step 3 — Install the rebuilt module

```bash
mkdir -p /lib/modules/${NEW_KVER}/extra
cp /root/vfio-fix/vfio_blackwell_fix.ko /lib/modules/${NEW_KVER}/extra/
depmod -a ${NEW_KVER}
```

### Step 4 — Test-boot the new kernel once

```bash
proxmox-boot-tool kernel pin ${NEW_KVER} --next-boot
reboot
# SSH back in after reboot
uname -r                # confirm new kernel is running
qm start 888            # confirm VM starts with GPU
dmesg | grep vfio_fix   # confirm require_direct was cleared
```

### Step 5 — Make it permanent if successful

```bash
proxmox-boot-tool kernel pin ${NEW_KVER}
proxmox-boot-tool refresh
```

### Step 6 — If VM fails, debug and fall back

Power cycling returns to the old pinned kernel automatically.

```bash
dmesg | grep -E "vfio|iommu|require_direct"
# Common failure: module not found for new kernel — check depmod -a ran correctly
# Common failure: require_direct check changed location in new kernel source
```

---

## Build a Patched Kernel (Permanent Alternative)

The module approach requires rebuilding after every kernel update. A patched kernel
removes the `require_direct` check entirely — no module, no service, no maintenance.

Takes 1–2 hours on 16 cores. Only worth doing if you want to drop the module dependency.

### The fix to apply

In `drivers/iommu/iommu.c` (around line 2379 in Linux 7.0.6), remove this block:

```c
if (dev->iommu->require_direct &&
    (new_domain->type == IOMMU_DOMAIN_BLOCKED ||
     new_domain == group->blocking_domain)) {
    dev_warn(dev, "Firmware has requested this device have a 1:1 IOMMU mapping...\n");
    return -EINVAL;
}
```

### Build steps

**1. Install build dependencies:**
```bash
apt install git build-essential bc flex bison libssl-dev libelf-dev \
    dwarves debhelper rsync fakeroot
```

**2. Get the Proxmox kernel repo:**
```bash
# On this server: already cloned at /tmp/pve-kernel/ — skip the git clone
# On a fresh server:
git clone https://git.proxmox.com/git/pve-kernel.git /tmp/pve-kernel
cd /tmp/pve-kernel
git checkout pve-kernel-7.0   # branch for 7.0.x kernels
```

**3. Get the vanilla kernel source:**
```bash
# On this server: tarball already at /tmp/linux-7.0.6.tar.xz — skip the wget
# On a fresh server: wget https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.0.6.tar.xz -P /tmp
mkdir -p /usr/src/pve-kernel-build
tar -xJf /tmp/linux-7.0.6.tar.xz -C /usr/src/pve-kernel-build
```

**4. Apply Proxmox patches:**
```bash
cd /usr/src/pve-kernel-build/linux-7.0.6
for patch in /tmp/pve-kernel/patches/kernel/*.patch; do
    patch -p1 < "$patch"
done
```

**5. Apply the Blackwell fix:**
```bash
# Find the exact line numbers
grep -n "require_direct" drivers/iommu/iommu.c
# Remove the require_direct enforcement block (lines ~2379-2385)
nano drivers/iommu/iommu.c
```

**6. Copy existing kernel config and set version:**
```bash
cp /usr/src/linux-headers-7.0.6-2-pve/.config .config
sed -i 's/^EXTRAVERSION.*/EXTRAVERSION=-2-pve-blackwell/' Makefile
make olddefconfig
```

**7. Build (~1-2 hours on 16 cores):**
```bash
make -j$(nproc) bindeb-pkg
# Produces .deb files in /usr/src/
ls /usr/src/linux-image-*.deb
```

**8. Install:**
```bash
dpkg -i /usr/src/linux-image-7.0.6-2-pve-blackwell_*.deb
dpkg -i /usr/src/linux-headers-7.0.6-2-pve-blackwell_*.deb
proxmox-boot-tool refresh
```

**9. Test-boot:**
```bash
proxmox-boot-tool kernel pin 7.0.6-2-pve-blackwell --next-boot
reboot
# SSH back in, test VM
```

**10. If it works — disable the module-based fix (no longer needed):**
```bash
systemctl disable --now vfio-blackwell-fix.service
proxmox-boot-tool kernel pin 7.0.6-2-pve-blackwell
proxmox-boot-tool refresh
```
