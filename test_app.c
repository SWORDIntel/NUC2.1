#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/ioctl.h>

struct inference_request {
    // This must match the kernel struct
    char data[128];
};

#define MOVIDIUS_IOCTL_SUBMIT_INFERENCE _IOW('m', 1, struct inference_request)

int main()
{
    int fd;
    struct inference_request req;

    printf("Opening /dev/movidius_x_vpu...\n");
    fd = open("/dev/movidius_x_vpu", O_RDWR);
    if (fd < 0) {
        perror("Failed to open the device");
        return EXIT_FAILURE;
    }

    printf("Device opened successfully.\n");

    printf("Submitting inference request...\n");
    if (ioctl(fd, MOVIDIUS_IOCTL_SUBMIT_INFERENCE, &req) < 0) {
        perror("ioctl failed");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Inference request submitted.\n");

    close(fd);
    printf("Device closed.\n");

    return EXIT_SUCCESS;
}
