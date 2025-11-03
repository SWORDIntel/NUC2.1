#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <liburing.h>

#define DMA_BUFFER_SIZE (4 * 1024 * 1024)
#define MAX_SG_SEGMENTS 16

#define MOVIDIUS_UAPI_VERSION 1

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
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

void list_devices()
{
    DIR *d;
    struct dirent *dir;
    d = opendir("/dev");
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, "movidius_x_vpu", 14) == 0) {
                printf("/dev/%s\n", dir->d_name);
            }
        }
        closedir(d);
    }
}

int main(int argc, char *argv[])
{
    int fd, ret;
    void *dma_buffer;
    struct io_uring ring;
    struct inference_request *req;
    char *dev_name = "/dev/movidius_x_vpu0";

    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        list_devices();
        return EXIT_SUCCESS;
    }

    if (argc > 1) {
        dev_name = argv[1];
    }

    printf("Opening %s...\n", dev_name);
    fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        perror("Failed to open the device");
        return EXIT_FAILURE;
    }

    printf("Device opened successfully.\n");

    dma_buffer = mmap(NULL, dma_buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

    req->hdr.version = MOVIDIUS_UAPI_VERSION;
    req->hdr.op = MOVIDIUS_URING_CMD_SUBMIT_INFERENCE;
    req->hdr.len = sizeof(*req);
    req->num_input_segs = 1;
    req->num_output_segs = 1;
    strcpy(dma_buffer, "Hello from user space!");
    req->input_segs[0].offset = 0;
    req->input_segs[0].len = strlen("Hello from user space!");
    req->output_segs[0].offset = 1024;
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

    if (munmap(dma_buffer, dma_buf_size) == -1) {
        perror("munmap failed");
    }

    io_uring_queue_exit(&ring);
    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
