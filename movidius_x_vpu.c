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
#include <linux/hrtimer.h>

#define MOVIDIUS_UAPI_VERSION 1

/* Module Parameters */
static ushort vid = 0x03e7;
module_param(vid, ushort, 0444);
MODULE_PARM_DESC(vid, "USB Vendor ID (default 0x03e7)");

static ushort pid = 0x2485;
module_param(pid, ushort, 0444);
MODULE_PARM_DESC(pid, "USB Product ID (default 0x2485)");

static int submission_cpu = -1;
module_param(submission_cpu, int, 0644);
MODULE_PARM_DESC(submission_cpu, "The CPU to bind the submission thread to. -1 for unbound.");

static int irq_cpu = -1;
module_param(irq_cpu, int, 0644);
MODULE_PARM_DESC(irq_cpu, "The CPU to affinitize USB interrupts to. -1 for unbound.");

static struct usb_device_id movidius_x_vpu_table[] = {
    { USB_DEVICE(vid, pid) },
    {} /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, movidius_x_vpu_table);

static char *fw_name = "movidius-x-vpu.fw";
module_param(fw_name, charp, 0444);
MODULE_PARM_DESC(fw_name, "The name of the firmware file to load.");

static int batch_delay_ms = 1;
module_param(batch_delay_ms, int, 0644);
MODULE_PARM_DESC(batch_delay_ms, "The maximum time in ms to wait for a batch to fill up.");

static int urb_pool_size = 16;
module_param(urb_pool_size, int, 0644);
MODULE_PARM_DESC(urb_pool_size, "The number of URBs to pre-allocate in the pool.");

static int batch_high_watermark = 8;
module_param(batch_high_watermark, int, 0644);
MODULE_PARM_DESC(batch_high_watermark, "The number of requests in the queue that will trigger an immediate batch submission.");

/* Data Structures */
#define MAX_SG_SEGMENTS 16
#define MAX_REQS_PER_URB 16

struct movidius_x_vpu_dev;

enum movidius_urb_direction {
    MOVIDIUS_URB_OUT,
    MOVIDIUS_URB_IN,
};

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

struct batch_job {
    struct io_uring_cmd *ioucmd;
    atomic_t pending_reqs;
};

struct internal_inference_request {
    struct list_head list;
    u64 user_data;

    __u32 num_input_segs;
    __u32 num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];

    struct batch_job *job;
};

struct movidius_urb {
    struct urb *urb;
    struct list_head list; /* for the pool */
    struct movidius_x_vpu_dev *dev;
    enum movidius_urb_direction direction;

    /* Context for a specific transfer */
    int num_reqs;
    struct internal_inference_request *reqs[MAX_REQS_PER_URB];
    struct scatterlist *sg;
};

struct urb_pool {
    struct list_head urb_list;
    spinlock_t lock;
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
    u8 bulk_in_endpoint_addr;
    u8 bulk_out_endpoint_addr;
    atomic_t pending_reqs;
    atomic_t completed_reqs;
    atomic_t reset_count;
    struct kobject kobj;
    struct hrtimer batch_timer;
    struct work_struct batch_work;

    /* User-managed DMA arena */
    struct scatterlist *sg_table;
    struct page **pages;
    int num_pages;

    struct list_head global_list;
};

/* Global Variables */
static dev_t dev_num;
static struct class *movidius_class;
static DEFINE_IDR(movidius_idr);
static struct cdev movidius_cdev;
static struct device *movidius_master_dev;

static LIST_HEAD(movidius_devices);
static DEFINE_SPINLOCK(movidius_devices_lock);
static struct list_head *movidius_device_rr_cursor = &movidius_devices;

static struct movidius_x_vpu_dev *get_next_movidius_dev(void)
{
    struct movidius_x_vpu_dev *dev;
    unsigned long flags;

    spin_lock_irqsave(&movidius_devices_lock, flags);
    if (list_empty(&movidius_devices)) {
        spin_unlock_irqrestore(&movidius_devices_lock, flags);
        return NULL;
    }

    movidius_device_rr_cursor = movidius_device_rr_cursor->next;
    if (movidius_device_rr_cursor == &movidius_devices)
        movidius_device_rr_cursor = movidius_device_rr_cursor->next;

    dev = list_entry(movidius_device_rr_cursor, struct movidius_x_vpu_dev, global_list);
    spin_unlock_irqrestore(&movidius_devices_lock, flags);

    return dev;
}

/* Function Prototypes */
static void urb_pool_free(struct movidius_x_vpu_dev *dev);
static void movidius_x_vpu_urb_complete(struct urb *urb);
static void movidius_x_vpu_batch_work(struct work_struct *work);
static enum hrtimer_restart movidius_x_vpu_batch_timer(struct hrtimer *timer);

/* URB Pool Management */
static int urb_pool_init(struct movidius_x_vpu_dev *dev)
{
    int i;
    struct movidius_urb *movidius_urb;

    INIT_LIST_HEAD(&dev->urb_pool.urb_list);
    spin_lock_init(&dev->urb_pool.lock);

    for (i = 0; i < urb_pool_size; i++) {
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

/* Submission Thread */
static int submission_thread_func(void *data)
{
    struct movidius_x_vpu_dev *dev = data;
    struct internal_inference_request *req, *tmp;
    struct movidius_urb *movidius_urb;
    int ret;

    while (!kthread_should_stop()) {
        wait_event_interruptible(dev->request_queue_wait,
                                 !list_empty(&dev->request_queue) || kthread_should_stop());

        if (kthread_should_stop())
            break;

        /* A batch is ready, either by size or by timeout. */
        hrtimer_cancel(&dev->batch_timer);
        cancel_work_sync(&dev->batch_work);

        while (1) {
            struct list_head req_list;
            int num_reqs = 0;
            int total_segs = 0;

            INIT_LIST_HEAD(&req_list);

            movidius_urb = get_urb_from_pool(dev);
            if (!movidius_urb) {
                break;
            }

            spin_lock_irq(&dev->request_queue_lock);
            list_for_each_entry_safe(req, tmp, &dev->request_queue, list) {
                if (num_reqs > 0 && req->job != list_first_entry(&req_list, struct internal_inference_request, list)->job)
                    break; /* Don't mix requests from different jobs */

                if (num_reqs == MAX_REQS_PER_URB || total_segs + req->num_input_segs > MAX_SG_SEGMENTS)
                    break;

                list_move_tail(&req->list, &req_list);
                num_reqs++;
                total_segs += req->num_input_segs;
            }
            spin_unlock_irq(&dev->request_queue_lock);

            if (num_reqs == 0) {
                return_urb_to_pool(dev, movidius_urb);
                break;
            }

            movidius_urb->num_reqs = num_reqs;
            movidius_urb->sg = kcalloc(total_segs, sizeof(struct scatterlist), GFP_KERNEL);
            if (!movidius_urb->sg) {
                /* Return requests to queue and try again later */
                spin_lock_irq(&dev->request_queue_lock);
                list_splice_tail(&req_list, &dev->request_queue);
                spin_unlock_irq(&dev->request_queue_lock);
                return_urb_to_pool(dev, movidius_urb);
                break;
            }
            sg_init_table(movidius_urb->sg, total_segs);

            int current_req = 0;
            int current_sg = 0;

            list_for_each_entry(req, &req_list, list) {
                movidius_urb->reqs[current_req++] = req;
                for (int i = 0; i < req->num_input_segs; i++) {
                    sg_set_page(&movidius_urb->sg[current_sg],
                                dev->pages[(req->input_segs[i].offset) / PAGE_SIZE],
                                req->input_segs[i].len,
                                (req->input_segs[i].offset) % PAGE_SIZE);
                    current_sg++;
                }
            }
            total_segs = current_sg;

            movidius_urb->direction = MOVIDIUS_URB_OUT;

            usb_fill_bulk_urb(movidius_urb->urb,
                              dev->udev,
                              usb_sndbulkpipe(dev->udev, dev->bulk_out_endpoint_addr),
                              NULL, 0,
                              movidius_x_vpu_urb_complete,
                              movidius_urb);

            movidius_urb->urb->num_sgs = total_segs;
            movidius_urb->urb->sg = movidius_urb->sg;

            ret = usb_submit_urb(movidius_urb->urb, GFP_KERNEL);
            if (ret) {
                printk(KERN_ERR "Failed to submit URB: %d\n", ret);
                /* Error handling: complete all requests in the batch with an error */
                for (int i = 0; i < num_reqs; i++) {
                    if (atomic_dec_and_test(&movidius_urb->reqs[i]->job->pending_reqs)) {
                        io_uring_cmd_done(movidius_urb->reqs[i]->job->ioucmd, ret, 0);
                        kfree(movidius_urb->reqs[i]->job);
                    }
                    kfree(movidius_urb->reqs[i]);
                }
                kfree(movidius_urb->sg);
                return_urb_to_pool(dev, movidius_urb);
            }
        }
    }

    return 0;
    }
    case MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA: {
        if (dev) {
            target_dev = dev;
        } else {
            target_dev = list_first_entry_or_null(&movidius_devices, struct movidius_x_vpu_dev, global_list);
            if (!target_dev)
                return -ENODEV;
        }

        if (!target_dev->sg_table)
            return 0;

        if (dev) { /* If not master device */
            dma_unmap_sg(&target_dev->udev->dev, target_dev->sg_table, target_dev->num_pages, DMA_BIDIRECTIONAL);
        } else { /* Master device, unmap for all devices */
            list_for_each_entry(target_dev, &movidius_devices, global_list) {
                dma_unmap_sg(&target_dev->udev->dev, target_dev->sg_table, target_dev->num_pages, DMA_BIDIRECTIONAL);
            }
        }

        unpin_user_pages(target_dev->pages, target_dev->num_pages);
        kfree(target_dev->pages);
        kfree(target_dev->sg_table);
        target_dev->sg_table = NULL;
        target_dev->pages = NULL;
        target_dev->num_pages = 0;

        return 0;
    }
    }
    return -EINVAL;
}

/* URB Completion Handler */
static void movidius_x_vpu_urb_complete(struct urb *urb)
{
    struct movidius_urb *movidius_urb = urb->context;
    struct movidius_x_vpu_dev *dev = movidius_urb->dev;
    int ret;

    if (urb->status) {
        printk(KERN_ERR "URB completed with status %d\n", urb->status);
        for (int i = 0; i < movidius_urb->num_reqs; i++) {
            struct internal_inference_request *req = movidius_urb->reqs[i];
            if (atomic_dec_and_test(&req->job->pending_reqs)) {
                io_uring_cmd_done(req->job->ioucmd, urb->status, 0);
                kfree(req->job);
            }
            kfree(req);
        }
        kfree(movidius_urb->sg);
        return_urb_to_pool(dev, movidius_urb);
        return;
    }

    if (movidius_urb->direction == MOVIDIUS_URB_OUT) {
        printk(KERN_INFO "movidius_x_vpu%d: processing batch of %d requests\n",
               iminor(dev->dev->inode), movidius_urb->num_reqs);

        int total_segs = 0;
        for (int i = 0; i < movidius_urb->num_reqs; i++)
            total_segs += movidius_urb->reqs[i]->num_output_segs;

        kfree(movidius_urb->sg);
        movidius_urb->sg = kcalloc(total_segs, sizeof(struct scatterlist), GFP_KERNEL);
        if (!movidius_urb->sg) {
            /* Error handling: complete all requests with an error */
            for (int i = 0; i < movidius_urb->num_reqs; i++) {
                 struct internal_inference_request *req = movidius_urb->reqs[i];
                if (atomic_dec_and_test(&req->job->pending_reqs)) {
                    io_uring_cmd_done(req->job->ioucmd, -ENOMEM, 0);
                    kfree(req->job);
                }
                kfree(req);
            }
            return_urb_to_pool(dev, movidius_urb);
            return;
        }
        sg_init_table(movidius_urb->sg, total_segs);

        int current_sg = 0;
        for (int i = 0; i < movidius_urb->num_reqs; i++) {
            struct internal_inference_request *req = movidius_urb->reqs[i];
            for (int j = 0; j < req->num_output_segs; j++, current_sg++) {
                sg_set_page(&movidius_urb->sg[current_sg], dev->pages[req->output_segs[j].offset / PAGE_SIZE],
                            req->output_segs[j].len, req->output_segs[j].offset % PAGE_SIZE);
            }
        }

        movidius_urb->direction = MOVIDIUS_URB_IN;
        usb_fill_bulk_urb(movidius_urb->urb, dev->udev,
                          usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpoint_addr),
                          NULL, 0, movidius_x_vpu_urb_complete, movidius_urb);
        movidius_urb->urb->num_sgs = total_segs;
        movidius_urb->urb->sg = movidius_urb->sg;

        ret = usb_submit_urb(movidius_urb->urb, GFP_KERNEL);
        if (ret) {
            printk(KERN_ERR "Failed to submit bulk in URB: %d\n", ret);
            for (int i = 0; i < movidius_urb->num_reqs; i++) {
                struct internal_inference_request *req = movidius_urb->reqs[i];
                if (atomic_dec_and_test(&req->job->pending_reqs)) {
                    io_uring_cmd_done(req->job->ioucmd, ret, 0);
                    kfree(req->job);
                }
                kfree(req);
            }
            kfree(movidius_urb->sg);
            return_urb_to_pool(dev, movidius_urb);
        }
    } else {
        printk(KERN_INFO "Batch of %d inferences complete\n", movidius_urb->num_reqs);
        for (int i = 0; i < movidius_urb->num_reqs; i++) {
            struct internal_inference_request *req = movidius_urb->reqs[i];
            atomic_inc(&dev->completed_reqs);
            if (atomic_dec_and_test(&req->job->pending_reqs)) {
                io_uring_cmd_done(req->job->ioucmd, 0, 0);
                kfree(req->job);
            }
            kfree(req);
        }
        kfree(movidius_urb->sg);
        return_urb_to_pool(dev, movidius_urb);
    }
}

/* File Operations */
static int movidius_x_vpu_open(struct inode *inode, struct file *file)
{
    if (imajor(inode) == MAJOR(dev_num) && iminor(inode) != 0) {
        struct movidius_x_vpu_dev *dev;
        dev = container_of(inode->i_cdev, struct movidius_x_vpu_dev, cdev);
        file->private_data = dev;
    }
    return 0;
}

static int movidius_x_vpu_release(struct inode *inode, struct file *file)
{
    return 0;
}

#define MOVIDIUS_IOCTL_REGISTER_DMA_ARENA _IOW('M', 1, struct movidius_dma_arena)
#define MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA _IO('M', 2)

struct movidius_dma_arena {
    __u64 addr;
    __u64 len;
};

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

static int movidius_x_vpu_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
    struct movidius_x_vpu_dev *dev = ioucmd->file->private_data;
    struct movidius_x_vpu_dev *target_dev;
    struct internal_inference_request *req, *tmp;
    unsigned long flags;
    int req_count = 0;

    if (dev) {
        target_dev = dev;
    } else {
        target_dev = get_next_movidius_dev();
        if (!target_dev)
            return -ENODEV;
    }

    switch (ioucmd->cmd.opcode) {
    case MOVIDIUS_URING_CMD_SUBMIT_INFERENCE: {

        const struct inference_request __user *user_req = (const struct inference_request __user *)(uintptr_t)ioucmd->cmd.addr;
        struct movidius_cmd_hdr hdr;
        struct batch_job *job;

        if (copy_from_user(&hdr, user_req, sizeof(hdr)))
            return -EFAULT;

        if (hdr.version != MOVIDIUS_UAPI_VERSION)
            return -EINVAL;

        job = kzalloc(sizeof(*job), GFP_KERNEL);
        if (!job)
            return -ENOMEM;
        job->ioucmd = ioucmd;
        atomic_set(&job->pending_reqs, 1);

        req = kzalloc(sizeof(*req), GFP_KERNEL);
        if (!req) {
            kfree(job);
            return -ENOMEM;
        }

        if (copy_from_user(req, user_req, sizeof(struct inference_request))) {
            kfree(req);
            kfree(job);
            return -EFAULT;
        }

        if (req->num_input_segs > MAX_SG_SEGMENTS || req->num_output_segs > MAX_SG_SEGMENTS) {
            kfree(req);
            kfree(job);
            return -EINVAL;
        }

        req->job = job;

        spin_lock_irqsave(&target_dev->request_queue_lock, flags);
        list_add_tail(&req->list, &target_dev->request_queue);
        req_count = 0;
        list_for_each_entry(req, &target_dev->request_queue, list)
            req_count++;

        if (req_count >= batch_high_watermark) {
            spin_unlock_irqrestore(&target_dev->request_queue_lock, flags);
            wake_up_interruptible(&target_dev->request_queue_wait);
        } else {
            if (!hrtimer_is_queued(&target_dev->batch_timer))
                hrtimer_start(&target_dev->batch_timer, ms_to_ktime(batch_delay_ms), HRTIMER_MODE_REL);
            spin_unlock_irqrestore(&target_dev->request_queue_lock, flags);
        }
        break;
    }
    case MOVIDIUS_URING_CMD_SUBMIT_BATCH: {
        target_dev = get_next_movidius_dev();
        if (!target_dev)
            return -ENODEV;

        struct batch_inference_request batch_req;
        struct inference_request __user *user_reqs;
        int i;
        struct movidius_cmd_hdr hdr;
        struct batch_job *job;

        if (copy_from_user(&hdr, (void __user *)(uintptr_t)ioucmd->cmd.addr, sizeof(hdr)))
            return -EFAULT;

        if (hdr.version != MOVIDIUS_UAPI_VERSION)
            return -EINVAL;

        if (copy_from_user(&batch_req, (void __user *)(uintptr_t)ioucmd->cmd.addr, sizeof(batch_req)))
            return -EFAULT;

        if (batch_req.count > urb_pool_size)
            return -EINVAL;

        job = kzalloc(sizeof(*job), GFP_KERNEL);
        if (!job)
            return -ENOMEM;
        job->ioucmd = ioucmd;
        atomic_set(&job->pending_reqs, batch_req.count);

        user_reqs = (struct inference_request __user *)(uintptr_t)batch_req.reqs;

        for (i = 0; i < batch_req.count; i++) {
            req = kzalloc(sizeof(*req), GFP_KERNEL);
            if (!req) {
                /* Clean up already allocated requests */
                spin_lock_irqsave(&target_dev->request_queue_lock, flags);
                list_for_each_entry_safe(req, tmp, &target_dev->request_queue, list) {
                    if (req->job == job) {
                        list_del(&req->list);
                        kfree(req);
                    }
                }
                spin_unlock_irqrestore(&target_dev->request_queue_lock, flags);
                kfree(job);
                return -ENOMEM;
            }

            if (copy_from_user(req, &user_reqs[i], sizeof(struct inference_request))) {
                kfree(req);
                kfree(job);
                return -EFAULT;
            }

            if (req->num_input_segs > MAX_SG_SEGMENTS || req->num_output_segs > MAX_SG_SEGMENTS) {
                kfree(req);
                kfree(job);
                return -EINVAL;
            }

            req->job = job;

            spin_lock_irqsave(&target_dev->request_queue_lock, flags);
            list_add_tail(&req->list, &target_dev->request_queue);
            spin_unlock_irqrestore(&target_dev->request_queue_lock, flags);
        }

        spin_lock_irqsave(&target_dev->request_queue_lock, flags);
        req_count = 0;
        list_for_each_entry(req, &target_dev->request_queue, list)
            req_count++;

        if (req_count >= batch_high_watermark) {
            spin_unlock_irqrestore(&target_dev->request_queue_lock, flags);
            wake_up_interruptible(&target_dev->request_queue_wait);
        } else {
            if (!hrtimer_is_queued(&target_dev->batch_timer))
                hrtimer_start(&target_dev->batch_timer, ms_to_ktime(batch_delay_ms), HRTIMER_MODE_REL);
            spin_unlock_irqrestore(&target_dev->request_queue_lock, flags);
        }
        break;
    }
    default:
        return -EOPNOTSUPP;
    }

    return 0;
}

static const struct file_operations movidius_x_vpu_fops;

/* USB Probe and Disconnect */
static int movidius_x_vpu_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct movidius_x_vpu_dev *dev;
    int ret = 0;
    unsigned long flags;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    int i;

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
        ret = -ENODEV;
        goto error_free_dev;
    }

    const struct firmware *fw;
    if (request_firmware(&fw, fw_name, &interface->dev) == 0) {
        printk(KERN_INFO "Firmware loaded, size %zu\n", fw->size);
        int actual_length;
        ret = usb_bulk_msg(dev->udev, usb_sndbulkpipe(dev->udev, dev->bulk_out_endpoint_addr),
                               (void *)fw->data, fw->size, &actual_length, 5000);
        if (ret)
            dev_err(&interface->dev, "Failed to send firmware: %d\n", ret);
        release_firmware(fw);
    } else {
        dev_warn(&interface->dev, "no firmware found, continuing bare\n");
    }

    if (urb_pool_init(dev)) {
        printk(KERN_ERR "urb_pool_init failed\n");
        ret = -ENOMEM;
        goto error_free_dev;
    }

    hrtimer_init(&dev->batch_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev->batch_timer.function = movidius_x_vpu_batch_timer;
    INIT_WORK(&dev->batch_work, movidius_x_vpu_batch_work);

    dev->submission_thread = kthread_run(submission_thread_func, dev, "movidius_submission");
    if (IS_ERR(dev->submission_thread)) {
        printk(KERN_ERR "Failed to create submission thread\n");
        ret = PTR_ERR(dev->submission_thread);
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

    int minor = idr_alloc(&movidius_idr, dev, 0, 0, GFP_KERNEL);
    if (minor < 0) {
        ret = minor;
        goto error_submission_thread;
    }

    ret = cdev_add(&dev->cdev, MKDEV(MAJOR(dev_num), minor), 1);
    if (ret) {
        printk(KERN_ERR "cdev_add failed\n");
        idr_remove(&movidius_idr, minor);
        goto error_submission_thread;
    }

    dev->dev = device_create(movidius_class, NULL, MKDEV(MAJOR(dev_num), minor), NULL, "movidius_x_vpu%d", minor);
    if (IS_ERR(dev->dev)) {
        printk(KERN_ERR "device_create failed\n");
        ret = PTR_ERR(dev->dev);
        cdev_del(&dev->cdev);
        idr_remove(&movidius_idr, minor);
        goto error_submission_thread;
    }

    ret = kobject_init_and_add(&dev->kobj, &movidius_ktype, &dev->dev->kobj, "telemetry");
    if (ret) {
        printk(KERN_ERR "kobject_init_and_add failed\n");
        goto error_device;
    }

    ret = sysfs_create_group(&dev->kobj, &attr_group);
    if (ret) {
        printk(KERN_ERR "sysfs_create_group failed\n");
        kobject_put(&dev->kobj);
        goto error_device;
    }

    spin_lock_irqsave(&movidius_devices_lock, flags);
    list_add_tail(&dev->global_list, &movidius_devices);
    spin_unlock_irqrestore(&movidius_devices_lock, flags);

    return 0;

error_device:
    idr_remove(&movidius_idr, minor);
error_submission_thread:
    kthread_stop(dev->submission_thread);
error_urb_pool:
    urb_pool_free(dev);
error_free_dev:
    usb_put_dev(dev->udev);
    kfree(dev);
    return ret;
}

static void movidius_x_vpu_disconnect(struct usb_interface *interface)
{
    struct movidius_x_vpu_dev *dev = usb_get_intfdata(interface);
    unsigned long flags;

    printk(KERN_INFO "Movidius Myriad X VPU device unplugged\n");

    spin_lock_irqsave(&movidius_devices_lock, flags);
    list_del(&dev->global_list);
    spin_unlock_irqrestore(&movidius_devices_lock, flags);

    hrtimer_cancel(&dev->batch_timer);
    cancel_work_sync(&dev->batch_work);
    kthread_stop(dev->submission_thread);
    kobject_put(&dev->kobj);
    urb_pool_free(dev);
    idr_remove(&movidius_idr, MINOR(dev->dev->devt));
    device_destroy(movidius_class, dev->dev->devt);
    cdev_del(&dev->cdev);
    usb_put_dev(dev->udev);
    kfree(dev);
}

/* Sysfs */
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

static ssize_t temperature_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    /* In a real driver, this would read from a hardware sensor. */
    return sprintf(buf, "42000\n");
}

static struct kobj_attribute pending_reqs_attribute = __ATTR_RO(pending_reqs);
static struct kobj_attribute completed_reqs_attribute = __ATTR_RO(completed_reqs);
static struct kobj_attribute reset_count_attribute = __ATTR_RO(reset_count);
static struct kobj_attribute temperature_attribute = __ATTR_RO(temperature);

static struct attribute *attrs[] = {
    &pending_reqs_attribute.attr,
    &completed_reqs_attribute.attr,
    &reset_count_attribute.attr,
    &temperature_attribute.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static const struct sysfs_ops kobj_sysfs_ops = {
    .show = NULL,
    .store = NULL,
};

static struct kobj_type movidius_ktype = {
    .sysfs_ops = &kobj_sysfs_ops,
    .release = NULL,
};


/* PM Callbacks */
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

/* USB Driver Struct */
static struct usb_driver movidius_x_vpu_driver = {
    .name = "movidius_x_vpu",
    .id_table = movidius_x_vpu_table,
    .probe = movidius_x_vpu_probe,
    .disconnect = movidius_x_vpu_disconnect,
    .supports_autosuspend = 1,
    .runtime_suspend = movidius_x_vpu_runtime_suspend,
    .runtime_resume = movidius_x_vpu_runtime_resume,
    .runtime_idle = movidius_x_vpu_runtime_idle,
};

/* File Operations Struct */
static long movidius_x_vpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static const struct file_operations movidius_x_vpu_fops = {
    .owner = THIS_MODULE,
    .open = movidius_x_vpu_open,
    .release = movidius_x_vpu_release,
    .unlocked_ioctl = movidius_x_vpu_ioctl,
    .io_uring_cmd = movidius_x_vpu_uring_cmd,
};

static long movidius_x_vpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct movidius_x_vpu_dev *dev = file->private_data;
    struct movidius_x_vpu_dev *target_dev;

    switch (cmd) {
    case MOVIDIUS_IOCTL_REGISTER_DMA_ARENA: {
        struct movidius_dma_arena arena;
        if (copy_from_user(&arena, (void __user *)arg, sizeof(arena)))
            return -EFAULT;

    if (dev) { /* If not master device */
        target_dev = dev;
    } else { /* Master device, use first device for arena info */
        target_dev = list_first_entry_or_null(&movidius_devices, struct movidius_x_vpu_dev, global_list);
        if (!target_dev)
            return -ENODEV;
    }

    if (target_dev->sg_table)
        return -EBUSY;

    target_dev->num_pages = (arena.len + PAGE_SIZE - 1) / PAGE_SIZE;
    target_dev->pages = kcalloc(target_dev->num_pages, sizeof(struct page *), GFP_KERNEL);
    if (!target_dev->pages)
        return -ENOMEM;

    int ret = pin_user_pages(arena.addr, target_dev->num_pages, FOLL_WRITE, target_dev->pages, NULL);
    if (ret < 0) {
        kfree(target_dev->pages);
        return ret;
    }

    target_dev->sg_table = kcalloc(target_dev->num_pages, sizeof(struct scatterlist), GFP_KERNEL);
    if (!target_dev->sg_table) {
        unpin_user_pages(target_dev->pages, target_dev->num_pages);
        kfree(target_dev->pages);
        return -ENOMEM;
    }
    sg_init_table(target_dev->sg_table, target_dev->num_pages);
    for (int i = 0; i < target_dev->num_pages; i++)
        sg_set_page(&target_dev->sg_table[i], target_dev->pages[i], PAGE_SIZE, 0);

    if (dev) { /* If not master device */
        ret = dma_map_sg(&target_dev->udev->dev, target_dev->sg_table, target_dev->num_pages, DMA_BIDIRECTIONAL);
        if (ret == 0) {
            unpin_user_pages(target_dev->pages, target_dev->num_pages);
            kfree(target_dev->pages);
            kfree(target_dev->sg_table);
            return -ENOMEM;
        }
    } else { /* Master device, map for all devices */
        list_for_each_entry(target_dev, &movidius_devices, global_list) {
            ret = dma_map_sg(&target_dev->udev->dev, target_dev->sg_table, target_dev->num_pages, DMA_BIDIRECTIONAL);
            if (ret == 0) {
                /* In a real implementation, we would need to unmap from the other devices here */
                unpin_user_pages(target_dev->pages, target_dev->num_pages);
                kfree(target_dev->pages);
                kfree(target_dev->sg_table);
                return -ENOMEM;
            }
        }
    }

    return 0;
}

static const struct file_operations movidius_master_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = movidius_x_vpu_ioctl,
    .io_uring_cmd = movidius_x_vpu_uring_cmd,
};

/* Module Init and Exit */
static int __init movidius_x_vpu_init(void)
{
    int result;
    printk(KERN_INFO "Movidius Myriad X VPU driver loading...\n");

    result = alloc_chrdev_region(&dev_num, 0, 1, "movidius_x_vpu");
    if (result < 0) {
        printk(KERN_ERR "alloc_chrdev_region failed\n");
        return result;
    }

    movidius_class = class_create("movidius_x_vpu");
    if (IS_ERR(movidius_class)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(movidius_class);
    }

    cdev_init(&movidius_cdev, &movidius_master_fops);
    movidius_cdev.owner = THIS_MODULE;
    result = cdev_add(&movidius_cdev, dev_num, 1);
    if (result) {
        printk(KERN_ERR "cdev_add for master failed\n");
        class_destroy(movidius_class);
        unregister_chrdev_region(dev_num, 1);
        return result;
    }

    movidius_master_dev = device_create(movidius_class, NULL, dev_num, NULL, "movidius_master");
    if (IS_ERR(movidius_master_dev)) {
        printk(KERN_ERR "device_create for master failed\n");
        cdev_del(&movidius_cdev);
        class_destroy(movidius_class);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(movidius_master_dev);
    }

    result = usb_register(&movidius_x_vpu_driver);
    if (result) {
        printk(KERN_ERR "usb_register failed. Error number %d\n", result);
        device_destroy(movidius_class, dev_num);
        cdev_del(&movidius_cdev);
        class_destroy(movidius_class);
        unregister_chrdev_region(dev_num, 1);
    }

    return result;
}

static void __exit movidius_x_vpu_exit(void)
{
    printk(KERN_INFO "Movidius Myriad X VPU driver unloading...\n");
    usb_deregister(&movidius_x_vpu_driver);
    device_destroy(movidius_class, dev_num);
    cdev_del(&movidius_cdev);
    class_destroy(movidius_class);
    unregister_chrdev_region(dev_num, 1);
}

module_init(movidius_x_vpu_init);
module_exit(movidius_x_vpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Custom driver for Intel Movidius Myriad X VPU");
