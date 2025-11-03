#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <liburing.h>

#define DMA_BUFFER_SIZE (4 * 1024 * 1024)
#define NUM_REQUESTS 8

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
};

struct inference_request {
    size_t input_offset;
    size_t input_size;
    size_t output_offset;
    size_t output_size;
    uint64_t user_data;
};

struct batch_inference_request {
    uint32_t count;
    uint32_t pad;
    uint64_t reqs;
};

int main()
{
    int fd, i, ret;
    void *dma_buffer;
    struct io_uring ring;
    struct inference_request *reqs;

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

    ret = io_uring_queue_init(1, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    reqs = malloc(sizeof(*reqs) * NUM_REQUESTS);
    if (!reqs) {
        fprintf(stderr, "malloc failed\n");
        return EXIT_FAILURE;
    }

    for (i = 0; i < NUM_REQUESTS; i++) {
        sprintf(dma_buffer + (i * 2048), "Hello from request %d", i);
        reqs[i].input_offset = i * 2048;
        reqs[i].input_size = strlen(dma_buffer + (i * 2048)) + 1;
        reqs[i].output_offset = (i * 2048) + 1024;
        reqs[i].output_size = 1024;
        reqs[i].user_data = i + 1;
    }

    struct batch_inference_request batch_req = {
        .count = NUM_REQUESTS,
        .reqs = (uint64_t)(uintptr_t)reqs,
    };

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "io_uring_get_sqe failed\n");
        return EXIT_FAILURE;
    }

    io_uring_prep_uring_cmd(sqe, fd, MOVIDIUS_URING_CMD_SUBMIT_BATCH, &batch_req, 0);

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
        io_uring_cqe_seen(&ring, cqe);
    }

    free(reqs);

    if (munmap(dma_buffer, DMA_BUFFER_SIZE) == -1) {
        perror("munmap failed");
    }

    io_uring_queue_exit(&ring);
    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
