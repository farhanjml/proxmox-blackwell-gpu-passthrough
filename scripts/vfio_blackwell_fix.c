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
