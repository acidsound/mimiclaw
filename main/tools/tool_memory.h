#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute memory_write tool.
 * Input: {"content": "..."}
 */
esp_err_t tool_memory_write_execute(const char *input_json, char *output,
                                    size_t output_size);

/**
 * Execute memory_append tool.
 * Input: {"content": "..."}
 */
esp_err_t tool_memory_append_execute(const char *input_json, char *output,
                                     size_t output_size);
