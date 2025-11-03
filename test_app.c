#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

#define DMA_BUFFER_SIZE (4 * 1024 * 1024)
#define NUM_REQUESTS 8

struct inference_request {
    size_t input_offset;
    size_t input_size;
    size_t output_offset;
    size_t output_size;
    int eventfd;
    u_int64_t seq;
};

#define MOVIDIUS_IOCTL_SUBMIT_INFERENCE _IOW('m', 1, struct inference_request)

int main()
{
    int fd, efd, i;
    struct inference_request req;
    void *dma_buffer;
    struct pollfd pollfd;

    printf("Opening /dev/movidius_x_vpu...\n");
    fd = open("/dev/movidius_x_vpu", O_RDWR);
    if (fd < 0) {
        perror("Failed to open the device");
        return EXIT_FAILURE;
    }

    printf("Device opened successfully.\n");

    efd = eventfd(0, EFD_CLOEXEC);
    if (efd < 0) {
        perror("eventfd failed");
        close(fd);
        return EXIT_FAILURE;
    }

    dma_buffer = mmap(NULL, DMA_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dma_buffer == MAP_FAILED) {
        perror("mmap failed");
        close(efd);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("DMA buffer mapped successfully.\n");

    for (i = 0; i < NUM_REQUESTS; i++) {
        // Populate the input buffer
        sprintf(dma_buffer + (i * 2048), "Hello from request %d", i);

        // Prepare the inference request
        req.input_offset = i * 2048;
        req.input_size = strlen(dma_buffer + (i * 2048)) + 1;
        req.output_offset = (i * 2048) + 1024;
        req.output_size = 1024;
        req.eventfd = efd;
        req.seq = i + 1;

        printf("Submitting inference request %d...\n", i);
        if (ioctl(fd, MOVIDIUS_IOCTL_SUBMIT_INFERENCE, &req) < 0) {
            perror("ioctl failed");
            break;
        }
    }

    printf("Waiting for completions...\n");

    pollfd.fd = efd;
    pollfd.events = POLLIN;

    for (i = 0; i < NUM_REQUESTS; i++) {
        if (poll(&pollfd, 1, 5000) <= 0) {
            perror("poll failed");
            break;
        }

        u_int64_t seq;
        if (read(efd, &seq, sizeof(seq)) != sizeof(seq)) {
            perror("read from eventfd failed");
            break;
        }
        printf("Inference complete for request with seq = %lu\n", seq);
    }

    if (munmap(dma_buffer, DMA_BUFFER_SIZE) == -1) {
        perror("munmap failed");
    }

    close(efd);
    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
