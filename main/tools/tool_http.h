#ifndef TOOL_HTTP_H
#define TOOL_HTTP_H

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute the http_request tool.
 * Supports GET/POST, secret substitution, and session management.
 * Enforces 8KB response limit and SSRF protection.
 */
esp_err_t tool_http_request_execute(const char *input_json, char *output,
                                    size_t output_size);

#endif // TOOL_HTTP_H
