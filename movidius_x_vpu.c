#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define VENDOR_ID_INTEL 0x03e7
#define PRODUCT_ID_MYRIAD_X 0x2485

static struct usb_device_id movidius_x_vpu_table[] = {
    { USB_DEVICE(VENDOR_ID_INTEL, PRODUCT_ID_MYRIAD_X) },
    {} /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, movidius_x_vpu_table);

#define MAX_DEV 1

static dev_t dev_num;
static struct class *movidius_class;
static struct cdev movidius_cdev;

#define URB_POOL_SIZE 16

struct urb_pool {
    struct list_head urb_list;
    spinlock_t lock;
};

struct movidius_urb {
    struct urb *urb;
    struct list_head list;
    struct movidius_x_vpu_dev *dev;
};

struct inference_request {
    struct list_head list;
    // Add fields for input/output buffers, completion, etc.
};

struct movidius_x_vpu_dev {
    struct usb_device *udev;
    struct cdev cdev;
    struct urb_pool urb_pool;
    struct list_head request_queue;
    spinlock_t request_queue_lock;
    wait_queue_head_t request_queue_wait;
    struct task_struct *submission_thread;
    // Add more fields for zero-copy implementation
};

static void urb_pool_free(struct movidius_x_vpu_dev *dev); // Forward declaration

static int urb_pool_init(struct movidius_x_vpu_dev *dev)
{
    int i;
    struct movidius_urb *movidius_urb;

    INIT_LIST_HEAD(&dev->urb_pool.urb_list);
    spin_lock_init(&dev->urb_pool.lock);

    for (i = 0; i < URB_POOL_SIZE; i++) {
        movidius_urb = kzalloc(sizeof(*movidius_urb), GFP_KERNEL);
        if (!movidius_urb)
            goto error;

        movidius_urb->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!movidius_urb->urb) {
            kfree(movidius_urb);
            goto error;
        }

        movidius_urb->dev = dev;
        list_add_tail(&movidius_urb->list, &dev->urb_pool.urb_list);
    }
    return 0;

error:
    urb_pool_free(dev);
    return -ENOMEM;
}

static void urb_pool_free(struct movidius_x_vpu_dev *dev)
{
    unsigned long flags;
    struct movidius_urb *movidius_urb, *tmp;

    spin_lock_irqsave(&dev->urb_pool.lock, flags);
    list_for_each_entry_safe(movidius_urb, tmp, &dev->urb_pool.urb_list, list) {
        list_del(&movidius_urb->list);
        usb_free_urb(movidius_urb->urb);
        kfree(movidius_urb);
    }
    spin_unlock_irqrestore(&dev->urb_pool.lock, flags);
}

static struct movidius_urb *get_urb_from_pool(struct movidius_x_vpu_dev *dev)
{
    unsigned long flags;
    struct movidius_urb *movidius_urb = NULL;

    spin_lock_irqsave(&dev->urb_pool.lock, flags);
    if (!list_empty(&dev->urb_pool.urb_list)) {
        movidius_urb = list_first_entry(&dev->urb_pool.urb_list, struct movidius_urb, list);
        list_del(&movidius_urb->list);
    }
    spin_unlock_irqrestore(&dev->urb_pool.lock, flags);
    return movidius_urb;
}

static void return_urb_to_pool(struct movidius_x_vpu_dev *dev, struct movidius_urb *movidius_urb)
{
    unsigned long flags;
    spin_lock_irqsave(&dev->urb_pool.lock, flags);
    list_add_tail(&movidius_urb->list, &dev->urb_pool.urb_list);
    spin_unlock_irqrestore(&dev->urb_pool.lock, flags);
}

static int submission_thread_func(void *data)
{
    struct movidius_x_vpu_dev *dev = data;
    struct inference_request *req;

    while (!kthread_should_stop()) {
        wait_event_interruptible(dev->request_queue_wait,
                                 !list_empty(&dev->request_queue) || kthread_should_stop());

        if (kthread_should_stop())
            break;

        spin_lock_irq(&dev->request_queue_lock);
        if (list_empty(&dev->request_queue)) {
            spin_unlock_irq(&dev->request_queue_lock);
            continue;
        }

        req = list_first_entry(&dev->request_queue, struct inference_request, list);
        list_del(&req->list);
        spin_unlock_irq(&dev->request_queue_lock);

        // Process the request here
        printk(KERN_INFO "Processing inference request\n");
        kfree(req); // For now, just free it
    }

    return 0;
}

static int start_submission_thread(struct movidius_x_vpu_dev *dev)
{
    dev->submission_thread = kthread_run(submission_thread_func, dev, "movidius_submission");
    if (IS_ERR(dev->submission_thread)) {
        printk(KERN_ERR "Failed to create submission thread\n");
        return PTR_ERR(dev->submission_thread);
    }
    return 0;
}

static void stop_submission_thread(struct movidius_x_vpu_dev *dev)
{
    if (dev->submission_thread) {
        kthread_stop(dev->submission_thread);
        dev->submission_thread = NULL;
    }
}

static int movidius_x_vpu_open(struct inode *inode, struct file *file)
{
    struct movidius_x_vpu_dev *dev;
    dev = container_of(inode->i_cdev, struct movidius_x_vpu_dev, cdev);
    file->private_data = dev;
    return 0;
}

static int movidius_x_vpu_release(struct inode *inode, struct file *file)
{
    return 0;
}

static int movidius_x_vpu_mmap(struct file *file, struct vm_area_struct *vma)
{
    // Zero-copy implementation will go here
    printk(KERN_INFO "mmap called\n");
    return -EINVAL; // Not implemented yet
}

#define MOVIDIUS_IOCTL_SUBMIT_INFERENCE _IOW('m', 1, struct inference_request)

static long movidius_x_vpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct movidius_x_vpu_dev *dev = file->private_data;
    struct inference_request *req;
    unsigned long flags;

    switch (cmd) {
    case MOVIDIUS_IOCTL_SUBMIT_INFERENCE:
        req = memdup_user((void __user *)arg, sizeof(*req));
        if (IS_ERR(req))
            return PTR_ERR(req);

        spin_lock_irqsave(&dev->request_queue_lock, flags);
        list_add_tail(&req->list, &dev->request_queue);
        spin_unlock_irqrestore(&dev->request_queue_lock, flags);

        wake_up_interruptible(&dev->request_queue_wait);
        break;
    default:
        return -ENOTTY;
    }

    return 0;
}

static const struct file_operations movidius_x_vpu_fops = {
    .owner = THIS_MODULE,
    .open = movidius_x_vpu_open,
    .release = movidius_x_vpu_release,
    .mmap = movidius_x_vpu_mmap,
    .unlocked_ioctl = movidius_x_vpu_ioctl,
};

static int movidius_x_vpu_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct movidius_x_vpu_dev *dev;
    int ret;

    printk(KERN_INFO "Movidius Myriad X VPU device plugged in\n");

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    INIT_LIST_HEAD(&dev->request_queue);
    spin_lock_init(&dev->request_queue_lock);
    init_waitqueue_head(&dev->request_queue_wait);

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    usb_set_intfdata(interface, dev);

    if (urb_pool_init(dev)) {
        printk(KERN_ERR "urb_pool_init failed\n");
        goto error;
    }

    if (start_submission_thread(dev)) {
        printk(KERN_ERR "start_submission_thread failed\n");
        goto error_urb_pool;
    }

    cdev_init(&dev->cdev, &movidius_x_vpu_fops);
    dev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&dev->cdev, dev_num, 1);
    if (ret) {
        printk(KERN_ERR "cdev_add failed\n");
        goto error_submission_thread;
    }

    device_create(movidius_class, NULL, dev_num, NULL, "movidius_x_vpu");

    return 0;

error_submission_thread:
    stop_submission_thread(dev);
error_urb_pool:
    urb_pool_free(dev);
error:
    kfree(dev);
    return ret;
}

static void free_request_queue(struct movidius_x_vpu_dev *dev)
{
    struct inference_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, &dev->request_queue, list) {
        list_del(&req->list);
        kfree(req);
    }
}

static void movidius_x_vpu_disconnect(struct usb_interface *interface)
{
    struct movidius_x_vpu_dev *dev = usb_get_intfdata(interface);

    printk(KERN_INFO "Movidius Myriad X VPU device unplugged\n");

    stop_submission_thread(dev);
    free_request_queue(dev);
    urb_pool_free(dev);
    device_destroy(movidius_class, dev_num);
    cdev_del(&dev->cdev);
    usb_put_dev(dev->udev);
    kfree(dev);
}

static struct usb_driver movidius_x_vpu_driver = {
    .name = "movidius_x_vpu",
    .id_table = movidius_x_vpu_table,
    .probe = movidius_x_vpu_probe,
    .disconnect = movidius_x_vpu_disconnect,
};

static int __init movidius_x_vpu_init(void)
{
    int result;
    printk(KERN_INFO "Movidius Myriad X VPU driver loading...\n");

    result = alloc_chrdev_region(&dev_num, 0, MAX_DEV, "movidius_x_vpu");
    if (result < 0) {
        printk(KERN_ERR "alloc_chrdev_region failed\n");
        return result;
    }

    movidius_class = class_create("movidius_x_vpu");
    if (IS_ERR(movidius_class)) {
        unregister_chrdev_region(dev_num, MAX_DEV);
        return PTR_ERR(movidius_class);
    }

    result = usb_register(&movidius_x_vpu_driver);
    if (result) {
        printk(KERN_ERR "usb_register failed. Error number %d\n", result);
        class_destroy(movidius_class);
        unregister_chrdev_region(dev_num, MAX_DEV);
    }

    return result;
}

static void __exit movidius_x_vpu_exit(void)
{
    printk(KERN_INFO "Movidius Myriad X VPU driver unloading...\n");
    usb_deregister(&movidius_x_vpu_driver);
    class_destroy(movidius_class);
    unregister_chrdev_region(dev_num, MAX_DEV);
}

module_init(movidius_x_vpu_init);
module_exit(movidius_x_vpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Custom driver for Intel Movidius Myriad X VPU");
