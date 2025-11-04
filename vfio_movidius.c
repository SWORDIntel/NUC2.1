#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/vfio_platform.h>

#define DRIVER_NAME "vfio_movidius"

/*
 * This driver will bind to the platform_device created by the movidius_x_vpu driver
 * and provide a VFIO interface for it, allowing passthrough to a VM or userspace.
 */

struct vfio_movidius_dev {
    struct vfio_device vfio_device;
    struct platform_device *pdev;
};

static int vfio_movidius_init(struct vfio_device *vdev)
{
    pr_info("%s: init\n", DRIVER_NAME);
    return 0;
}

static void vfio_movidius_release(struct vfio_device *vdev)
{
    pr_info("%s: release\n", DRIVER_NAME);
}

static long vfio_movidius_ioctl(struct vfio_device *vdev,
                                unsigned int cmd, unsigned long arg)
{
    pr_info("%s: ioctl cmd=%u\n", DRIVER_NAME, cmd);
    return -EINVAL;
}

static const struct vfio_device_ops vfio_movidius_ops = {
    .name = DRIVER_NAME,
    .init = vfio_movidius_init,
    .release = vfio_movidius_release,
    .ioctl = vfio_movidius_ioctl,
};

static int vfio_movidius_probe(struct platform_device *pdev)
{
    struct vfio_movidius_dev *dev;
    int ret;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->pdev = pdev;
    platform_set_drvdata(pdev, dev);

    dev->vfio_device.ops = &vfio_movidius_ops;
    dev->vfio_device.dev = &pdev->dev;

    ret = vfio_platform_add_dev(pdev, &dev->vfio_device);
    if (ret) {
        dev_err(&pdev->dev, "Failed to add device to VFIO platform\n");
        kfree(dev);
        return ret;
    }

    dev_info(&pdev->dev, "VFIO movidius driver probed successfully\n");
    return 0;
}

static int vfio_movidius_remove(struct platform_device *pdev)
{
    struct vfio_movidius_dev *dev = platform_get_drvdata(pdev);

    vfio_platform_remove_dev(pdev, &dev->vfio_device);
    kfree(dev);

    dev_info(&pdev->dev, "VFIO movidius driver removed\n");
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
MODULE_DESCRIPTION("VFIO platform driver for Movidius Myriad X VPU");
