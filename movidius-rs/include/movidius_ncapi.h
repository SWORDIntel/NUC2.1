#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_NAME_SIZE 28

/**
 * Optimal number of threads per device for Myriad X
 * Myriad X has 2 NCEs and performs best with 3 threads
 */
#define THREADS_PER_DEVICE 3

/**
 * Number of simultaneous async inference requests per thread
 */
#define ASYNC_REQUESTS_PER_THREAD 6

/**
 * Total concurrent requests per device (3 × 6 = 18)
 */
#define TOTAL_REQUESTS_PER_DEVICE (THREADS_PER_DEVICE * ASYNC_REQUESTS_PER_THREAD)

/**
 * Default FIFO depth (optimal for most workloads)
 */
#define DEFAULT_DEPTH 4

/**
 * Minimum recommended FIFO depth
 */
#define MIN_DEPTH 2

/**
 * Maximum practical FIFO depth before diminishing returns
 */
#define MAX_DEPTH 10

/**
 * Result queue size for consumer threads (from benchmark_ncs.py)
 */
#define RESULT_QUEUE_SIZE 6

/**
 * Enable hardware pipeline optimization
 * WARNING: Requires models compiled with --scale (4, 8, or 16)
 * Can cause precision issues with half-precision overflow otherwise
 */
#define HW_STAGES_OPTIMIZATION true

/**
 * Enable reshape optimization
 */
#define RESHAPE_OPTIMIZATION false

/**
 * Memory usage warning threshold (percentage)
 */
#define MEMORY_WARNING_THRESHOLD 80

/**
 * Memory usage critical threshold (percentage)
 */
#define MEMORY_CRITICAL_THRESHOLD 90

/**
 * Temperature warning threshold (Celsius)
 */
#define WARNING_TEMP_C 75.0

/**
 * Temperature critical threshold (Celsius)
 */
#define CRITICAL_TEMP_C 85.0

/**
 * Maximum number of devices to enumerate
 */
#define MAX_DEVICE_INDEX 32

/**
 * Maximum graphs per device (memory-limited, typically ~10)
 */
#define TYPICAL_MAX_GRAPHS 10

/**
 * Maximum FIFOs per device
 */
#define TYPICAL_MAX_FIFOS 20

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
