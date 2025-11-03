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

struct register_dma_buffer_request {
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
    MOVIDIUS_URING_CMD_REGISTER_DMA_BUFFER,
    MOVIDIUS_URING_CMD_UNREGISTER_DMA_BUFFER,
};

#define DMA_BUFFER_SIZE (16 * 1024 * 1024) // 16 MB

int main(int argc, char *argv[]) {
    int fd;
    struct io_uring ring;
    int ret;
    void *dma_buffer;

    fd = open("/dev/movidius_x_vpu0", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    ret = io_uring_queue_init(32, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %d\n", ret);
        close(fd);
        return 1;
    }

    dma_buffer = mmap(NULL, DMA_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dma_buffer == MAP_FAILED) {
        perror("mmap");
        io_uring_queue_exit(&ring);
        close(fd);
        return 1;
    }

    printf("Registering DMA buffer...\n");
    struct register_dma_buffer_request reg_req = {
        .addr = (uint64_t)dma_buffer,
        .len = DMA_BUFFER_SIZE,
    };

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_uring_cmd(sqe, MOVIDIUS_URING_CMD_REGISTER_DMA_BUFFER, fd);
    sqe->addr = (uint64_t)(uintptr_t)&reg_req;
    sqe->len = sizeof(reg_req);
    io_uring_sqe_set_data(sqe, (void *)1);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %d\n", ret);
        munmap(dma_buffer, DMA_BUFFER_SIZE);
        io_uring_queue_exit(&ring);
        close(fd);
        return 1;
    }

    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe failed: %d\n", ret);
        munmap(dma_buffer, DMA_BUFFER_SIZE);
        io_uring_queue_exit(&ring);
        close(fd);
        return 1;
    }

    if (cqe->res < 0) {
        fprintf(stderr, "DMA buffer registration failed: %s\n", strerror(-cqe->res));
        io_uring_cqe_seen(&ring, cqe);
        munmap(dma_buffer, DMA_BUFFER_SIZE);
        io_uring_queue_exit(&ring);
        close(fd);
        return 1;
    }
    io_uring_cqe_seen(&ring, cqe);
    printf("DMA buffer registered.\n");

    printf("Submitting batch of inferences...\n");

    const int batch_size = 4;
    struct inference_request *infer_reqs = calloc(batch_size, sizeof(struct inference_request));
    if (!infer_reqs) {
        perror("calloc");
        // Handle cleanup
        return 1;
    }

    for (int i = 0; i < batch_size; i++) {
        infer_reqs[i] = (struct inference_request) {
            .hdr = { .version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(struct inference_request) },
            .num_input_segs = 1,
            .num_output_segs = 1,
            .input_segs = { { .offset = i * 2048, .len = 1024 } },
            .output_segs = { { .offset = i * 2048 + 1024, .len = 1024 } },
            .user_data = 0xdeadbeef + i,
        };
        memset(dma_buffer + i * 2048, 0xAA + i, 1024);
    }

    struct batch_inference_request batch_req = {
        .hdr = { .version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(batch_req) },
        .count = batch_size,
        .reqs = (uint64_t)(uintptr_t)infer_reqs,
    };

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_uring_cmd(sqe, MOVIDIUS_URING_CMD_SUBMIT_BATCH, fd);
    sqe->addr = (uint64_t)(uintptr_t)&batch_req;
    sqe->len = sizeof(batch_req);
    io_uring_sqe_set_data(sqe, (void *)2);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %d\n", ret);
        // Don't forget to unregister
    }

    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe failed: %d\n", ret);
        // Don't forget to unregister
    }

    if (cqe->res < 0) {
        fprintf(stderr, "Inference failed: %s\n", strerror(-cqe->res));
    } else {
        printf("Inference successful.\n");
    }
    io_uring_cqe_seen(&ring, cqe);

    printf("Unregistering DMA buffer...\n");
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_uring_cmd(sqe, MOVIDIUS_URING_CMD_UNREGISTER_DMA_BUFFER, fd);
    sqe->addr = 0;
    sqe->len = 0;
    io_uring_sqe_set_data(sqe, (void *)3);
    io_uring_submit(&ring);
    io_uring_wait_cqe(&ring, &cqe);
    if (cqe->res < 0) {
        fprintf(stderr, "DMA buffer unregistration failed: %s\n", strerror(-cqe->res));
    }
    io_uring_cqe_seen(&ring, cqe);
    printf("DMA buffer unregistered.\n");

    free(infer_reqs);
    munmap(dma_buffer, DMA_BUFFER_SIZE);
    io_uring_queue_exit(&ring);
    close(fd);
    return 0;
}
