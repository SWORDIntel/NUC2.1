#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <liburing.h>

#define DMA_BUFFER_SIZE (4 * 1024 * 1024)
#define MAX_SG_SEGMENTS 16

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
};

struct movidius_sg_segment {
    uint32_t offset;
    uint32_t len;
};

struct inference_request {
    uint32_t num_input_segs;
    uint32_t num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];
    uint64_t user_data;
};

int main()
{
    int fd, i, ret;
    void *dma_buffer;
    struct io_uring ring;
    struct inference_request *req;

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

    req = malloc(sizeof(*req));
    if (!req) {
        fprintf(stderr, "malloc failed\n");
        return EXIT_FAILURE;
    }

    /* Prepare a request with 2 input segments and 1 output segment */
    req->num_input_segs = 2;
    req->num_output_segs = 1;

    /* First input segment */
    strcpy(dma_buffer, "Hello from ");
    req->input_segs[0].offset = 0;
    req->input_segs[0].len = strlen("Hello from ");

    /* Second input segment */
    strcpy(dma_buffer + 1024, "user space!");
    req->input_segs[1].offset = 1024;
    req->input_segs[1].len = strlen("user space!");

    /* Output segment */
    req->output_segs[0].offset = 2048;
    req->output_segs[0].len = 128;

    req->user_data = 1;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "io_uring_get_sqe failed\n");
        return EXIT_FAILURE;
    }

    io_uring_prep_uring_cmd(sqe, fd, MOVIDIUS_URING_CMD_SUBMIT_INFERENCE, req, 0);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %d\n", ret);
    }

    printf("Waiting for completion...\n");

    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe failed: %d\n", ret);
    } else {
        printf("Inference complete for request with user_data = %lu, result = %d\n",
               (unsigned long)cqe->user_data, cqe->res);
        io_uring_cqe_seen(&ring, cqe);
    }

    free(req);

    if (munmap(dma_buffer, DMA_BUFFER_SIZE) == -1) {
        perror("munmap failed");
    }

    io_uring_queue_exit(&ring);
    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
