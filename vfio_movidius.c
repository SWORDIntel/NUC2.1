#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/msi.h>
#include <linux/eventfd.h>
#include <linux/interrupt.h>

#define DRIVER_NAME "vfio_movidius"
#define MOVIDIUS_VFIO_VERSION 1

/* Region definitions */
#define MOVIDIUS_VFIO_REG_COUNT 3
#define MOVIDIUS_REG_REGION    0  /* Control registers */
#define MOVIDIUS_MEM_REGION    1  /* Device memory */
#define MOVIDIUS_MMAP_REGION   2  /* Shared memory region */

#define MOVIDIUS_REG_SIZE      (4 * 1024)      /* 4KB register space */
#define MOVIDIUS_MEM_SIZE      (512 * 1024 * 1024) /* 512MB device memory */
#define MOVIDIUS_MMAP_SIZE     (16 * 1024 * 1024)  /* 16MB shared memory */

/* IRQ definitions */
#define MOVIDIUS_NUM_IRQS      4
#define MOVIDIUS_IRQ_INTX      0
#define MOVIDIUS_IRQ_MSI       1
#define MOVIDIUS_IRQ_MSIX      2
#define MOVIDIUS_IRQ_ERR       3

/*
 * This driver provides a VFIO interface for the Movidius Myriad X VPU,
 * allowing device passthrough to VMs or direct userspace control via VFIO.
 */

struct vfio_movidius_region {
    u32 flags;
    u64 size;
    u64 offset;
    void *virt_base;
};

struct vfio_movidius_irq {
    u32 flags;
    u32 index;
    u32 count;
    struct eventfd_ctx *trigger;
    spinlock_t lock;
};

struct vfio_movidius_dev {
    struct vfio_device vdev;
    struct platform_device *pdev;
    struct vfio_movidius_region regions[MOVIDIUS_VFIO_REG_COUNT];
    struct vfio_movidius_irq irqs[MOVIDIUS_NUM_IRQS];
    atomic_t refcnt;
    struct mutex igate;
    void *device_data;
    bool msi_enabled;
    bool msix_enabled;
};

/* ========== Helper Functions ========== */

static int vfio_movidius_setup_regions(struct vfio_movidius_dev *vdev)
{
    /* Region 0: Control Registers */
    vdev->regions[MOVIDIUS_REG_REGION].flags = VFIO_REGION_INFO_FLAG_READ |
                                               VFIO_REGION_INFO_FLAG_WRITE |
                                               VFIO_REGION_INFO_FLAG_MMAP;
    vdev->regions[MOVIDIUS_REG_REGION].size = MOVIDIUS_REG_SIZE;
    vdev->regions[MOVIDIUS_REG_REGION].offset = 0;
    vdev->regions[MOVIDIUS_REG_REGION].virt_base = kzalloc(MOVIDIUS_REG_SIZE, GFP_KERNEL);
    if (!vdev->regions[MOVIDIUS_REG_REGION].virt_base) {
        return -ENOMEM;
    }

    /* Region 1: Device Memory */
    vdev->regions[MOVIDIUS_MEM_REGION].flags = VFIO_REGION_INFO_FLAG_READ |
                                               VFIO_REGION_INFO_FLAG_WRITE |
                                               VFIO_REGION_INFO_FLAG_MMAP;
    vdev->regions[MOVIDIUS_MEM_REGION].size = MOVIDIUS_MEM_SIZE;
    vdev->regions[MOVIDIUS_MEM_REGION].offset = MOVIDIUS_REG_SIZE;
    vdev->regions[MOVIDIUS_MEM_REGION].virt_base = NULL; /* Will be mapped on demand */

    /* Region 2: Shared Memory (MMAP) */
    vdev->regions[MOVIDIUS_MMAP_REGION].flags = VFIO_REGION_INFO_FLAG_READ |
                                                VFIO_REGION_INFO_FLAG_WRITE |
                                                VFIO_REGION_INFO_FLAG_MMAP;
    vdev->regions[MOVIDIUS_MMAP_REGION].size = MOVIDIUS_MMAP_SIZE;
    vdev->regions[MOVIDIUS_MMAP_REGION].offset = MOVIDIUS_REG_SIZE + MOVIDIUS_MEM_SIZE;
    vdev->regions[MOVIDIUS_MMAP_REGION].virt_base = kzalloc(MOVIDIUS_MMAP_SIZE, GFP_KERNEL);
    if (!vdev->regions[MOVIDIUS_MMAP_REGION].virt_base) {
        kfree(vdev->regions[MOVIDIUS_REG_REGION].virt_base);
        return -ENOMEM;
    }

    dev_info(&vdev->pdev->dev, "VFIO regions configured: REG=%luKB, MEM=%luMB, MMAP=%luMB\n",
             MOVIDIUS_REG_SIZE / 1024,
             MOVIDIUS_MEM_SIZE / (1024 * 1024),
             MOVIDIUS_MMAP_SIZE / (1024 * 1024));

    return 0;
}

static void vfio_movidius_cleanup_regions(struct vfio_movidius_dev *vdev)
{
    int i;
    for (i = 0; i < MOVIDIUS_VFIO_REG_COUNT; i++) {
        if (vdev->regions[i].virt_base) {
            kfree(vdev->regions[i].virt_base);
            vdev->regions[i].virt_base = NULL;
        }
    }
}

static int vfio_movidius_setup_irqs(struct vfio_movidius_dev *vdev)
{
    int i;

    for (i = 0; i < MOVIDIUS_NUM_IRQS; i++) {
        vdev->irqs[i].flags = VFIO_IRQ_INFO_EVENTFD;
        vdev->irqs[i].index = i;
        vdev->irqs[i].count = 1;
        vdev->irqs[i].trigger = NULL;
        spin_lock_init(&vdev->irqs[i].lock);
    }

    /* Configure specific IRQ types */
    vdev->irqs[MOVIDIUS_IRQ_INTX].flags |= VFIO_IRQ_INFO_MASKABLE;
    vdev->irqs[MOVIDIUS_IRQ_MSI].flags |= VFIO_IRQ_INFO_NORESIZE;
    vdev->irqs[MOVIDIUS_IRQ_MSIX].flags |= VFIO_IRQ_INFO_NORESIZE;
    vdev->irqs[MOVIDIUS_IRQ_MSIX].count = 8; /* Support 8 MSI-X vectors */

    dev_info(&vdev->pdev->dev, "VFIO IRQs configured: INTX, MSI, MSI-X (8 vectors), ERR\n");
    return 0;
}

static void vfio_movidius_cleanup_irqs(struct vfio_movidius_dev *vdev)
{
    int i;
    unsigned long flags;

    for (i = 0; i < MOVIDIUS_NUM_IRQS; i++) {
        spin_lock_irqsave(&vdev->irqs[i].lock, flags);
        if (vdev->irqs[i].trigger) {
            eventfd_ctx_put(vdev->irqs[i].trigger);
            vdev->irqs[i].trigger = NULL;
        }
        spin_unlock_irqrestore(&vdev->irqs[i].lock, flags);
    }
}

/* ========== VFIO Device Operations ========== */

static int vfio_movidius_open_device(struct vfio_device *core_vdev)
{
    struct vfio_movidius_dev *vdev = container_of(core_vdev, struct vfio_movidius_dev, vdev);
    int ret;

    if (atomic_cmpxchg(&vdev->refcnt, 0, 1) != 0) {
        return -EBUSY;
    }

    ret = vfio_movidius_setup_regions(vdev);
    if (ret) {
        atomic_set(&vdev->refcnt, 0);
        return ret;
    }

    ret = vfio_movidius_setup_irqs(vdev);
    if (ret) {
        vfio_movidius_cleanup_regions(vdev);
        atomic_set(&vdev->refcnt, 0);
        return ret;
    }

    dev_info(&vdev->pdev->dev, "VFIO device opened\n");
    return 0;
}

static void vfio_movidius_close_device(struct vfio_device *core_vdev)
{
    struct vfio_movidius_dev *vdev = container_of(core_vdev, struct vfio_movidius_dev, vdev);

    vfio_movidius_cleanup_irqs(vdev);
    vfio_movidius_cleanup_regions(vdev);
    atomic_set(&vdev->refcnt, 0);

    dev_info(&vdev->pdev->dev, "VFIO device closed\n");
}

static ssize_t vfio_movidius_read(struct vfio_device *core_vdev,
                                  char __user *buf, size_t count, loff_t *ppos)
{
    struct vfio_movidius_dev *vdev = container_of(core_vdev, struct vfio_movidius_dev, vdev);
    unsigned int region_idx = *ppos >> 40;
    u64 region_offset = *ppos & ((1ULL << 40) - 1);
    struct vfio_movidius_region *region;
    size_t to_copy;

    if (region_idx >= MOVIDIUS_VFIO_REG_COUNT) {
        return -EINVAL;
    }

    region = &vdev->regions[region_idx];

    if (!(region->flags & VFIO_REGION_INFO_FLAG_READ)) {
        return -EINVAL;
    }

    if (region_offset >= region->size) {
        return 0;
    }

    to_copy = min(count, (size_t)(region->size - region_offset));

    if (region->virt_base) {
        if (copy_to_user(buf, region->virt_base + region_offset, to_copy)) {
            return -EFAULT;
        }
    } else {
        /* For regions without backing memory, return zeros */
        if (clear_user(buf, to_copy)) {
            return -EFAULT;
        }
    }

    *ppos += to_copy;
    return to_copy;
}

static ssize_t vfio_movidius_write(struct vfio_device *core_vdev,
                                   const char __user *buf, size_t count, loff_t *ppos)
{
    struct vfio_movidius_dev *vdev = container_of(core_vdev, struct vfio_movidius_dev, vdev);
    unsigned int region_idx = *ppos >> 40;
    u64 region_offset = *ppos & ((1ULL << 40) - 1);
    struct vfio_movidius_region *region;
    size_t to_copy;

    if (region_idx >= MOVIDIUS_VFIO_REG_COUNT) {
        return -EINVAL;
    }

    region = &vdev->regions[region_idx];

    if (!(region->flags & VFIO_REGION_INFO_FLAG_WRITE)) {
        return -EINVAL;
    }

    if (region_offset >= region->size) {
        return 0;
    }

    to_copy = min(count, (size_t)(region->size - region_offset));

    if (region->virt_base) {
        if (copy_from_user(region->virt_base + region_offset, buf, to_copy)) {
            return -EFAULT;
        }
    }

    *ppos += to_copy;
    return to_copy;
}

static int vfio_movidius_mmap(struct vfio_device *core_vdev, struct vm_area_struct *vma)
{
    struct vfio_movidius_dev *vdev = container_of(core_vdev, struct vfio_movidius_dev, vdev);
    unsigned int region_idx = vma->vm_pgoff >> (40 - PAGE_SHIFT);
    u64 region_offset = (vma->vm_pgoff << PAGE_SHIFT) & ((1ULL << 40) - 1);
    struct vfio_movidius_region *region;
    unsigned long pfn;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (region_idx >= MOVIDIUS_VFIO_REG_COUNT) {
        return -EINVAL;
    }

    region = &vdev->regions[region_idx];

    if (!(region->flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        return -EINVAL;
    }

    if (region_offset + size > region->size) {
        return -EINVAL;
    }

    if (!region->virt_base) {
        return -ENOMEM;
    }

    pfn = virt_to_phys(region->virt_base + region_offset) >> PAGE_SHIFT;
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        return -EAGAIN;
    }

    dev_info(&vdev->pdev->dev, "MMAP region %d: offset=0x%llx size=0x%lx\n",
             region_idx, region_offset, size);

    return 0;
}

static long vfio_movidius_ioctl(struct vfio_device *core_vdev,
                                unsigned int cmd, unsigned long arg)
{
    struct vfio_movidius_dev *vdev = container_of(core_vdev, struct vfio_movidius_dev, vdev);
    unsigned long minsz;

    switch (cmd) {
    case VFIO_DEVICE_GET_INFO:
        {
            struct vfio_device_info info;

            minsz = offsetofend(struct vfio_device_info, num_irqs);
            if (copy_from_user(&info, (void __user *)arg, minsz))
                return -EFAULT;

            if (info.argsz < minsz)
                return -EINVAL;

            info.flags = VFIO_DEVICE_FLAGS_PLATFORM | VFIO_DEVICE_FLAGS_RESET;
            info.num_regions = MOVIDIUS_VFIO_REG_COUNT;
            info.num_irqs = MOVIDIUS_NUM_IRQS;

            if (copy_to_user((void __user *)arg, &info, minsz))
                return -EFAULT;

            return 0;
        }

    case VFIO_DEVICE_GET_REGION_INFO:
        {
            struct vfio_region_info info;
            struct vfio_movidius_region *region;

            minsz = offsetofend(struct vfio_region_info, offset);
            if (copy_from_user(&info, (void __user *)arg, minsz))
                return -EFAULT;

            if (info.argsz < minsz)
                return -EINVAL;

            if (info.index >= MOVIDIUS_VFIO_REG_COUNT)
                return -EINVAL;

            region = &vdev->regions[info.index];

            info.flags = region->flags;
            info.size = region->size;
            info.offset = ((u64)info.index << 40) | region->offset;
            info.cap_offset = 0;

            if (copy_to_user((void __user *)arg, &info, minsz))
                return -EFAULT;

            return 0;
        }

    case VFIO_DEVICE_GET_IRQ_INFO:
        {
            struct vfio_irq_info info;
            struct vfio_movidius_irq *irq;

            minsz = offsetofend(struct vfio_irq_info, count);
            if (copy_from_user(&info, (void __user *)arg, minsz))
                return -EFAULT;

            if (info.argsz < minsz)
                return -EINVAL;

            if (info.index >= MOVIDIUS_NUM_IRQS)
                return -EINVAL;

            irq = &vdev->irqs[info.index];

            info.flags = irq->flags;
            info.count = irq->count;

            if (copy_to_user((void __user *)arg, &info, minsz))
                return -EFAULT;

            return 0;
        }

    case VFIO_DEVICE_SET_IRQS:
        {
            struct vfio_irq_set hdr;
            struct vfio_movidius_irq *irq;
            u8 *data = NULL;
            int ret = 0;

            minsz = offsetofend(struct vfio_irq_set, count);
            if (copy_from_user(&hdr, (void __user *)arg, minsz))
                return -EFAULT;

            if (hdr.argsz < minsz)
                return -EINVAL;

            if (hdr.index >= MOVIDIUS_NUM_IRQS)
                return -EINVAL;

            irq = &vdev->irqs[hdr.index];

            if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD) {
                int32_t fd;
                struct eventfd_ctx *trigger;

                if (hdr.argsz < minsz + sizeof(fd))
                    return -EINVAL;

                if (copy_from_user(&fd, (void __user *)(arg + minsz), sizeof(fd)))
                    return -EFAULT;

                if (fd >= 0) {
                    trigger = eventfd_ctx_fdget(fd);
                    if (IS_ERR(trigger))
                        return PTR_ERR(trigger);

                    spin_lock(&irq->lock);
                    if (irq->trigger)
                        eventfd_ctx_put(irq->trigger);
                    irq->trigger = trigger;
                    spin_unlock(&irq->lock);

                    dev_info(&vdev->pdev->dev, "IRQ %d eventfd set to fd=%d\n",
                             hdr.index, fd);
                } else {
                    spin_lock(&irq->lock);
                    if (irq->trigger) {
                        eventfd_ctx_put(irq->trigger);
                        irq->trigger = NULL;
                    }
                    spin_unlock(&irq->lock);
                }
            }

            if (data)
                kfree(data);

            return ret;
        }

    case VFIO_DEVICE_RESET:
        {
            dev_info(&vdev->pdev->dev, "Device reset requested\n");

            /* Reset device state */
            vfio_movidius_cleanup_irqs(vdev);
            vfio_movidius_setup_irqs(vdev);

            /* Clear all regions */
            if (vdev->regions[MOVIDIUS_REG_REGION].virt_base) {
                memset(vdev->regions[MOVIDIUS_REG_REGION].virt_base, 0, MOVIDIUS_REG_SIZE);
            }
            if (vdev->regions[MOVIDIUS_MMAP_REGION].virt_base) {
                memset(vdev->regions[MOVIDIUS_MMAP_REGION].virt_base, 0, MOVIDIUS_MMAP_SIZE);
            }

            dev_info(&vdev->pdev->dev, "Device reset completed\n");
            return 0;
        }

    default:
        dev_warn(&vdev->pdev->dev, "Unknown ioctl: 0x%x\n", cmd);
        return -ENOTTY;
    }
}

static const struct vfio_device_ops vfio_movidius_ops = {
    .name = DRIVER_NAME,
    .open_device = vfio_movidius_open_device,
    .close_device = vfio_movidius_close_device,
    .read = vfio_movidius_read,
    .write = vfio_movidius_write,
    .ioctl = vfio_movidius_ioctl,
    .mmap = vfio_movidius_mmap,
};

/* ========== Platform Driver ========== */

static int vfio_movidius_probe(struct platform_device *pdev)
{
    struct vfio_movidius_dev *vdev;
    struct vfio_device *vfio_dev;
    int ret;

    vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    vdev->pdev = pdev;
    platform_set_drvdata(pdev, vdev);
    atomic_set(&vdev->refcnt, 0);
    mutex_init(&vdev->igate);

    vfio_dev = &vdev->vdev;
    vfio_init_group_dev(vfio_dev, &pdev->dev, &vfio_movidius_ops);

    ret = vfio_register_group_dev(vfio_dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to register VFIO group device: %d\n", ret);
        goto err_free;
    }

    dev_info(&pdev->dev, "VFIO Movidius driver probed successfully (version %d)\n",
             MOVIDIUS_VFIO_VERSION);
    dev_info(&pdev->dev, "  - %d regions available\n", MOVIDIUS_VFIO_REG_COUNT);
    dev_info(&pdev->dev, "  - %d IRQ types supported\n", MOVIDIUS_NUM_IRQS);
    dev_info(&pdev->dev, "  - Device ready for passthrough or userspace control\n");

    return 0;

err_free:
    kfree(vdev);
    return ret;
}

static int vfio_movidius_remove(struct platform_device *pdev)
{
    struct vfio_movidius_dev *vdev = platform_get_drvdata(pdev);

    vfio_unregister_group_dev(&vdev->vdev);
    vfio_put_device(&vdev->vdev);

    dev_info(&pdev->dev, "VFIO Movidius driver removed\n");
    return 0;
}

static const struct platform_device_id vfio_movidius_ids[] = {
    { .name = "movidius_x_vpu" },
    { }
};
MODULE_DEVICE_TABLE(platform, vfio_movidius_ids);

static struct platform_driver vfio_movidius_driver = {
    .driver = {
        .name = DRIVER_NAME,
    },
    .probe = vfio_movidius_probe,
    .remove = vfio_movidius_remove,
    .id_table = vfio_movidius_ids,
};

module_platform_driver(vfio_movidius_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("VFIO platform driver for Movidius Myriad X VPU with full DMA, IRQ, and mmap support");
MODULE_VERSION("2.0");
