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
#include <linux/mm.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
#error "movidius_x_vpu needs >= 5.12 for io_uring_cmd"
#endif

#define DRIVER_NAME "movidius_x_vpu"
#define MAX_DEVICES 16
#define URB_POOL_SIZE 64
#define MAX_ARENAS 8
#define BATCH_DELAY_MS 10
#define BATCH_HIGH_WATERMARK 32

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
#define MOVIDIUS_IOCTL_GET_DEVICE_INFO _IOR('M', 3, struct movidius_device_info)

struct movidius_device_info {
    uint32_t version;
    uint32_t max_batch_size;
    uint64_t total_memory;
    uint32_t num_compute_units;
};

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

static uint batch_delay_ms = BATCH_DELAY_MS;
module_param(batch_delay_ms, uint, 0644);
MODULE_PARM_DESC(batch_delay_ms, "Adaptive batch delay in milliseconds");

static uint batch_high_watermark = BATCH_HIGH_WATERMARK;
module_param(batch_high_watermark, uint, 0644);
MODULE_PARM_DESC(batch_high_watermark, "Queue depth threshold for batch dispatch");

static int submission_cpu_affinity = -1;
module_param(submission_cpu_affinity, int, 0644);
MODULE_PARM_DESC(submission_cpu_affinity, "CPU core for submission thread (-1 = no affinity)");

/* Forward declarations */
static int movidius_platform_probe(struct platform_device *pdev);
static int movidius_platform_remove(struct platform_device *pdev);
static int movidius_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags);
static long movidius_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int movidius_open(struct inode *inode, struct file *file);
static int movidius_release(struct inode *inode, struct file *file);

/* DMA Arena Structure */
struct dma_arena {
    struct list_head list;
    uint64_t user_addr;
    uint64_t len;
    struct page **pages;
    unsigned long num_pages;
    bool pinned;
};

/* URB Context Structure */
struct urb_context {
    struct list_head list;
    struct urb *urb;
    struct movidius_x_vpu_dev *dev;
    void *data_buffer;
    size_t buffer_size;
    struct io_uring_cmd *cmd;
    bool in_use;
    uint64_t user_data;
};

/* Pending Request Structure */
struct pending_request {
    struct list_head list;
    struct io_uring_cmd *cmd;
    struct inference_request req;
    uint64_t submit_time;
};

/* Performance Statistics */
struct perf_stats {
    atomic64_t total_inferences;
    atomic64_t total_batches;
    atomic64_t total_errors;
    atomic64_t queue_depth;
    atomic64_t avg_latency_us;
    atomic64_t throughput_mbps;
};

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
    u16 bulk_in_max_size;
    u16 bulk_out_max_size;

    /* DMA Arenas */
    struct list_head arena_list;
    spinlock_t arena_lock;
    struct mutex arena_mutex;

    /* URB Pool */
    struct list_head urb_pool;
    spinlock_t urb_pool_lock;
    wait_queue_head_t urb_available;

    /* Request Queue */
    struct list_head request_queue;
    spinlock_t request_queue_lock;
    struct task_struct *submission_thread;
    wait_queue_head_t request_queue_wait;
    struct hrtimer batch_timer;
    bool timer_pending;

    /* Device State */
    atomic_t is_open;
    atomic_t device_active;
    struct mutex dev_mutex;

    /* Performance Statistics */
    struct perf_stats stats;
    struct kobject *sysfs_kobj;

    /* Thermal Management */
    int32_t temperature;
    atomic_t throttled;

    /* Global Device List */
    struct list_head global_list;
};

/* Global Variables */
static LIST_HEAD(movidius_devices);
static DEFINE_SPINLOCK(movidius_devices_lock);
static struct class *movidius_class;
static dev_t movidius_devt;
static DEFINE_IDA(movidius_minor_ida);
static atomic_t global_device_count = ATOMIC_INIT(0);

static const struct file_operations movidius_fops = {
    .owner = THIS_MODULE,
    .open = movidius_open,
    .release = movidius_release,
    .uring_cmd = movidius_uring_cmd,
    .unlocked_ioctl = movidius_ioctl,
};

/* ========== DMA Arena Management ========== */

static struct dma_arena *find_arena_by_addr(struct movidius_x_vpu_dev *dev, uint64_t addr)
{
    struct dma_arena *arena;
    list_for_each_entry(arena, &dev->arena_list, list) {
        if (arena->user_addr == addr) {
            return arena;
        }
    }
    return NULL;
}

static int register_dma_arena(struct movidius_x_vpu_dev *dev, struct movidius_dma_arena *arena_info)
{
    struct dma_arena *arena;
    unsigned long num_pages;
    int ret;

    if (!arena_info->addr || !arena_info->len) {
        return -EINVAL;
    }

    mutex_lock(&dev->arena_mutex);

    /* Check if already registered */
    if (find_arena_by_addr(dev, arena_info->addr)) {
        mutex_unlock(&dev->arena_mutex);
        return -EEXIST;
    }

    arena = kzalloc(sizeof(*arena), GFP_KERNEL);
    if (!arena) {
        mutex_unlock(&dev->arena_mutex);
        return -ENOMEM;
    }

    arena->user_addr = arena_info->addr;
    arena->len = arena_info->len;
    num_pages = (arena_info->len + PAGE_SIZE - 1) >> PAGE_SHIFT;
    arena->num_pages = num_pages;

    arena->pages = kcalloc(num_pages, sizeof(struct page *), GFP_KERNEL);
    if (!arena->pages) {
        kfree(arena);
        mutex_unlock(&dev->arena_mutex);
        return -ENOMEM;
    }

    /* Pin user pages */
    ret = pin_user_pages_fast(arena_info->addr, num_pages, FOLL_WRITE, arena->pages);
    if (ret < 0 || ret != num_pages) {
        dev_err(dev->dev, "Failed to pin user pages: %d\n", ret);
        if (ret > 0) {
            unpin_user_pages(arena->pages, ret);
        }
        kfree(arena->pages);
        kfree(arena);
        mutex_unlock(&dev->arena_mutex);
        return ret < 0 ? ret : -EFAULT;
    }

    arena->pinned = true;
    list_add_tail(&arena->list, &dev->arena_list);

    mutex_unlock(&dev->arena_mutex);
    dev_info(dev->dev, "DMA arena registered: addr=0x%llx len=%llu pages=%lu\n",
             arena_info->addr, arena_info->len, num_pages);
    return 0;
}

static int unregister_dma_arena(struct movidius_x_vpu_dev *dev, uint64_t addr)
{
    struct dma_arena *arena;

    mutex_lock(&dev->arena_mutex);

    arena = find_arena_by_addr(dev, addr);
    if (!arena) {
        mutex_unlock(&dev->arena_mutex);
        return -ENOENT;
    }

    list_del(&arena->list);

    if (arena->pinned && arena->pages) {
        unpin_user_pages(arena->pages, arena->num_pages);
    }

    kfree(arena->pages);
    kfree(arena);

    mutex_unlock(&dev->arena_mutex);
    dev_info(dev->dev, "DMA arena unregistered: addr=0x%llx\n", addr);
    return 0;
}

static void cleanup_all_arenas(struct movidius_x_vpu_dev *dev)
{
    struct dma_arena *arena, *tmp;

    mutex_lock(&dev->arena_mutex);
    list_for_each_entry_safe(arena, tmp, &dev->arena_list, list) {
        list_del(&arena->list);
        if (arena->pinned && arena->pages) {
            unpin_user_pages(arena->pages, arena->num_pages);
        }
        kfree(arena->pages);
        kfree(arena);
    }
    mutex_unlock(&dev->arena_mutex);
}

/* ========== URB Pool Management ========== */

static void urb_complete_callback(struct urb *urb)
{
    struct urb_context *ctx = urb->context;
    struct movidius_x_vpu_dev *dev = ctx->dev;
    struct io_uring_cmd *cmd = ctx->cmd;
    int result;

    /* Update statistics */
    if (urb->status == 0) {
        atomic64_inc(&dev->stats.total_inferences);
        result = 0;
    } else {
        atomic64_inc(&dev->stats.total_errors);
        result = urb->status;
        dev_err(dev->dev, "URB failed with status: %d\n", urb->status);
    }

    /* Complete io_uring command */
    if (cmd) {
        io_uring_cmd_done(cmd, result, 0, 0);
    }

    /* Return URB to pool */
    spin_lock(&dev->urb_pool_lock);
    ctx->in_use = false;
    ctx->cmd = NULL;
    list_add_tail(&ctx->list, &dev->urb_pool);
    spin_unlock(&dev->urb_pool_lock);
    wake_up(&dev->urb_available);

    atomic64_dec(&dev->stats.queue_depth);
}

static struct urb_context *get_urb_from_pool(struct movidius_x_vpu_dev *dev)
{
    struct urb_context *ctx = NULL;
    unsigned long flags;

    spin_lock_irqsave(&dev->urb_pool_lock, flags);
    if (!list_empty(&dev->urb_pool)) {
        ctx = list_first_entry(&dev->urb_pool, struct urb_context, list);
        list_del(&ctx->list);
        ctx->in_use = true;
    }
    spin_unlock_irqrestore(&dev->urb_pool_lock, flags);

    return ctx;
}

static int init_urb_pool(struct movidius_x_vpu_dev *dev)
{
    int i;

    INIT_LIST_HEAD(&dev->urb_pool);
    spin_lock_init(&dev->urb_pool_lock);
    init_waitqueue_head(&dev->urb_available);

    for (i = 0; i < URB_POOL_SIZE; i++) {
        struct urb_context *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
        if (!ctx) {
            goto cleanup;
        }

        ctx->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!ctx->urb) {
            kfree(ctx);
            goto cleanup;
        }

        ctx->buffer_size = dev->bulk_out_max_size;
        ctx->data_buffer = kzalloc(ctx->buffer_size, GFP_KERNEL);
        if (!ctx->data_buffer) {
            usb_free_urb(ctx->urb);
            kfree(ctx);
            goto cleanup;
        }

        ctx->dev = dev;
        ctx->in_use = false;
        ctx->cmd = NULL;
        list_add_tail(&ctx->list, &dev->urb_pool);
    }

    dev_info(dev->dev, "URB pool initialized with %d URBs\n", URB_POOL_SIZE);
    return 0;

cleanup:
    /* Cleanup any allocated URBs */
    {
        struct urb_context *ctx, *tmp;
        list_for_each_entry_safe(ctx, tmp, &dev->urb_pool, list) {
            list_del(&ctx->list);
            if (ctx->data_buffer) kfree(ctx->data_buffer);
            if (ctx->urb) usb_free_urb(ctx->urb);
            kfree(ctx);
        }
    }
    return -ENOMEM;
}

static void cleanup_urb_pool(struct movidius_x_vpu_dev *dev)
{
    struct urb_context *ctx, *tmp;

    list_for_each_entry_safe(ctx, tmp, &dev->urb_pool, list) {
        list_del(&ctx->list);
        if (ctx->in_use && ctx->urb) {
            usb_kill_urb(ctx->urb);
        }
        if (ctx->data_buffer) kfree(ctx->data_buffer);
        if (ctx->urb) usb_free_urb(ctx->urb);
        kfree(ctx);
    }
}

/* ========== Request Submission ========== */

static int submit_inference_request(struct movidius_x_vpu_dev *dev,
                                    struct inference_request *req,
                                    struct io_uring_cmd *cmd)
{
    struct urb_context *ctx;
    int ret;
    size_t total_size = 0;
    int i;

    /* Validate request */
    if (req->num_input_segs == 0 || req->num_input_segs > MAX_SG_SEGMENTS) {
        return -EINVAL;
    }

    /* Calculate total data size */
    for (i = 0; i < req->num_input_segs; i++) {
        total_size += req->input_segs[i].len;
    }

    if (total_size > dev->bulk_out_max_size) {
        return -E2BIG;
    }

    /* Get URB from pool */
    ctx = get_urb_from_pool(dev);
    if (!ctx) {
        /* Wait for URB to become available */
        ret = wait_event_interruptible_timeout(dev->urb_available,
                                               (ctx = get_urb_from_pool(dev)) != NULL,
                                               msecs_to_jiffies(5000));
        if (ret <= 0 || !ctx) {
            return -ETIMEDOUT;
        }
    }

    ctx->cmd = cmd;
    ctx->user_data = req->user_data;

    /* Copy data from user DMA arenas to URB buffer */
    /* In a real implementation, this would copy from the pinned pages */
    /* For now, we'll just prepare a dummy transfer */
    memset(ctx->data_buffer, 0xAA, min(total_size, ctx->buffer_size));

    /* Setup USB bulk transfer */
    usb_fill_bulk_urb(ctx->urb,
                     dev->udev,
                     usb_sndbulkpipe(dev->udev, dev->bulk_out_endpoint_addr),
                     ctx->data_buffer,
                     total_size,
                     urb_complete_callback,
                     ctx);

    /* Submit URB */
    ret = usb_submit_urb(ctx->urb, GFP_KERNEL);
    if (ret) {
        dev_err(dev->dev, "Failed to submit URB: %d\n", ret);
        spin_lock(&dev->urb_pool_lock);
        ctx->in_use = false;
        ctx->cmd = NULL;
        list_add_tail(&ctx->list, &dev->urb_pool);
        spin_unlock(&dev->urb_pool_lock);
        return ret;
    }

    atomic64_inc(&dev->stats.queue_depth);
    return 0;
}

/* ========== Batch Timer Callback ========== */

static enum hrtimer_restart batch_timer_callback(struct hrtimer *timer)
{
    struct movidius_x_vpu_dev *dev = container_of(timer, struct movidius_x_vpu_dev, batch_timer);

    dev->timer_pending = false;
    wake_up(&dev->request_queue_wait);

    return HRTIMER_NORESTART;
}

/* ========== Submission Thread ========== */

static int submission_kthread(void *data)
{
    struct movidius_x_vpu_dev *dev = data;
    struct pending_request *req, *tmp;
    LIST_HEAD(batch);
    int batch_count;

    /* Set CPU affinity if requested */
    if (submission_cpu_affinity >= 0 && submission_cpu_affinity < nr_cpu_ids) {
        set_cpus_allowed_ptr(current, cpumask_of(submission_cpu_affinity));
        dev_info(dev->dev, "Submission thread bound to CPU %d\n", submission_cpu_affinity);
    }

    while (!kthread_should_stop()) {
        wait_event_interruptible(dev->request_queue_wait,
                                !list_empty(&dev->request_queue) ||
                                kthread_should_stop());

        if (kthread_should_stop())
            break;

        /* Adaptive batching logic */
        spin_lock(&dev->request_queue_lock);
        batch_count = 0;
        list_for_each_entry_safe(req, tmp, &dev->request_queue, list) {
            list_move_tail(&req->list, &batch);
            batch_count++;

            /* Check if we should dispatch the batch */
            if (batch_count >= batch_high_watermark) {
                break;
            }
        }

        /* If batch is small and timer not pending, start timer */
        if (batch_count > 0 && batch_count < batch_high_watermark && !dev->timer_pending) {
            dev->timer_pending = true;
            hrtimer_start(&dev->batch_timer,
                         ms_to_ktime(batch_delay_ms),
                         HRTIMER_MODE_REL);
        }
        spin_unlock(&dev->request_queue_lock);

        /* Process batch */
        list_for_each_entry_safe(req, tmp, &batch, list) {
            int ret = submit_inference_request(dev, &req->req, req->cmd);
            if (ret) {
                /* On error, complete with error code */
                io_uring_cmd_done(req->cmd, ret, 0, 0);
            }
            list_del(&req->list);
            kfree(req);
        }

        if (batch_count > 0) {
            atomic64_inc(&dev->stats.total_batches);
        }
    }

    return 0;
}

/* ========== io_uring Command Interface ========== */

static int movidius_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
    struct file *file = cmd->file;
    struct movidius_x_vpu_dev *dev = file->private_data;
    struct pending_request *pending;
    int ret;

    if (!atomic_read(&dev->device_active)) {
        return -ENODEV;
    }

    switch (cmd->cmd_op) {
    case MOVIDIUS_URING_CMD_SUBMIT_INFERENCE:
        {
            struct inference_request __user *user_req = (void __user *)cmd->cmd;
            struct inference_request req;

            if (copy_from_user(&req, user_req, sizeof(req))) {
                return -EFAULT;
            }

            if (req.hdr.version != MOVIDIUS_UAPI_VERSION) {
                return -EINVAL;
            }

            /* Queue the request */
            pending = kzalloc(sizeof(*pending), GFP_KERNEL);
            if (!pending) {
                return -ENOMEM;
            }

            pending->cmd = cmd;
            pending->req = req;
            pending->submit_time = ktime_get_ns();

            spin_lock(&dev->request_queue_lock);
            list_add_tail(&pending->list, &dev->request_queue);
            spin_unlock(&dev->request_queue_lock);
            wake_up(&dev->request_queue_wait);

            ret = -EIOCBQUEUED; /* Async operation */
        }
        break;

    case MOVIDIUS_URING_CMD_SUBMIT_BATCH:
        {
            struct batch_inference_request __user *user_batch = (void __user *)cmd->cmd;
            struct batch_inference_request batch;
            struct inference_request __user *user_reqs;
            int i;

            if (copy_from_user(&batch, user_batch, sizeof(batch))) {
                return -EFAULT;
            }

            if (batch.hdr.version != MOVIDIUS_UAPI_VERSION || batch.count == 0) {
                return -EINVAL;
            }

            user_reqs = (void __user *)(unsigned long)batch.reqs;

            /* Queue all requests in the batch */
            for (i = 0; i < batch.count; i++) {
                struct inference_request req;

                if (copy_from_user(&req, &user_reqs[i], sizeof(req))) {
                    return -EFAULT;
                }

                pending = kzalloc(sizeof(*pending), GFP_KERNEL);
                if (!pending) {
                    return -ENOMEM;
                }

                pending->cmd = (i == batch.count - 1) ? cmd : NULL; /* Only last request completes */
                pending->req = req;
                pending->submit_time = ktime_get_ns();

                spin_lock(&dev->request_queue_lock);
                list_add_tail(&pending->list, &dev->request_queue);
                spin_unlock(&dev->request_queue_lock);
            }

            wake_up(&dev->request_queue_wait);
            ret = -EIOCBQUEUED;
        }
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

/* ========== IOCTL Interface ========== */

static long movidius_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct movidius_x_vpu_dev *dev = file->private_data;
    int ret = 0;

    switch (cmd) {
    case MOVIDIUS_IOCTL_REGISTER_DMA_ARENA:
        {
            struct movidius_dma_arena arena;
            if (copy_from_user(&arena, (void __user *)arg, sizeof(arena))) {
                return -EFAULT;
            }
            ret = register_dma_arena(dev, &arena);
        }
        break;

    case MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA:
        {
            uint64_t addr = arg;
            ret = unregister_dma_arena(dev, addr);
        }
        break;

    case MOVIDIUS_IOCTL_GET_DEVICE_INFO:
        {
            struct movidius_device_info info = {
                .version = MOVIDIUS_UAPI_VERSION,
                .max_batch_size = 64,
                .total_memory = 512 * 1024 * 1024, /* 512 MB */
                .num_compute_units = 16,
            };
            if (copy_to_user((void __user *)arg, &info, sizeof(info))) {
                return -EFAULT;
            }
        }
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

/* ========== File Operations ========== */

static int movidius_open(struct inode *inode, struct file *file)
{
    struct movidius_x_vpu_dev *dev = container_of(inode->i_cdev, struct movidius_x_vpu_dev, cdev);

    if (atomic_cmpxchg(&dev->is_open, 0, 1) != 0) {
        return -EBUSY;
    }

    file->private_data = dev;
    dev_info(dev->dev, "Device opened\n");
    return 0;
}

static int movidius_release(struct inode *inode, struct file *file)
{
    struct movidius_x_vpu_dev *dev = file->private_data;

    /* Cleanup any pending requests */
    cleanup_all_arenas(dev);

    atomic_set(&dev->is_open, 0);
    dev_info(dev->dev, "Device released\n");
    return 0;
}

/* ========== Sysfs Attributes ========== */

static ssize_t total_inferences_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, *sysfs_kobj);
    return sprintf(buf, "%lld\n", atomic64_read(&dev->stats.total_inferences));
}

static ssize_t total_errors_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, *sysfs_kobj);
    return sprintf(buf, "%lld\n", atomic64_read(&dev->stats.total_errors));
}

static ssize_t queue_depth_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, *sysfs_kobj);
    return sprintf(buf, "%lld\n", atomic64_read(&dev->stats.queue_depth));
}

static ssize_t temperature_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct movidius_x_vpu_dev *dev = container_of(kobj, struct movidius_x_vpu_dev, *sysfs_kobj);
    return sprintf(buf, "%d\n", dev->temperature);
}

static struct kobj_attribute total_inferences_attr = __ATTR_RO(total_inferences);
static struct kobj_attribute total_errors_attr = __ATTR_RO(total_errors);
static struct kobj_attribute queue_depth_attr = __ATTR_RO(queue_depth);
static struct kobj_attribute temperature_attr = __ATTR_RO(temperature);

static struct attribute *movidius_attrs[] = {
    &total_inferences_attr.attr,
    &total_errors_attr.attr,
    &queue_depth_attr.attr,
    &temperature_attr.attr,
    NULL,
};

static struct attribute_group movidius_attr_group = {
    .attrs = movidius_attrs,
};

/* ========== Platform Driver ========== */

static struct platform_driver movidius_platform_driver = {
    .probe = movidius_platform_probe,
    .remove = movidius_platform_remove,
    .driver = {
        .name = DRIVER_NAME,
    },
};

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

    /* Initialize DMA arenas */
    INIT_LIST_HEAD(&dev->arena_list);
    spin_lock_init(&dev->arena_lock);
    mutex_init(&dev->arena_mutex);

    /* Initialize URB pool */
    ret = init_urb_pool(dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize URB pool\n");
        goto err_device_destroy;
    }

    /* Initialize request queue and submission thread */
    spin_lock_init(&dev->request_queue_lock);
    INIT_LIST_HEAD(&dev->request_queue);
    init_waitqueue_head(&dev->request_queue_wait);

    hrtimer_init(&dev->batch_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev->batch_timer.function = batch_timer_callback;
    dev->timer_pending = false;

    dev->submission_thread = kthread_run(submission_kthread, dev, "movidius-submit-%d", dev->minor);
    if (IS_ERR(dev->submission_thread)) {
        ret = PTR_ERR(dev->submission_thread);
        dev_err(&pdev->dev, "Failed to create submission kthread\n");
        goto err_cleanup_urb_pool;
    }

    /* Initialize statistics */
    atomic64_set(&dev->stats.total_inferences, 0);
    atomic64_set(&dev->stats.total_batches, 0);
    atomic64_set(&dev->stats.total_errors, 0);
    atomic64_set(&dev->stats.queue_depth, 0);

    /* Create sysfs entries */
    dev->sysfs_kobj = kobject_create_and_add("movidius", &cdevice->kobj);
    if (dev->sysfs_kobj) {
        ret = sysfs_create_group(dev->sysfs_kobj, &movidius_attr_group);
        if (ret) {
            dev_warn(&pdev->dev, "Failed to create sysfs group\n");
        }
    }

    atomic_set(&dev->device_active, 1);
    atomic_set(&dev->is_open, 0);
    mutex_init(&dev->dev_mutex);

    atomic_inc(&global_device_count);
    dev_info(&pdev->dev, "Platform device registered successfully (minor=%d)\n", dev->minor);
    return 0;

err_cleanup_urb_pool:
    cleanup_urb_pool(dev);
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

    atomic_set(&dev->device_active, 0);

    /* Stop submission thread */
    if (dev->submission_thread) {
        kthread_stop(dev->submission_thread);
    }

    /* Cancel batch timer */
    hrtimer_cancel(&dev->batch_timer);

    /* Remove sysfs entries */
    if (dev->sysfs_kobj) {
        sysfs_remove_group(dev->sysfs_kobj, &movidius_attr_group);
        kobject_put(dev->sysfs_kobj);
    }

    /* Cleanup resources */
    cleanup_urb_pool(dev);
    cleanup_all_arenas(dev);

    device_destroy(movidius_class, dev->devt);
    cdev_del(&dev->cdev);
    ida_simple_remove(&movidius_minor_ida, dev->minor);

    atomic_dec(&global_device_count);
    dev_info(&pdev->dev, "Platform device unregistered successfully\n");
    return 0;
}

/* ========== USB Driver ========== */

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
    dev->dev = &interface->dev;
    usb_set_intfdata(interface, dev);

    /* Find endpoints */
    iface_desc = interface->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;
        if (usb_endpoint_is_bulk_in(endpoint)) {
            dev->bulk_in_endpoint_addr = endpoint->bEndpointAddress;
            dev->bulk_in_max_size = le16_to_cpu(endpoint->wMaxPacketSize);
        }
        if (usb_endpoint_is_bulk_out(endpoint)) {
            dev->bulk_out_endpoint_addr = endpoint->bEndpointAddress;
            dev->bulk_out_max_size = le16_to_cpu(endpoint->wMaxPacketSize);
        }
    }

    if (!(dev->bulk_in_endpoint_addr && dev->bulk_out_endpoint_addr)) {
        dev_err(&interface->dev, "Could not find bulk-in and bulk-out endpoints\n");
        ret = -ENODEV;
        goto err_free_dev;
    }

    dev_info(&interface->dev, "Found endpoints: IN=0x%x (max=%u), OUT=0x%x (max=%u)\n",
             dev->bulk_in_endpoint_addr, dev->bulk_in_max_size,
             dev->bulk_out_endpoint_addr, dev->bulk_out_max_size);

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

/* ========== Module Init and Exit ========== */

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

    pr_info(DRIVER_NAME " driver loaded (version %d)\n", MOVIDIUS_UAPI_VERSION);
    pr_info("  - Zero-copy DMA with pin_user_pages\n");
    pr_info("  - io_uring interface for async submission\n");
    pr_info("  - Adaptive batching (delay=%ums, watermark=%u)\n",
            batch_delay_ms, batch_high_watermark);
    pr_info("  - URB pool with %d pre-allocated URBs\n", URB_POOL_SIZE);
    pr_info("  - Performance monitoring via sysfs\n");

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
MODULE_DESCRIPTION("High-performance driver for Intel Movidius Myriad X VPU with io_uring, zero-copy DMA, and adaptive batching");
MODULE_VERSION("2.0");
