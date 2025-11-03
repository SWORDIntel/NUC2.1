#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>

#define DMA_BUFFER_SIZE (4 * 1024 * 1024)

struct inference_request {
    size_t input_offset;
    size_t input_size;
    size_t output_offset;
    size_t output_size;
};

#define MOVIDIUS_IOCTL_SUBMIT_INFERENCE _IOW('m', 1, struct inference_request)

int main()
{
    int fd;
    struct inference_request req;
    void *dma_buffer;

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

    // Populate the input buffer
    strcpy(dma_buffer, "Hello from user space!");

    // Prepare the inference request
    req.input_offset = 0;
    req.input_size = strlen("Hello from user space!") + 1;
    req.output_offset = 1024;
    req.output_size = 1024; // The driver will fill this in

    printf("Submitting inference request...\n");
    if (ioctl(fd, MOVIDIUS_IOCTL_SUBMIT_INFERENCE, &req) < 0) {
        perror("ioctl failed");
        munmap(dma_buffer, DMA_BUFFER_SIZE);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Inference request submitted.\n");

    // Here we would wait for the inference to complete and then read the result
    // from the output buffer.
    usleep(100000); // 100ms

    if (munmap(dma_buffer, DMA_BUFFER_SIZE) == -1) {
        perror("munmap failed");
    }

    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
