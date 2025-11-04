#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io_uring.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/scatterlist.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/vfio.h>
#include <linux/firmware.h>
#include <linux/pm_runtime.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/idr.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
#error "movidius_x_vpu needs >= 5.12 for io_uring_cmd"
#endif

#define DRIVER_NAME "movidius_x_vpu"
#define MAX_DEVICES 16

/* UAPI START */
#define MOVIDIUS_UAPI_VERSION 1
#define MAX_SG_SEGMENTS 16

struct movidius_dma_arena {
    uint64_t addr;
    uint64_t len;
};

struct movidius_sg_segment {
    uint32_t offset;
    uint32_t len;
};

struct movidius_cmd_hdr {
    uint16_t version;
    uint16_t op;
    uint32_t len;
};

struct inference_request {
    struct movidius_cmd_hdr hdr;
    uint32_t num_input_segs;
    uint32_t num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];
    uint64_t user_data;
};

struct batch_inference_request {
    struct movidius_cmd_hdr hdr;
    uint32_t count;
    uint32_t pad;
    uint64_t reqs;
};

#define MOVIDIUS_IOCTL_REGISTER_DMA_ARENA _IOW('M', 1, struct movidius_dma_arena)
#define MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA _IO('M', 2)

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
};
/* UAPI END */

/* Module Parameters */
static ushort vid = 0x03e7;
module_param(vid, ushort, 0444);
MODULE_PARM_DESC(vid, "USB Vendor ID (default 0x03e7)");

static ushort pid = 0x2485;
module_param(pid, ushort, 0444);
MODULE_PARM_DESC(pid, "USB Product ID (default 0x2485)");

/* Forward declarations */
static int movidius_platform_probe(struct platform_device *pdev);
static int movidius_platform_remove(struct platform_device *pdev);
static int movidius_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags);

/* Data Structures */
struct movidius_x_vpu_dev {
    struct device *dev;
    struct usb_device *udev;
    struct usb_interface *interface;
    struct platform_device *pdev;
    struct cdev cdev;
    dev_t devt;
    int minor;

    u8 bulk_in_endpoint_addr;
    u8 bulk_out_endpoint_addr;

    struct list_head request_queue;
    spinlock_t request_queue_lock;
    struct task_struct *submission_thread;
    wait_queue_head_t request_queue_wait;

    atomic_t is_open;
    struct list_head global_list; /* Member of global device list */
};

/* Global Variables */
static LIST_HEAD(movidius_devices);
static DEFINE_SPINLOCK(movidius_devices_lock);
static struct class *movidius_class;
static dev_t movidius_devt;
static DEFINE_IDA(movidius_minor_ida);

static const struct file_operations movidius_fops = {
    .owner = THIS_MODULE,
    .uring_cmd = movidius_uring_cmd,
    .unlocked_ioctl = NULL, /* To be added */
};

static int movidius_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
    // Will be implemented later
    return -EINVAL;
}

/* Platform Driver */
static struct platform_driver movidius_platform_driver = {
    .probe = movidius_platform_probe,
    .remove = movidius_platform_remove,
    .driver = {
        .name = DRIVER_NAME,
    },
};

static int submission_kthread(void *data)
{
    struct movidius_x_vpu_dev *dev = data;
    while (!kthread_should_stop()) {
        wait_event_interruptible(dev->request_queue_wait,
                                 !list_empty(&dev->request_queue) || kthread_should_stop());
        // Process requests
    }
    return 0;
}

static int movidius_platform_probe(struct platform_device *pdev)
{
    struct movidius_x_vpu_dev *dev = platform_get_drvdata(pdev);
    int ret;
    int minor;
    struct device *cdevice;

    dev_info(&pdev->dev, "platform probe entered\n");

    minor = ida_simple_get(&movidius_minor_ida, 0, MAX_DEVICES, GFP_KERNEL);
    if (minor < 0) {
        ret = minor;
        dev_err(&pdev->dev, "Failed to allocate minor number\n");
        goto err_out;
    }
    dev->minor = minor;
    dev->devt = MKDEV(MAJOR(movidius_devt), dev->minor);

    cdev_init(&dev->cdev, &movidius_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devt, 1);
    if (ret) {
        dev_err(&pdev->dev, "Failed to add cdev\n");
        goto err_ida_remove;
    }

    cdevice = device_create(movidius_class, &pdev->dev, dev->devt, dev,
                            DRIVER_NAME "_%d", dev->minor);
    if (IS_ERR(cdevice)) {
        dev_err(&pdev->dev, "Failed to create device node\n");
        ret = PTR_ERR(cdevice);
        goto err_cdev_del;
    }

    /* Initialize request queue and submission thread */
    spin_lock_init(&dev->request_queue_lock);
    INIT_LIST_HEAD(&dev->request_queue);
    init_waitqueue_head(&dev->request_queue_wait);

    dev->submission_thread = kthread_run(submission_kthread, dev, "movidius-submit-%d", dev->minor);
    if (IS_ERR(dev->submission_thread)) {
        ret = PTR_ERR(dev->submission_thread);
        dev_err(&pdev->dev, "Failed to create submission kthread\n");
        goto err_device_destroy;
    }

    dev_info(&pdev->dev, "Platform device registered successfully\n");
    return 0;

err_device_destroy:
    device_destroy(movidius_class, dev->devt);
err_cdev_del:
    cdev_del(&dev->cdev);
err_ida_remove:
    ida_simple_remove(&movidius_minor_ida, dev->minor);
err_out:
    return ret;
}

static int movidius_platform_remove(struct platform_device *pdev)
{
    struct movidius_x_vpu_dev *dev = platform_get_drvdata(pdev);

    dev_info(&pdev->dev, "platform remove entered\n");

    if (dev->submission_thread)
        kthread_stop(dev->submission_thread);

    device_destroy(movidius_class, dev->devt);
    cdev_del(&dev->cdev);
    ida_simple_remove(&movidius_minor_ida, dev->minor);

    dev_info(&pdev->dev, "Platform device unregistered successfully\n");
    return 0;
}


/* USB Driver */
static int movidius_x_vpu_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct movidius_x_vpu_dev *dev;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    int i;
    int ret;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;
    usb_set_intfdata(interface, dev);

    /* Find endpoints */
    iface_desc = interface->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;
        if (usb_endpoint_is_bulk_in(endpoint))
            dev->bulk_in_endpoint_addr = endpoint->bEndpointAddress;
        if (usb_endpoint_is_bulk_out(endpoint))
            dev->bulk_out_endpoint_addr = endpoint->bEndpointAddress;
    }
    if (!(dev->bulk_in_endpoint_addr && dev->bulk_out_endpoint_addr)) {
        dev_err(&interface->dev, "Could not find bulk-in and bulk-out endpoints\n");
        ret = -ENODEV;
        goto err_free_dev;
    }

    /* Register a platform device */
    dev->pdev = platform_device_alloc(DRIVER_NAME, PLATFORM_DEVID_AUTO);
    if (!dev->pdev) {
        ret = -ENOMEM;
        goto err_free_dev;
    }

    platform_set_drvdata(dev->pdev, dev);
    dev->pdev->dev.parent = &interface->dev;

    ret = platform_device_add(dev->pdev);
    if (ret) {
        platform_device_put(dev->pdev);
        goto err_free_dev;
    }

    spin_lock(&movidius_devices_lock);
    list_add_tail(&dev->global_list, &movidius_devices);
    spin_unlock(&movidius_devices_lock);

    dev_info(&interface->dev, "USB device probed successfully and platform device registered\n");
    return 0;

err_free_dev:
    usb_set_intfdata(interface, NULL);
    usb_put_dev(dev->udev);
    kfree(dev);
    return ret;
}

static void movidius_x_vpu_disconnect(struct usb_interface *interface)
{
    struct movidius_x_vpu_dev *dev = usb_get_intfdata(interface);

    dev_info(&interface->dev, "USB disconnect\n");

    spin_lock(&movidius_devices_lock);
    list_del(&dev->global_list);
    spin_unlock(&movidius_devices_lock);

    platform_device_unregister(dev->pdev);
    usb_set_intfdata(interface, NULL);
    usb_put_dev(dev->udev);
    kfree(dev);
}

static struct usb_device_id movidius_x_vpu_table[] = {
    { .match_flags = USB_DEVICE_ID_MATCH_DEVICE },
    {} /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, movidius_x_vpu_table);

static struct usb_driver movidius_x_vpu_driver = {
    .name = DRIVER_NAME,
    .id_table = movidius_x_vpu_table,
    .probe = movidius_x_vpu_probe,
    .disconnect = movidius_x_vpu_disconnect,
};

/* Module Init and Exit */
static int __init movidius_x_vpu_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&movidius_devt, 0, MAX_DEVICES, DRIVER_NAME);
    if (ret) {
        pr_err("Failed to allocate char device region\n");
        return ret;
    }

    movidius_class = class_create(DRIVER_NAME);
    if (IS_ERR(movidius_class)) {
        ret = PTR_ERR(movidius_class);
        unregister_chrdev_region(movidius_devt, MAX_DEVICES);
        return ret;
    }

    ret = platform_driver_register(&movidius_platform_driver);
    if (ret) {
        class_destroy(movidius_class);
        unregister_chrdev_region(movidius_devt, MAX_DEVICES);
        return ret;
    }

    /* Initialize the USB device ID table with module parameters */
    movidius_x_vpu_table[0].idVendor = vid;
    movidius_x_vpu_table[0].idProduct = pid;

    ret = usb_register(&movidius_x_vpu_driver);
    if (ret) {
        platform_driver_unregister(&movidius_platform_driver);
        class_destroy(movidius_class);
        unregister_chrdev_region(movidius_devt, MAX_DEVICES);
        return ret;
    }
    pr_info(DRIVER_NAME " driver loaded\n");
    return 0;
}

static void __exit movidius_x_vpu_exit(void)
{
    usb_deregister(&movidius_x_vpu_driver);
    platform_driver_unregister(&movidius_platform_driver);
    class_destroy(movidius_class);
    unregister_chrdev_region(movidius_devt, MAX_DEVICES);
    ida_destroy(&movidius_minor_ida);
    pr_info(DRIVER_NAME " driver unloaded\n");
}

module_init(movidius_x_vpu_init);
module_exit(movidius_x_vpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Custom driver for Intel Movidius Myriad X VPU with Platform model");
