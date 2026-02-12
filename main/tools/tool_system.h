#ifndef TOOL_SYSTEM_H
#define TOOL_SYSTEM_H

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute the heap_info tool.
 * Returns free internal, psram, and total heap sizes.
 */
esp_err_t tool_heap_info_execute(const char *input_json, char *output,
                                 size_t output_size);

/**
 * Execute the restart tool.
 * Restarts the device after a short delay. Admin-only.
 */
esp_err_t tool_restart_execute(const char *input_json, char *output,
                               size_t output_size);

#endif // TOOL_SYSTEM_H
