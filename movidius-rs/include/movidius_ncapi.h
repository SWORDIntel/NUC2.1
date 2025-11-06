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

/**
 * Create a device handle
 */
int ncDeviceCreate(int index, struct ncDeviceHandle_t **device_handle);

/**
 * Open a device
 */
int ncDeviceOpen(struct ncDeviceHandle_t *device_handle);

/**
 * Close a device
 */
int ncDeviceClose(struct ncDeviceHandle_t *device_handle);

/**
 * Destroy a device handle
 */
int ncDeviceDestroy(struct ncDeviceHandle_t **device_handle);

/**
 * Create a graph handle
 */
int ncGraphCreate(const char *name, struct ncGraphHandle_t **graph_handle);
