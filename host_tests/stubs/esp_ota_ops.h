#pragma once
/* host-test stand-in for ESP-IDF's esp_ota_ops.h (only what sdupdate.c uses) */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { size_t size; } esp_partition_t;
typedef uint32_t esp_ota_handle_t;
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start);
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t image_size, esp_ota_handle_t *h);
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
