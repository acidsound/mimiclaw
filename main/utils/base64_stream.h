#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  uint8_t rem[2];
  size_t rem_len;
} base64_stream_t;

esp_err_t base64_stream_init(base64_stream_t *stream, size_t initial_cap);
esp_err_t base64_stream_append(base64_stream_t *stream, const uint8_t *data,
                               size_t len);
esp_err_t base64_stream_finish(base64_stream_t *stream);
char *base64_stream_claim(base64_stream_t *stream);
void base64_stream_free(base64_stream_t *stream);
