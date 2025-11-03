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

#define VENDOR_ID_INTEL 0x03e7
#define PRODUCT_ID_MYRIAD_X 0x2485

static struct usb_device_id movidius_x_vpu_table[] = {
    { USB_DEVICE(VENDOR_ID_INTEL, PRODUCT_ID_MYRIAD_X) },
    {} /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, movidius_x_vpu_table);

static int submission_cpu = -1;
module_param(submission_cpu, int, 0644);
MODULE_PARM_DESC(submission_cpu, "The CPU to bind the submission thread to. -1 for unbound.");

static int irq_cpu = -1;
module_param(irq_cpu, int, 0644);
MODULE_PARM_DESC(irq_cpu, "The CPU to affinitize USB interrupts to. -1 for unbound.");

#define MAX_DEV 1

static dev_t dev_num;
static struct class *movidius_class;
static struct cdev movidius_cdev;

#define URB_POOL_SIZE 16

struct urb_pool {
    struct list_head urb_list;
    spinlock_t lock;
};

enum movidius_urb_direction {
    MOVIDIUS_URB_OUT,
    MOVIDIUS_URB_IN,
};


struct movidius_urb {
    struct urb *urb;
    struct list_head list;
    struct movidius_x_vpu_dev *dev;
    enum movidius_urb_direction direction;
    struct internal_inference_request *req;
};

struct inference_request {
    size_t input_offset;
    size_t input_size;
    size_t output_offset;
    size_t output_size;
    u64 user_data;
};

#define DMA_BUFFER_SIZE (4 * 1024 * 1024) // 4MB

struct movidius_x_vpu_dev {
    struct usb_device *udev;
    struct cdev cdev;
    struct urb_pool urb_pool;
    struct list_head request_queue;
    spinlock_t request_queue_lock;
    wait_queue_head_t request_queue_wait;
    struct task_struct *submission_thread;
    void *dma_buffer;
    dma_addr_t dma_handle;
    u8 bulk_in_endpoint_addr;
    u8 bulk_out_endpoint_addr;
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
    wake_up_interruptible(&dev->request_queue_wait);
}

static void movidius_x_vpu_urb_complete(struct urb *urb); // Forward declaration

static int submission_thread_func(void *data)
{
    struct movidius_x_vpu_dev *dev = data;
    struct internal_inference_request *req;
    struct movidius_urb *movidius_urb;
    int ret;

    while (!kthread_should_stop()) {
        wait_event_interruptible(dev->request_queue_wait,
                                 !list_empty(&dev->request_queue) || kthread_should_stop());

        if (kthread_should_stop())
            break;

        /* Process as many requests as we have URBs for */
        while (1) {
            movidius_urb = get_urb_from_pool(dev);
            if (!movidius_urb) {
                /* No URBs available, device pipeline is full. Go back to waiting. */
                break;
            }

            spin_lock_irq(&dev->request_queue_lock);
            if (list_empty(&dev->request_queue)) {
                /* No more requests to process. */
                spin_unlock_irq(&dev->request_queue_lock);
                return_urb_to_pool(dev, movidius_urb);
                break;
            }
            req = list_first_entry(&dev->request_queue, struct internal_inference_request, list);
            list_del(&req->list);
            spin_unlock_irq(&dev->request_queue_lock);

            printk(KERN_INFO "Processing inference request: input_offset=%zu, input_size=%zu, output_offset=%zu, output_size=%zu, user_data=%llu\n",
                   req->input_offset, req->input_size, req->output_offset, req->output_size, req->user_data);

            movidius_urb->direction = MOVIDIUS_URB_OUT;
            movidius_urb->req = req;

            usb_fill_bulk_urb(movidius_urb->urb,
                              dev->udev,
                              usb_sndbulkpipe(dev->udev, dev->bulk_out_endpoint_addr),
                              dev->dma_buffer + req->input_offset,
                              req->input_size,
                              movidius_x_vpu_urb_complete,
                              movidius_urb);

            movidius_urb->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
            movidius_urb->urb->transfer_dma = dev->dma_handle + req->input_offset;

            ret = usb_submit_urb(movidius_urb->urb, GFP_KERNEL);
            if (ret) {
                printk(KERN_ERR "Failed to submit URB: %d\n", ret);
                io_uring_cmd_done(req->ioucmd, ret, 0);
                kfree(req);
                return_urb_to_pool(dev, movidius_urb);
                /* Continue to the next request */
            }
        }
    }

    return 0;
}

static void movidius_x_vpu_urb_complete(struct urb *urb)
{
    struct movidius_urb *movidius_urb = urb->context;
    struct movidius_x_vpu_dev *dev = movidius_urb->dev;
    struct internal_inference_request *req = movidius_urb->req;
    int ret;

    if (urb->status) {
        printk(KERN_ERR "URB completed with status %d\n", urb->status);
        io_uring_cmd_done(req->ioucmd, urb->status, 0);
        kfree(req);
        return_urb_to_pool(dev, movidius_urb);
        return;
    }

    if (movidius_urb->direction == MOVIDIUS_URB_OUT) {
        movidius_urb->direction = MOVIDIUS_URB_IN;
        usb_fill_bulk_urb(movidius_urb->urb,
                          dev->udev,
                          usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpoint_addr),
                          dev->dma_buffer + req->output_offset,
                          req->output_size,
                          movidius_x_vpu_urb_complete,
                          movidius_urb);
        movidius_urb->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
        movidius_urb->urb->transfer_dma = dev->dma_handle + req->output_offset;
        ret = usb_submit_urb(movidius_urb->urb, GFP_KERNEL);
        if (ret) {
            printk(KERN_ERR "Failed to submit bulk in URB: %d\n", ret);
            io_uring_cmd_done(req->ioucmd, ret, 0);
            kfree(req);
            return_urb_to_pool(dev, movidius_urb);
        }
    } else {
        printk(KERN_INFO "Inference complete\n");
        io_uring_cmd_done(req->ioucmd, 0, 0);
        kfree(req);
        return_urb_to_pool(dev, movidius_urb);
    }
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
    struct movidius_x_vpu_dev *dev = file->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    if (size > DMA_BUFFER_SIZE)
        return -EINVAL;

    pfn = virt_to_phys(dev->dma_buffer) >> PAGE_SHIFT;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        printk(KERN_ERR "remap_pfn_range failed\n");
        return -EAGAIN;
    }

    return 0;
}

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
};

struct internal_inference_request {
    struct list_head list;
    struct io_uring_cmd *ioucmd;
    size_t input_offset;
    size_t input_size;
    size_t output_offset;
    size_t output_size;
    u64 user_data;
};

static int movidius_x_vpu_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
    struct movidius_x_vpu_dev *dev = ioucmd->file->private_data;
    struct internal_inference_request *req;
    const struct inference_request __user *user_req = (const struct inference_request __user *)(uintptr_t)ioucmd->cmd.addr;
    unsigned long flags;

    switch (ioucmd->cmd.opcode) {
    case MOVIDIUS_URING_CMD_SUBMIT_INFERENCE:
        req = kzalloc(sizeof(*req), GFP_KERNEL);
        if (!req)
            return -ENOMEM;

        if (copy_from_user(&req->input_offset, user_req, sizeof(struct inference_request))) {
            kfree(req);
            return -EFAULT;
        }

        if (req->input_offset + req->input_size > DMA_BUFFER_SIZE ||
            req->output_offset + req->output_size > DMA_BUFFER_SIZE) {
            kfree(req);
            return -EINVAL;
        }

        req->ioucmd = ioucmd;
        io_uring_cmd_set_user_data(ioucmd, req->user_data);

        spin_lock_irqsave(&dev->request_queue_lock, flags);
        list_add_tail(&req->list, &dev->request_queue);
        spin_unlock_irqrestore(&dev->request_queue_lock, flags);

        wake_up_interruptible(&dev->request_queue_wait);
        break;
    default:
        return -EOPNOTSUPP;
    }

    return 0;
}

static const struct file_operations movidius_x_vpu_fops = {
    .owner = THIS_MODULE,
    .open = movidius_x_vpu_open,
    .release = movidius_x_vpu_release,
    .mmap = movidius_x_vpu_mmap,
    .io_uring_cmd = movidius_x_vpu_uring_cmd,
};

static int movidius_x_vpu_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct movidius_x_vpu_dev *dev;
    int ret = 0;

    printk(KERN_INFO "Movidius Myriad X VPU device plugged in\n");

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    INIT_LIST_HEAD(&dev->request_queue);
    spin_lock_init(&dev->request_queue_lock);
    init_waitqueue_head(&dev->request_queue_wait);

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    usb_set_intfdata(interface, dev);

    dev->dma_buffer = usb_alloc_coherent(dev->udev, DMA_BUFFER_SIZE, GFP_KERNEL, &dev->dma_handle);
    if (!dev->dma_buffer) {
        printk(KERN_ERR "Failed to allocate DMA buffer\n");
        goto error;
    }

    if (urb_pool_init(dev)) {
        printk(KERN_ERR "urb_pool_init failed\n");
        goto error_dma_buffer;
    }

    if (start_submission_thread(dev)) {
        printk(KERN_ERR "start_submission_thread failed\n");
        goto error_urb_pool;
    }

    if (submission_cpu != -1 && submission_cpu < num_possible_cpus()) {
        kthread_bind(dev->submission_thread, submission_cpu);
        printk(KERN_INFO "Submission thread bound to CPU %d\n", submission_cpu);
    }

    if (irq_cpu != -1 && irq_cpu < num_possible_cpus()) {
        struct usb_hcd *hcd = bus_to_hcd(dev->udev->bus);
        if (hcd->irq > 0) {
            if (irq_set_affinity_hint(hcd->irq, cpumask_of(irq_cpu)) == 0)
                printk(KERN_INFO "IRQ %d affinity set to CPU %d\n", hcd->irq, irq_cpu);
            else
                printk(KERN_WARNING "Failed to set IRQ affinity for IRQ %d\n", hcd->irq);
        }
    }

    cdev_init(&dev->cdev, &movidius_x_vpu_fops);
    dev->cdev.owner = THIS_MODULE;

    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    int i;

    iface_desc = interface->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;
        if (!dev->bulk_in_endpoint_addr &&
            usb_endpoint_is_bulk_in(endpoint)) {
            dev->bulk_in_endpoint_addr = endpoint->bEndpointAddress;
        }
        if (!dev->bulk_out_endpoint_addr &&
            usb_endpoint_is_bulk_out(endpoint)) {
            dev->bulk_out_endpoint_addr = endpoint->bEndpointAddress;
        }
    }
    if (!(dev->bulk_in_endpoint_addr && dev->bulk_out_endpoint_addr)) {
        printk(KERN_ERR "Could not find bulk-in and bulk-out endpoints\n");
        goto error_submission_thread;
    }

    ret = cdev_add(&dev->cdev, dev_num, 1);
    if (ret) {
        printk(KERN_ERR "cdev_add failed\n");
        goto error_submission_thread;
    }

    struct device *device = device_create(movidius_class, NULL, dev_num, NULL, "movidius_x_vpu");
    if (IS_ERR(device)) {
        printk(KERN_ERR "device_create failed\n");
        ret = PTR_ERR(device);
        goto error_cdev;
    }

    return 0;

error_cdev:
    cdev_del(&dev->cdev);
error_submission_thread:
    stop_submission_thread(dev);
error_urb_pool:
    urb_pool_free(dev);
error_dma_buffer:
    usb_free_coherent(dev->udev, DMA_BUFFER_SIZE, dev->dma_buffer, dev->dma_handle);
error:
    kfree(dev);
    return ret;
}

static void movidius_x_vpu_disconnect(struct usb_interface *interface)
{
    struct movidius_x_vpu_dev *dev = usb_get_intfdata(interface);

    printk(KERN_INFO "Movidius Myriad X VPU device unplugged\n");

    stop_submission_thread(dev);
    urb_pool_free(dev);
    usb_free_coherent(dev->udev, DMA_BUFFER_SIZE, dev->dma_buffer, dev->dma_handle);
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
