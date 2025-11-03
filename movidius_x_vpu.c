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

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
#error "movidius_x_vpu needs >= 5.12 for io_uring_cmd"
#endif

#include <linux/firmware.h>
#include <linux/pm_runtime.h>

#define MOVIDIUS_UAPI_VERSION 1

static ushort vid = 0x03e7;
module_param(vid, ushort, 0444);
MODULE_PARM_DESC(vid, "USB Vendor ID (default 0x03e7)");

static ushort pid = 0x2485;
module_param(pid, ushort, 0444);
MODULE_PARM_DESC(pid, "USB Product ID (default 0x2485)");

static ulong dma_buf_size = 4UL * 1024 * 1024; /* 4 MiB default */
module_param(dma_buf_size, ulong, 0644);
MODULE_PARM_DESC(dma_buf_size, "DMA buffer size in bytes");

static struct usb_device_id movidius_x_vpu_table[] = {
    { USB_DEVICE(vid, pid) },
    {} /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, movidius_x_vpu_table);

static int submission_cpu = -1;
module_param(submission_cpu, int, 0644);
MODULE_PARM_DESC(submission_cpu, "The CPU to bind the submission thread to. -1 for unbound.");

static int irq_cpu = -1;
module_param(irq_cpu, int, 0644);
MODULE_PARM_DESC(irq_cpu, "The CPU to affinitize USB interrupts to. -1 for unbound.");

static char *fw_name = "movidius-x-vpu.fw";
module_param(fw_name, charp, 0444);
MODULE_PARM_DESC(fw_name, "The name of the firmware file to load.");

static dev_t dev_num;
static struct class *movidius_class;
static DEFINE_IDR(movidius_idr);
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

#define MAX_SG_SEGMENTS 16

struct movidius_sg_segment {
    __u32 offset;
    __u32 len;
};

struct movidius_cmd_hdr {
    __u16 version;
    __u16 op;
    __u32 len;
};

struct inference_request {
    struct movidius_cmd_hdr hdr;
    __u32 num_input_segs;
    __u32 num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];
    u64 user_data;
};

struct movidius_x_vpu_dev {
    struct device *dev;
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
    atomic_t pending_reqs;
    atomic_t completed_reqs;
    atomic_t reset_count;
    struct kobject kobj;
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

            int i;

            printk(KERN_INFO "Processing SG inference request: num_input_segs=%u, user_data=%llu\n",
                   req->num_input_segs, req->user_data);

            req->sg = kcalloc(req->num_input_segs, sizeof(struct scatterlist), GFP_KERNEL);
            if (!req->sg) {
                io_uring_cmd_done(req->ioucmd, -ENOMEM, 0);
                kfree(req);
                return_urb_to_pool(dev, movidius_urb);
                continue;
            }

            sg_init_table(req->sg, req->num_input_segs);

            for (i = 0; i < req->num_input_segs; i++) {
                sg_set_buf(&req->sg[i], dev->dma_buffer + req->input_segs[i].offset, req->input_segs[i].len);
            }

            movidius_urb->direction = MOVIDIUS_URB_OUT;
            movidius_urb->req = req;

            usb_fill_bulk_urb(movidius_urb->urb,
                              dev->udev,
                              usb_sndbulkpipe(dev->udev, dev->bulk_out_endpoint_addr),
                              NULL, /* We are using SG, so this is NULL */
                              0,    /* And this is 0 */
                              movidius_x_vpu_urb_complete,
                              movidius_urb);

            movidius_urb->urb->num_sgs = req->num_input_segs;
            movidius_urb->urb->sg = req->sg;

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
    u64 count;

    if (urb->status) {
        printk(KERN_ERR "URB completed with status %d\n", urb->status);
        kfree(req->sg);
        count = io_uring_cmd_get_user_data(req->ioucmd);
        if (atomic_dec_and_test((atomic_t *)&count))
            io_uring_cmd_done(req->ioucmd, urb->status, 0);
        atomic_inc(&dev->completed_reqs);
        kfree(req);
        return_urb_to_pool(dev, movidius_urb);
        return;
    }

    if (movidius_urb->direction == MOVIDIUS_URB_OUT) {
        int i;

        kfree(req->sg);

        req->sg = kcalloc(req->num_output_segs, sizeof(struct scatterlist), GFP_KERNEL);
        if (!req->sg) {
            count = io_uring_cmd_get_user_data(req->ioucmd);
            if (atomic_dec_and_test((atomic_t *)&count))
                io_uring_cmd_done(req->ioucmd, -ENOMEM, 0);
            kfree(req);
            return_urb_to_pool(dev, movidius_urb);
            return;
        }

        sg_init_table(req->sg, req->num_output_segs);

        for (i = 0; i < req->num_output_segs; i++) {
            sg_set_buf(&req->sg[i], dev->dma_buffer + req->output_segs[i].offset, req->output_segs[i].len);
        }

        movidius_urb->direction = MOVIDIUS_URB_IN;
        usb_fill_bulk_urb(movidius_urb->urb,
                          dev->udev,
                          usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpoint_addr),
                          NULL,
                          0,
                          movidius_x_vpu_urb_complete,
                          movidius_urb);
        movidius_urb->urb->num_sgs = req->num_output_segs;
        movidius_urb->urb->sg = req->sg;
        ret = usb_submit_urb(movidius_urb->urb, GFP_KERNEL);
        if (ret) {
            printk(KERN_ERR "Failed to submit bulk in URB: %d\n", ret);
            kfree(req->sg);
            count = io_uring_cmd_get_user_data(req->ioucmd);
            if (atomic_dec_and_test((atomic_t *)&count))
                io_uring_cmd_done(req->ioucmd, ret, 0);
            kfree(req);
            return_urb_to_pool(dev, movidius_urb);
        }
    } else {
        printk(KERN_INFO "Inference complete\n");
        kfree(req->sg);
        count = io_uring_cmd_get_user_data(req->ioucmd);
        if (atomic_dec_and_test((atomic_t *)&count))
            io_uring_cmd_done(req->ioucmd, 0, 0);
        atomic_inc(&dev->completed_reqs);
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

    if (size > dma_buf_size)
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
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
};

struct batch_inference_request {
    struct movidius_cmd_hdr hdr;
    __u32 count;
    __u32 pad;
    __u64 reqs;
};

struct internal_inference_request {
    struct list_head list;
    struct io_uring_cmd *ioucmd;
    __u32 num_input_segs;
    __u32 num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];
    u64 user_data;
    struct scatterlist *sg;
};

static int movidius_x_vpu_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
    struct movidius_x_vpu_dev *dev = ioucmd->file->private_data;
    struct internal_inference_request *req;
    unsigned long flags;

    switch (ioucmd->cmd.opcode) {
    case MOVIDIUS_URING_CMD_SUBMIT_INFERENCE: {
        const struct inference_request __user *user_req = (const struct inference_request __user *)(uintptr_t)ioucmd->cmd.addr;
        int i;
        struct movidius_cmd_hdr hdr;

        if (copy_from_user(&hdr, user_req, sizeof(hdr)))
            return -EFAULT;

        if (hdr.version != MOVIDIUS_UAPI_VERSION)
            return -EINVAL;

        req = kzalloc(sizeof(*req), GFP_KERNEL);
        if (!req)
            return -ENOMEM;

        if (copy_from_user(req, user_req, sizeof(struct inference_request))) {
            kfree(req);
            return -EFAULT;
        }

        if (req->num_input_segs > MAX_SG_SEGMENTS || req->num_output_segs > MAX_SG_SEGMENTS) {
            kfree(req);
            return -EINVAL;
        }

        for (i = 0; i < req->num_input_segs; i++) {
            if (req->input_segs[i].offset + req->input_segs[i].len > dma_buf_size) {
                kfree(req);
                return -EINVAL;
            }
        }

        for (i = 0; i < req->num_output_segs; i++) {
            if (req->output_segs[i].offset + req->output_segs[i].len > dma_buf_size) {
                kfree(req);
                return -EINVAL;
            }
        }

        req->ioucmd = ioucmd;
        atomic_inc(&dev->pending_reqs);
        io_uring_cmd_set_user_data(ioucmd, 1);

        spin_lock_irqsave(&dev->request_queue_lock, flags);
        list_add_tail(&req->list, &dev->request_queue);
        spin_unlock_irqrestore(&dev->request_queue_lock, flags);

        wake_up_interruptible(&dev->request_queue_wait);
        break;
    }
    case MOVIDIUS_URING_CMD_SUBMIT_BATCH: {
        struct batch_inference_request batch_req;
        struct inference_request __user *user_reqs;
        int i, j;
        struct movidius_cmd_hdr hdr;

        if (copy_from_user(&hdr, (void __user *)(uintptr_t)ioucmd->cmd.addr, sizeof(hdr)))
            return -EFAULT;

        if (hdr.version != MOVIDIUS_UAPI_VERSION)
            return -EINVAL;

        if (copy_from_user(&batch_req, (void __user *)(uintptr_t)ioucmd->cmd.addr, sizeof(batch_req)))
            return -EFAULT;

        if (batch_req.count > URB_POOL_SIZE)
            return -EINVAL;

        io_uring_cmd_set_user_data(ioucmd, batch_req.count);

        user_reqs = (struct inference_request __user *)(uintptr_t)batch_req.reqs;

        for (i = 0; i < batch_req.count; i++) {
            req = kzalloc(sizeof(*req), GFP_KERNEL);
            if (!req)
                return -ENOMEM;

            if (copy_from_user(req, &user_reqs[i], sizeof(struct inference_request))) {
                kfree(req);
                return -EFAULT;
            }

            if (req->num_input_segs > MAX_SG_SEGMENTS || req->num_output_segs > MAX_SG_SEGMENTS) {
                kfree(req);
                return -EINVAL;
            }

            for (j = 0; j < req->num_input_segs; j++) {
                if (req->input_segs[j].offset + req->input_segs[j].len > dma_buf_size) {
                    kfree(req);
                    return -EINVAL;
                }
            }

            for (j = 0; j < req->num_output_segs; j++) {
                if (req->output_segs[j].offset + req->output_segs[j].len > dma_buf_size) {
                    kfree(req);
                    return -EINVAL;
                }
            }
            req->ioucmd = ioucmd;
            atomic_inc(&dev->pending_reqs);

            spin_lock_irqsave(&dev->request_queue_lock, flags);
            list_add_tail(&req->list, &dev->request_queue);
            spin_unlock_irqrestore(&dev->request_queue_lock, flags);
        }
        wake_up_interruptible(&dev->request_queue_wait);
        break;
    }
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
    atomic_set(&dev->pending_reqs, 0);
    atomic_set(&dev->completed_reqs, 0);
    atomic_set(&dev->reset_count, 0);

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    usb_set_intfdata(interface, dev);

    dev->dma_buffer = usb_alloc_coherent(dev->udev, dma_buf_size, GFP_KERNEL, &dev->dma_handle);
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

    const struct firmware *fw;
    if (request_firmware(&fw, fw_name, &interface->dev) == 0) {
        printk(KERN_INFO "Firmware loaded, size %zu\n", fw->size);
        /* Here we would send the firmware to the device */
        release_firmware(fw);
    } else {
        dev_warn(&interface->dev, "no firmware found, continuing bare\n");
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

    int minor = idr_alloc(&movidius_idr, dev, 0, 0, GFP_KERNEL);
    if (minor < 0) {
        ret = minor;
        goto error_cdev;
    }

    dev->dev = device_create(movidius_class, NULL, MKDEV(MAJOR(dev_num), minor), NULL, "movidius_x_vpu%d", minor);
    if (IS_ERR(dev->dev)) {
        printk(KERN_ERR "device_create failed\n");
        ret = PTR_ERR(dev->dev);
        idr_remove(&movidius_idr, minor);
        goto error_cdev;
    }

    usb_enable_autosuspend(dev->udev);
    interface->needs_remote_wakeup = 1;
    pm_runtime_set_active(&interface->dev);
    pm_runtime_enable(&interface->dev);

    ret = kobject_init_and_add(&dev->kobj, &movidius_ktype, &dev->dev->kobj, "telemetry");
    if (ret) {
        printk(KERN_ERR "kobject_init_and_add failed\n");
        goto error_device;
    }

    ret = sysfs_create_group(&dev->kobj, &attr_group);
    if (ret) {
        printk(KERN_ERR "sysfs_create_group failed\n");
        goto error_kobject;
    }

    return 0;

error_kobject:
    kobject_put(&dev->kobj);
error_device:
    device_destroy(movidius_class, dev_num);
error_cdev:
    cdev_del(&dev->cdev);
error_submission_thread:
    stop_submission_thread(dev);
error_urb_pool:
    urb_pool_free(dev);
error_dma_buffer:
    usb_free_coherent(dev->udev, dma_buf_size, dev->dma_buffer, dev->dma_handle);
error:
    kfree(dev);
    return ret;
}

static ssize_t pending_reqs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, kobj);
    return sprintf(buf, "%d\n", atomic_read(&dev->pending_reqs));
}

static ssize_t completed_reqs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, kobj);
    return sprintf(buf, "%d\n", atomic_read(&dev->completed_reqs));
}

static ssize_t reset_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, kobj);
    return sprintf(buf, "%d\n", atomic_read(&dev->reset_count));
}

static ssize_t dma_buf_size_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%lu\n", dma_buf_size);
}

static struct kobj_attribute pending_reqs_attribute = __ATTR_RO(pending_reqs);
static struct kobj_attribute completed_reqs_attribute = __ATTR_RO(completed_reqs);
static struct kobj_attribute reset_count_attribute = __ATTR_RO(reset_count);
static struct kobj_attribute dma_buf_size_attribute = __ATTR_RO(dma_buf_size);

static struct attribute *attrs[] = {
    &pending_reqs_attribute.attr,
    &completed_reqs_attribute.attr,
    &reset_count_attribute.attr,
    &dma_buf_size_attribute.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static void movidius_x_vpu_release_sysfs(struct kobject *kobj)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, kobj);
    usb_put_dev(dev->udev);
}

static struct kobj_type movidius_ktype = {
    .sysfs_ops = &kobj_sysfs_ops,
    .release = movidius_x_vpu_release_sysfs,
};

static void movidius_x_vpu_disconnect(struct usb_interface *interface)
{
    struct movidius_x_vpu_dev *dev = usb_get_intfdata(interface);

    printk(KERN_INFO "Movidius Myriad X VPU device unplugged\n");

    pm_runtime_disable(&interface->dev);
    stop_submission_thread(dev);
    kobject_put(&dev->kobj);
    urb_pool_free(dev);
    usb_free_coherent(dev->udev, dma_buf_size, dev->dma_buffer, dev->dma_handle);
    device_destroy(movidius_class, dev->dev->devt);
    idr_remove(&movidius_idr, MINOR(dev->dev->devt));
    cdev_del(&dev->cdev);
    usb_put_dev(dev->udev);
    kfree(dev);
}

static int movidius_x_vpu_post_reset(struct usb_interface *intf)
{
    struct movidius_x_vpu_dev *dev = usb_get_intfdata(intf);

    printk(KERN_INFO "Movidius Myriad X VPU device reset\n");
    atomic_inc(&dev->reset_count);

    return 0;
}

static int movidius_x_vpu_runtime_suspend(struct usb_interface *intf, pm_message_t message)
{
    printk(KERN_INFO "Movidius Myriad X VPU runtime suspend\n");
    return 0;
}

static int movidius_x_vpu_runtime_resume(struct usb_interface *intf)
{
    printk(KERN_INFO "Movidius Myriad X VPU runtime resume\n");
    return 0;
}

static int movidius_x_vpu_runtime_idle(struct usb_interface *intf)
{
    printk(KERN_INFO "Movidius Myriad X VPU runtime idle\n");
    return 0;
}

static struct usb_driver movidius_x_vpu_driver = {
    .name = "movidius_x_vpu",
    .id_table = movidius_x_vpu_table,
    .probe = movidius_x_vpu_probe,
    .disconnect = movidius_x_vpu_disconnect,
    .post_reset = movidius_x_vpu_post_reset,
    .supports_autosuspend = 1,
    .runtime_suspend = movidius_x_vpu_runtime_suspend,
    .runtime_resume = movidius_x_vpu_runtime_resume,
    .runtime_idle = movidius_x_vpu_runtime_idle,
};

static int __init movidius_x_vpu_init(void)
{
    int result;
    printk(KERN_INFO "Movidius Myriad X VPU driver loading...\n");

    result = alloc_chrdev_region(&dev_num, 0, 0, "movidius_x_vpu");
    if (result < 0) {
        printk(KERN_ERR "alloc_chrdev_region failed\n");
        return result;
    }

    movidius_class = class_create("movidius_x_vpu");
    if (IS_ERR(movidius_class)) {
        unregister_chrdev_region(dev_num, 0);
        return PTR_ERR(movidius_class);
    }

    result = usb_register(&movidius_x_vpu_driver);
    if (result) {
        printk(KERN_ERR "usb_register failed. Error number %d\n", result);
        class_destroy(movidius_class);
        unregister_chrdev_region(dev_num, 0);
    }

    return result;
}

static void __exit movidius_x_vpu_exit(void)
{
    printk(KERN_INFO "Movidius Myriad X VPU driver unloading...\n");
    usb_deregister(&movidius_x_vpu_driver);
    class_destroy(movidius_class);
    unregister_chrdev_region(dev_num, 0);
}

module_init(movidius_x_vpu_init);
module_exit(movidius_x_vpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Custom driver for Intel Movidius Myriad X VPU");
