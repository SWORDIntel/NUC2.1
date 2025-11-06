#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_NAME_SIZE 28

typedef struct ncDeviceHandle_t {
  uint8_t _private[0];
} ncDeviceHandle_t;

typedef struct ncGraphHandle_t {
  uint8_t _private[0];
} ncGraphHandle_t;

typedef struct ncFifoHandle_t {
  uint8_t _private[0];
} ncFifoHandle_t;

/**
 * Create a device handle
 *
 * # Safety
 * - device_handle must be a valid pointer
 */
int ncDeviceCreate(int index, struct ncDeviceHandle_t **device_handle);

/**
 * Open a device
 *
 * # Safety
 * - device_handle must be a valid handle from ncDeviceCreate
 */
int ncDeviceOpen(struct ncDeviceHandle_t *device_handle);

/**
 * Close a device
 *
 * # Safety
 * - device_handle must be a valid handle from ncDeviceCreate
 */
int ncDeviceClose(struct ncDeviceHandle_t *device_handle);

/**
 * Destroy a device handle
 *
 * # Safety
 * - device_handle must be a valid pointer to a handle
 * - The handle must not be used after this call
 */
int ncDeviceDestroy(struct ncDeviceHandle_t **device_handle);

/**
 * Get device option
 *
 * # Safety
 * - All pointers must be valid
 */
int ncDeviceGetOption(struct ncDeviceHandle_t *device_handle,
                      int option,
                      void *data,
                      unsigned int *data_length);

/**
 * Create a graph handle
 *
 * # Safety
 * - name must be a valid null-terminated C string
 * - graph_handle must be a valid pointer
 */
int ncGraphCreate(const char *name, struct ncGraphHandle_t **graph_handle);

/**
 * Allocate graph
 *
 * # Safety
 * - All handles must be valid
 * - graph_buffer must point to valid memory of graph_buffer_length bytes
 */
int ncGraphAllocate(struct ncDeviceHandle_t *device_handle,
                    struct ncGraphHandle_t *graph_handle,
                    const void *graph_buffer,
                    unsigned int graph_buffer_length);

/**
 * Allocate graph with FIFOs
 *
 * # Safety
 * - All pointers must be valid
 */
int ncGraphAllocateWithFifos(struct ncDeviceHandle_t *device_handle,
                             struct ncGraphHandle_t *graph_handle,
                             const void *graph_buffer,
                             unsigned int graph_buffer_length,
                             struct ncFifoHandle_t **in_fifo_handle,
                             struct ncFifoHandle_t **out_fifo_handle);

/**
 * Destroy graph
 *
 * # Safety
 * - graph_handle must be a valid pointer to a handle
 */
int ncGraphDestroy(struct ncGraphHandle_t **graph_handle);

/**
 * Create FIFO
 *
 * # Safety
 * - name must be valid C string
 * - fifo_handle must be valid pointer
 */
int ncFifoCreate(const char *name, int fifo_type, struct ncFifoHandle_t **fifo_handle);

/**
 * Write element to FIFO
 *
 * # Safety
 * - All pointers must be valid
 * - input_tensor must point to valid memory
 */
int ncFifoWriteElem(struct ncFifoHandle_t *fifo_handle,
                    const void *input_tensor,
                    unsigned int *input_tensor_length,
                    void *user_param);

/**
 * Read element from FIFO
 *
 * # Safety
 * - All pointers must be valid
 * - output_data must have space for the full element
 */
int ncFifoReadElem(struct ncFifoHandle_t *fifo_handle,
                   void *output_data,
                   unsigned int *output_data_len,
                   void **user_param);

/**
 * Destroy FIFO
 *
 * # Safety
 * - fifo_handle must be valid pointer to handle
 */
int ncFifoDestroy(struct ncFifoHandle_t **fifo_handle);

/**
 * Get global option
 *
 * # Safety
 * - All pointers must be valid
 */
int ncGlobalGetOption(int option, void *data, unsigned int *data_length);

/**
 * Set global option
 *
 * # Safety
 * - All pointers must be valid
 */
int ncGlobalSetOption(int option, const void *data, unsigned int data_length);
