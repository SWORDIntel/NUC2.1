#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <liburing.h>
#include <sys/ioctl.h>
#include <stdint.h>

#define MOVIDIUS_UAPI_VERSION 1
#define MAX_SG_SEGMENTS 16

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

#define MOVIDIUS_IOCTL_REGISTER_DMA_ARENA _IOW('M', 1, struct movidius_dma_arena)
#define MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA _IO('M', 2)

struct movidius_dma_arena {
    uint64_t addr;
    uint64_t len;
};

struct batch_inference_request {
    struct movidius_cmd_hdr hdr;
    uint32_t count;
    uint32_t pad;
    uint64_t reqs;
};

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
};

#define DMA_BUFFER_SIZE (16 * 1024 * 1024) // 16 MB

#define MAX_DEVICES 16
static int num_devices = 0;
static int fds[MAX_DEVICES];

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
                        printf("Opened %s\n", path);
                        num_devices++;
                    }
                }
            }
        }
        closedir(d);
    }
}

int main(int argc, char *argv[]) {
    struct io_uring ring;
    int ret;
    void *dma_buffer;

    find_devices();
    if (num_devices == 0) {
        fprintf(stderr, "No Movidius devices found.\n");
        return 1;
    }

    ret = io_uring_queue_init(32, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %d\n", ret);
        for (int i = 0; i < num_devices; i++) close(fds[i]);
        return 1;
    }

    dma_buffer = mmap(NULL, DMA_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dma_buffer == MAP_FAILED) {
        perror("mmap");
        io_uring_queue_exit(&ring);
        for (int i = 0; i < num_devices; i++) close(fds[i]);
        return 1;
    }

    printf("Registering DMA buffer...\n");
    struct movidius_dma_arena arena = {
        .addr = (uint64_t)dma_buffer,
        .len = DMA_BUFFER_SIZE,
    };

    ret = ioctl(fds[0], MOVIDIUS_IOCTL_REGISTER_DMA_ARENA, &arena);
    if (ret < 0) {
        perror("ioctl");
        munmap(dma_buffer, DMA_BUFFER_SIZE);
        io_uring_queue_exit(&ring);
        for (int i = 0; i < num_devices; i++) close(fds[i]);
        return 1;
    }
    printf("DMA buffer registered.\n");

    printf("Submitting batches of inferences...\n");

    const int num_batches = 100;
    for (int j = 0; j < num_batches; j++) {
        const int batch_size = 4;
        struct inference_request *infer_reqs = calloc(batch_size, sizeof(struct inference_request));
        if (!infer_reqs) {
            perror("calloc");
            return 1;
        }

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
    }

    struct batch_inference_request batch_req = {
        .hdr = {.version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(batch_req)},
        .count = batch_size,
        .reqs = (uint64_t)(uintptr_t)infer_reqs,
    };

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_uring_cmd(sqe, MOVIDIUS_URING_CMD_SUBMIT_BATCH, fds[j % num_devices]);
    sqe->addr = (uint64_t)(uintptr_t)&batch_req;
    sqe->len = sizeof(batch_req);
    io_uring_sqe_set_data(sqe, (void *)2);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %d\n", ret);
    }

    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe failed: %d\n", ret);
    } else {
        if (cqe->res < 0) {
            fprintf(stderr, "Inference failed: %s\n", strerror(-cqe->res));
        } else {
            printf("Inference batch %d successful.\n", j);
        }
        io_uring_cqe_seen(&ring, cqe);
        free(infer_reqs);
    }

    printf("Unregistering DMA buffer...\n");
    ret = ioctl(fds[0], MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA);
    if (ret < 0) {
        perror("ioctl");
    }
    printf("DMA buffer unregistered.\n");

    munmap(dma_buffer, DMA_BUFFER_SIZE);
    io_uring_queue_exit(&ring);
    for (int i = 0; i < num_devices; i++) {
        close(fds[i]);
    }
    return 0;
}
