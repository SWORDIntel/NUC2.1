#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <liburing.h>

#define DMA_BUFFER_SIZE (4 * 1024 * 1024)
#define NUM_REQUESTS 8

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
};

struct inference_request {
    size_t input_offset;
    size_t input_size;
    size_t output_offset;
    size_t output_size;
    uint64_t user_data;
};

int main()
{
    int fd, i, ret;
    void *dma_buffer;
    struct io_uring ring;
    struct iovec iov;

    printf("Opening /dev/movidius_x_vpu...\n");
    fd = open("/dev/movidius_x_vpu", O_RDWR);
    if (fd < 0) {
        perror("Failed to open the device");
        return EXIT_FAILURE;
    }

    printf("Device opened successfully.\n");

    dma_buffer = mmap(NULL, DMA_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dma_buffer == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("DMA buffer mapped successfully.\n");

    ret = io_uring_queue_init(NUM_REQUESTS, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    for (i = 0; i < NUM_REQUESTS; i++) {
        struct io_uring_sqe *sqe;
        struct inference_request *req;

        sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            fprintf(stderr, "io_uring_get_sqe failed\n");
            break;
        }

        req = malloc(sizeof(*req));
        if (!req) {
            fprintf(stderr, "malloc failed\n");
            break;
        }

        sprintf(dma_buffer + (i * 2048), "Hello from request %d", i);
        req->input_offset = i * 2048;
        req->input_size = strlen(dma_buffer + (i * 2048)) + 1;
        req->output_offset = (i * 2048) + 1024;
        req->output_size = 1024;
        req->user_data = i + 1;

        iov.iov_base = req;
        iov.iov_len = sizeof(*req);
        io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, &iov, 1, 0);
        sqe->opcode = MOVIDIUS_URING_CMD_SUBMIT_INFERENCE;
        io_uring_sqe_set_data(sqe, (void *)(uintptr_t)req->user_data);
    }

    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %d\n", ret);
    }

    printf("Waiting for completions...\n");

    for (i = 0; i < NUM_REQUESTS; i++) {
        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed: %d\n", ret);
            break;
        }
        printf("Inference complete for request with user_data = %lu, result = %d\n",
               (unsigned long)cqe->user_data, cqe->res);
        free((void *)(uintptr_t)cqe->user_data);
        io_uring_cqe_seen(&ring, cqe);
    }

    if (munmap(dma_buffer, DMA_BUFFER_SIZE) == -1) {
        perror("munmap failed");
    }

    io_uring_queue_exit(&ring);
    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
