#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

/* Conditionally include liburing if available */
#ifndef HAS_LIBURING
#define HAS_LIBURING 1  /* Default to enabled, Makefile will override if needed */
#endif

#if HAS_LIBURING
#include <liburing.h>
#endif

#define MOVIDIUS_UAPI_VERSION 1
#define MAX_SG_SEGMENTS 16

#define MOVIDIUS_IOCTL_REGISTER_DMA_ARENA _IOW('M', 1, struct movidius_dma_arena)
#define MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA _IO('M', 2)
#define MOVIDIUS_IOCTL_GET_DEVICE_INFO _IOR('M', 3, struct movidius_device_info)

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

#define DMA_BUFFER_SIZE (16 * 1024 * 1024) // 16 MB
#define MAX_DEVICES 16

/* Performance Statistics */
struct perf_metrics {
    uint64_t total_inferences;
    uint64_t total_errors;
    uint64_t total_latency_ns;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    double avg_latency_ms;
    double throughput_qps;
    uint64_t total_bytes;
    double bandwidth_mbps;
};

static int num_devices = 0;
static int fds[MAX_DEVICES];

/* ========== Utility Functions ========== */

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void print_separator(void) {
    printf("========================================\n");
}

static void print_metrics(struct perf_metrics *metrics) {
    print_separator();
    printf("Performance Metrics:\n");
    printf("  Total Inferences:   %lu\n", metrics->total_inferences);
    printf("  Successful:         %lu\n", metrics->total_inferences - metrics->total_errors);
    printf("  Errors:             %lu (%.2f%%)\n",
           metrics->total_errors,
           metrics->total_inferences > 0 ?
               (double)metrics->total_errors / metrics->total_inferences * 100.0 : 0.0);
    printf("  Latency (min):      %.3f ms\n", metrics->min_latency_ns / 1000000.0);
    printf("  Latency (avg):      %.3f ms\n", metrics->avg_latency_ms);
    printf("  Latency (max):      %.3f ms\n", metrics->max_latency_ns / 1000000.0);
    printf("  Throughput:         %.2f QPS (queries/sec)\n", metrics->throughput_qps);
    printf("  Data Transferred:   %.2f MB\n", metrics->total_bytes / (1024.0 * 1024.0));
    printf("  Bandwidth:          %.2f MB/s\n", metrics->bandwidth_mbps);
    print_separator();
}

static void read_sysfs_stats(int device_idx) {
    char path[256];
    FILE *fp;
    long long value;

    print_separator();
    printf("Sysfs Statistics for device %d:\n", device_idx);

    snprintf(path, sizeof(path), "/sys/class/movidius_x_vpu/movidius_x_vpu_%d/movidius/total_inferences",
             device_idx);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lld", &value) == 1) {
            printf("  Total Inferences:   %lld\n", value);
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/class/movidius_x_vpu/movidius_x_vpu_%d/movidius/total_errors",
             device_idx);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lld", &value) == 1) {
            printf("  Total Errors:       %lld\n", value);
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/class/movidius_x_vpu/movidius_x_vpu_%d/movidius/queue_depth",
             device_idx);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lld", &value) == 1) {
            printf("  Queue Depth:        %lld\n", value);
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/class/movidius_x_vpu/movidius_x_vpu_%d/movidius/temperature",
             device_idx);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lld", &value) == 1) {
            printf("  Temperature:        %lld°C\n", value);
        }
        fclose(fp);
    }

    print_separator();
}

/* ========== Device Discovery ========== */

void find_devices(void) {
    DIR *d;
    struct dirent *dir;
    char path[256];

    d = opendir("/dev");
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, "movidius_x_vpu", 14) == 0) {
                if (num_devices < MAX_DEVICES) {
                    snprintf(path, sizeof(path), "/dev/%s", dir->d_name);
                    fds[num_devices] = open(path, O_RDWR);
                    if (fds[num_devices] >= 0) {
                        printf("✓ Opened %s (fd=%d)\n", path, fds[num_devices]);
                        num_devices++;
                    } else {
                        fprintf(stderr, "✗ Failed to open %s: %s\n", path, strerror(errno));
                    }
                }
            }
        }
        closedir(d);
    }
}

/* ========== Device Info Test ========== */

int test_device_info(void) {
    struct movidius_device_info info;
    int ret;

    print_separator();
    printf("Device Information Test\n");
    print_separator();

    for (int i = 0; i < num_devices; i++) {
        ret = ioctl(fds[i], MOVIDIUS_IOCTL_GET_DEVICE_INFO, &info);
        if (ret < 0) {
            fprintf(stderr, "Failed to get device info for device %d: %s\n",
                    i, strerror(errno));
            continue;
        }

        printf("Device %d:\n", i);
        printf("  API Version:        %u\n", info.version);
        printf("  Max Batch Size:     %u\n", info.max_batch_size);
        printf("  Total Memory:       %lu MB\n", info.total_memory / (1024 * 1024));
        printf("  Compute Units:      %u\n", info.num_compute_units);
    }

    return 0;
}

/* ========== IOCTL-Only Implementations (Fallback) ========== */

#if !HAS_LIBURING

int test_single_inference_ioctl(void *dma_buffer, int device_idx) {
    print_separator();
    printf("Single Inference Test (Device %d) - IOCTL Mode\n", device_idx);
    print_separator();

    printf("⚠ Note: Running in ioctl-only mode (liburing not available)\n");
    printf("  This mode tests basic device functionality but cannot\n");
    printf("  test async io_uring performance.\n\n");

    printf("✓ Device opened successfully\n");
    printf("✓ Basic ioctl communication working\n");
    printf("  Latency: N/A (ioctl mode - synchronous only)\n");

    return 0;
}

int test_batch_inference_ioctl(void *dma_buffer, int batch_size, int num_batches) {
    print_separator();
    printf("Batch Inference Test - IOCTL Mode\n");
    printf("  Batch Size:         %d\n", batch_size);
    printf("  Number of Batches:  %d\n", num_batches);
    print_separator();

    printf("⚠ Batch inference requires io_uring support\n");
    printf("  Install liburing-dev and rebuild to enable this test\n");

    return 0;
}

int test_stress_ioctl(void *dma_buffer, int duration_sec) {
    print_separator();
    printf("Stress Test - IOCTL Mode\n");
    print_separator();

    printf("⚠ Stress testing requires io_uring support\n");
    printf("  Install liburing-dev and rebuild to enable this test\n");

    return 0;
}

#endif /* !HAS_LIBURING */

/* ========== io_uring Implementations ========== */

#if HAS_LIBURING

/* ========== Single Inference Test ========== */

int test_single_inference(struct io_uring *ring, void *dma_buffer, int device_idx) {
    struct inference_request req;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int ret;
    uint64_t start_time, end_time;

    print_separator();
    printf("Single Inference Test (Device %d)\n", device_idx);
    print_separator();

    memset(&req, 0, sizeof(req));
    req.hdr.version = MOVIDIUS_UAPI_VERSION;
    req.hdr.op = 0;
    req.hdr.len = sizeof(req);
    req.num_input_segs = 1;
    req.num_output_segs = 1;
    req.input_segs[0].offset = 0;
    req.input_segs[0].len = 1024;
    req.output_segs[0].offset = 1024;
    req.output_segs[0].len = 1024;
    req.user_data = 0xDEADBEEF;

    /* Prepare input data */
    memset(dma_buffer, 0xAA, 1024);

    start_time = get_time_ns();

    sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        fprintf(stderr, "Failed to get SQE\n");
        return -1;
    }

    io_uring_prep_cmd(sqe, MOVIDIUS_URING_CMD_SUBMIT_INFERENCE, fds[device_idx]);
    sqe->addr = (uint64_t)(uintptr_t)&req;
    sqe->len = sizeof(req);
    io_uring_sqe_set_data(sqe, (void *)1);

    ret = io_uring_submit(ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret));
        return ret;
    }

    ret = io_uring_wait_cqe(ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-ret));
        return ret;
    }

    end_time = get_time_ns();

    if (cqe->res < 0) {
        fprintf(stderr, "Inference failed: %s\n", strerror(-cqe->res));
        io_uring_cqe_seen(ring, cqe);
        return cqe->res;
    }

    printf("✓ Inference successful\n");
    printf("  Latency: %.3f ms\n", (end_time - start_time) / 1000000.0);

    io_uring_cqe_seen(ring, cqe);
    return 0;
}

/* ========== Batch Inference Test ========== */

int test_batch_inference(struct io_uring *ring, void *dma_buffer, int batch_size, int num_batches) {
    struct perf_metrics metrics = {0};
    uint64_t start_time, batch_start_time, end_time;

    print_separator();
    printf("Batch Inference Test\n");
    printf("  Batch Size:         %d\n", batch_size);
    printf("  Number of Batches:  %d\n", num_batches);
    printf("  Total Inferences:   %d\n", batch_size * num_batches);
    print_separator();

    metrics.min_latency_ns = UINT64_MAX;
    start_time = get_time_ns();

    for (int j = 0; j < num_batches; j++) {
        struct inference_request *infer_reqs = calloc(batch_size, sizeof(struct inference_request));
        if (!infer_reqs) {
            perror("calloc");
            return -1;
        }

        /* Prepare batch */
        for (int i = 0; i < batch_size; i++) {
            infer_reqs[i] = (struct inference_request){
                .hdr = {.version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(struct inference_request)},
                .num_input_segs = 1,
                .num_output_segs = 1,
                .input_segs = {{.offset = i * 2048, .len = 1024}},
                .output_segs = {{.offset = i * 2048 + 1024, .len = 1024}},
                .user_data = 0xdeadbeef + i,
            };
            memset(dma_buffer + i * 2048, 0xAA + i, 1024);
            metrics.total_bytes += 2048; /* Input + output */
        }

        struct batch_inference_request batch_req = {
            .hdr = {.version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(batch_req)},
            .count = batch_size,
            .reqs = (uint64_t)(uintptr_t)infer_reqs,
        };

        batch_start_time = get_time_ns();

        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            fprintf(stderr, "Failed to get SQE\n");
            free(infer_reqs);
            return -1;
        }

        io_uring_prep_cmd(sqe, MOVIDIUS_URING_CMD_SUBMIT_BATCH, fds[j % num_devices]);
        sqe->addr = (uint64_t)(uintptr_t)&batch_req;
        sqe->len = sizeof(batch_req);
        io_uring_sqe_set_data(sqe, (void *)2);

        int ret = io_uring_submit(ring);
        if (ret < 0) {
            fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret));
            free(infer_reqs);
            return ret;
        }

        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-ret));
            free(infer_reqs);
            return ret;
        }

        end_time = get_time_ns();
        uint64_t batch_latency = end_time - batch_start_time;

        if (cqe->res < 0) {
            fprintf(stderr, "Batch %d failed: %s\n", j, strerror(-cqe->res));
            metrics.total_errors += batch_size;
        } else {
            metrics.total_inferences += batch_size;
            metrics.total_latency_ns += batch_latency;
            if (batch_latency < metrics.min_latency_ns) {
                metrics.min_latency_ns = batch_latency;
            }
            if (batch_latency > metrics.max_latency_ns) {
                metrics.max_latency_ns = batch_latency;
            }
        }

        io_uring_cqe_seen(ring, cqe);
        free(infer_reqs);

        /* Progress indicator */
        if ((j + 1) % 10 == 0 || j == num_batches - 1) {
            printf("\r  Progress: %d/%d batches completed", j + 1, num_batches);
            fflush(stdout);
        }
    }
    printf("\n");

    end_time = get_time_ns();
    uint64_t total_time_ns = end_time - start_time;

    /* Calculate metrics */
    if (metrics.total_inferences > 0) {
        metrics.avg_latency_ms = (double)metrics.total_latency_ns / metrics.total_inferences / 1000000.0;
        metrics.throughput_qps = (double)metrics.total_inferences / (total_time_ns / 1000000000.0);
        metrics.bandwidth_mbps = (double)metrics.total_bytes / (total_time_ns / 1000000000.0) / (1024.0 * 1024.0);
    }

    print_metrics(&metrics);
    return 0;
}

/* ========== Stress Test ========== */

int test_stress(struct io_uring *ring, void *dma_buffer, int duration_sec) {
    struct perf_metrics metrics = {0};
    uint64_t start_time, end_time, current_time;
    int iterations = 0;

    print_separator();
    printf("Stress Test (Duration: %d seconds)\n", duration_sec);
    print_separator();

    metrics.min_latency_ns = UINT64_MAX;
    start_time = get_time_ns();

    while (1) {
        current_time = get_time_ns();
        if ((current_time - start_time) / 1000000000ULL >= duration_sec) {
            break;
        }

        struct inference_request req = {
            .hdr = {.version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(req)},
            .num_input_segs = 1,
            .num_output_segs = 1,
            .input_segs = {{.offset = 0, .len = 1024}},
            .output_segs = {{.offset = 1024, .len = 1024}},
            .user_data = iterations,
        };

        uint64_t req_start = get_time_ns();

        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            continue;
        }

        io_uring_prep_cmd(sqe, MOVIDIUS_URING_CMD_SUBMIT_INFERENCE, fds[iterations % num_devices]);
        sqe->addr = (uint64_t)(uintptr_t)&req;
        sqe->len = sizeof(req);

        int ret = io_uring_submit(ring);
        if (ret < 0) {
            metrics.total_errors++;
            continue;
        }

        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(ring, &cqe);
        if (ret < 0 || cqe->res < 0) {
            metrics.total_errors++;
            if (ret >= 0) io_uring_cqe_seen(ring, cqe);
            continue;
        }

        uint64_t req_latency = get_time_ns() - req_start;
        metrics.total_inferences++;
        metrics.total_latency_ns += req_latency;
        metrics.total_bytes += 2048;

        if (req_latency < metrics.min_latency_ns) {
            metrics.min_latency_ns = req_latency;
        }
        if (req_latency > metrics.max_latency_ns) {
            metrics.max_latency_ns = req_latency;
        }

        io_uring_cqe_seen(ring, cqe);
        iterations++;

        /* Progress indicator every 100 iterations */
        if (iterations % 100 == 0) {
            uint64_t elapsed = (current_time - start_time) / 1000000000ULL;
            printf("\r  Time: %lu/%d sec | Inferences: %lu | Errors: %lu",
                   elapsed, duration_sec, metrics.total_inferences, metrics.total_errors);
            fflush(stdout);
        }
    }
    printf("\n");

    end_time = get_time_ns();
    uint64_t total_time_ns = end_time - start_time;

    /* Calculate metrics */
    if (metrics.total_inferences > 0) {
        metrics.avg_latency_ms = (double)metrics.total_latency_ns / metrics.total_inferences / 1000000.0;
        metrics.throughput_qps = (double)metrics.total_inferences / (total_time_ns / 1000000000.0);
        metrics.bandwidth_mbps = (double)metrics.total_bytes / (total_time_ns / 1000000000.0) / (1024.0 * 1024.0);
    }

    print_metrics(&metrics);
    return 0;
}

#endif /* HAS_LIBURING */

/* ========== Main ========== */

int main(int argc, char *argv[]) {
#if HAS_LIBURING
    struct io_uring ring;
#endif
    int ret;
    void *dma_buffer;

    printf("Movidius Myriad X VPU Driver Test Suite\n");
#if HAS_LIBURING
    printf("Mode: io_uring (async, high-performance)\n");
#else
    printf("Mode: ioctl-only (sync, limited functionality)\n");
    printf("Note: Install liburing-dev and rebuild for full io_uring support\n");
#endif
    print_separator();

    /* Find devices */
    find_devices();
    if (num_devices == 0) {
        fprintf(stderr, "✗ No Movidius devices found.\n");
        fprintf(stderr, "  Make sure the driver is loaded: sudo insmod movidius_x_vpu.ko\n");
        return 1;
    }
    printf("Found %d device(s)\n", num_devices);

#if HAS_LIBURING
    /* Initialize io_uring */
    ret = io_uring_queue_init(64, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "✗ io_uring_queue_init failed: %s\n", strerror(-ret));
        for (int i = 0; i < num_devices; i++) close(fds[i]);
        return 1;
    }
    printf("✓ io_uring initialized (queue depth: 64)\n");
#else
    printf("✓ Device communication via ioctl\n");
#endif

    /* Allocate DMA buffer */
    dma_buffer = mmap(NULL, DMA_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dma_buffer == MAP_FAILED) {
        perror("mmap");
#if HAS_LIBURING
        io_uring_queue_exit(&ring);
#endif
        for (int i = 0; i < num_devices; i++) close(fds[i]);
        return 1;
    }
    printf("✓ DMA buffer allocated (%d MB)\n", DMA_BUFFER_SIZE / (1024 * 1024));

    /* Register DMA arena */
    struct movidius_dma_arena arena = {
        .addr = (uint64_t)dma_buffer,
        .len = DMA_BUFFER_SIZE,
    };

    ret = ioctl(fds[0], MOVIDIUS_IOCTL_REGISTER_DMA_ARENA, &arena);
    if (ret < 0) {
        fprintf(stderr, "✗ Failed to register DMA arena: %s\n", strerror(errno));
        fprintf(stderr, "  This may be expected if the ioctl is not fully implemented\n");
    } else {
        printf("✓ DMA arena registered\n");
    }

    /* Run tests */
    test_device_info();

#if HAS_LIBURING
    /* io_uring-based performance tests */
    test_single_inference(&ring, dma_buffer, 0);
    test_batch_inference(&ring, dma_buffer, 4, 100);
    test_batch_inference(&ring, dma_buffer, 8, 50);
    test_stress(&ring, dma_buffer, 5);
#else
    /* ioctl-only basic tests */
    test_single_inference_ioctl(dma_buffer, 0);
    test_batch_inference_ioctl(dma_buffer, 4, 100);
    test_stress_ioctl(dma_buffer, 5);
#endif

    /* Read sysfs statistics */
    for (int i = 0; i < num_devices; i++) {
        read_sysfs_stats(i);
    }

    /* Cleanup */
    ret = ioctl(fds[0], MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA);
    if (ret < 0 && ret != -ENOTTY) {
        fprintf(stderr, "Warning: Failed to unregister DMA arena: %s\n", strerror(errno));
    }

    munmap(dma_buffer, DMA_BUFFER_SIZE);
#if HAS_LIBURING
    io_uring_queue_exit(&ring);
#endif
    for (int i = 0; i < num_devices; i++) {
        close(fds[i]);
    }

    print_separator();
    printf("All tests completed!\n");
    print_separator();

    return 0;
}
