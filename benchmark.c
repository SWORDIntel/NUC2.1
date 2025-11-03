#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <liburing.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define MOVIDIUS_UAPI_VERSION 1
#define MAX_SG_SEGMENTS 16

#define MOVIDIUS_IOCTL_REGISTER_DMA_ARENA _IOW('M', 1, struct movidius_dma_arena)

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

enum {
    MOVIDIUS_URING_CMD_SUBMIT_INFERENCE,
    MOVIDIUS_URING_CMD_SUBMIT_BATCH,
};

#define DMA_ARENA_SIZE (16 * 1024 * 1024) // 16 MB
#define NUM_THREADS 4
#define NUM_BATCHES 1000
#define BATCH_SIZE 4

void *dma_arena;

void *worker_thread(void *arg) {
    int fd = (int)(uintptr_t)arg;
    struct io_uring ring;
    int ret;

    ret = io_uring_queue_init(32, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %d\n", ret);
        return NULL;
    }

    for (int i = 0; i < NUM_BATCHES; i++) {
        struct inference_request *infer_reqs = calloc(BATCH_SIZE, sizeof(struct inference_request));
        if (!infer_reqs) {
            perror("calloc");
            return NULL;
        }

        for (int j = 0; j < BATCH_SIZE; j++) {
            infer_reqs[j] = (struct inference_request){
                .hdr = {.version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(struct inference_request)},
                .num_input_segs = 1,
                .num_output_segs = 1,
                .input_segs = {{.offset = (i * BATCH_SIZE + j) * 2048, .len = 1024}},
                .output_segs = {{.offset = (i * BATCH_SIZE + j) * 2048 + 1024, .len = 1024}},
                .user_data = (uint64_t)(uintptr_t)pthread_self() + i * BATCH_SIZE + j,
            };
            memset(dma_arena + (i * BATCH_SIZE + j) * 2048, 0xAA, 1024);
        }

        struct batch_inference_request batch_req = {
            .hdr = {.version = MOVIDIUS_UAPI_VERSION, .op = 0, .len = sizeof(batch_req)},
            .count = BATCH_SIZE,
            .reqs = (uint64_t)(uintptr_t)infer_reqs,
        };

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_uring_cmd(sqe, MOVIDIUS_URING_CMD_SUBMIT_BATCH, fd);
        sqe->addr = (uint64_t)(uintptr_t)&batch_req;
        sqe->len = sizeof(batch_req);

        ret = io_uring_submit_and_wait(&ring, 1);
        if (ret < 0) {
            fprintf(stderr, "io_uring_submit_and_wait failed: %d\n", ret);
        }

        struct io_uring_cqe *cqe;
        io_uring_peek_cqe(&ring, &cqe);
        if (cqe->res < 0) {
            fprintf(stderr, "Inference failed: %s\n", strerror(-cqe->res));
        }
        io_uring_cqe_seen(&ring, cqe);
        free(infer_reqs);
    }

    io_uring_queue_exit(&ring);
    return NULL;
}


int main(int argc, char *argv[]) {
    int fd;
    int ret;

    fd = open("/dev/movidius_master", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    dma_arena = mmap(NULL, DMA_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dma_arena == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    struct movidius_dma_arena arena = {
        .addr = (uint64_t)dma_arena,
        .len = DMA_ARENA_SIZE,
    };

    ret = ioctl(fd, MOVIDIUS_IOCTL_REGISTER_DMA_ARENA, &arena);
    if (ret < 0) {
        perror("ioctl");
        munmap(dma_arena, DMA_ARENA_SIZE);
        close(fd);
        return 1;
    }

    printf("DMA arena registered.\n");

    pthread_t threads[NUM_THREADS];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker_thread, (void *)(uintptr_t)fd);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double inferences_per_second = (NUM_THREADS * NUM_BATCHES * BATCH_SIZE) / elapsed_time;

    printf("Total time: %.2f s\n", elapsed_time);
    printf("Inferences per second: %.2f\n", inferences_per_second);


    munmap(dma_arena, DMA_ARENA_SIZE);
    close(fd);
    return 0;
}
