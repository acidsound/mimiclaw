#include "utils/base64_stream.h"

#include "esp_check.h"
#include <stdlib.h>
#include <string.h>

static const char k_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static esp_err_t ensure_capacity(base64_stream_t *stream, size_t need) {
  if (!stream->buf) {
    size_t cap = need > 0 ? need : 4096;
    stream->buf = malloc(cap);
    if (!stream->buf)
      return ESP_ERR_NO_MEM;
    stream->cap = cap;
    stream->len = 0;
    stream->buf[0] = '\0';
    return ESP_OK;
  }

  if (stream->len + need < stream->cap)
    return ESP_OK;

  size_t new_cap = stream->cap;
  while (new_cap <= stream->len + need) {
    new_cap *= 2;
  }
  char *tmp = realloc(stream->buf, new_cap);
  if (!tmp)
    return ESP_ERR_NO_MEM;
  stream->buf = tmp;
  stream->cap = new_cap;
  return ESP_OK;
}

esp_err_t base64_stream_init(base64_stream_t *stream, size_t initial_cap) {
  if (!stream)
    return ESP_ERR_INVALID_ARG;
  memset(stream, 0, sizeof(*stream));
  if (initial_cap == 0)
    return ESP_OK;
  return ensure_capacity(stream, initial_cap);
}

static esp_err_t emit_block(base64_stream_t *stream, const uint8_t *block,
                            size_t block_len) {
  char out[4];
  uint32_t buf = 0;

  buf |= block[0] << 16;
  if (block_len > 1)
    buf |= block[1] << 8;
  if (block_len > 2)
    buf |= block[2];

  out[0] = k_b64_table[(buf >> 18) & 0x3F];
  out[1] = k_b64_table[(buf >> 12) & 0x3F];
  out[2] = (block_len > 1) ? k_b64_table[(buf >> 6) & 0x3F] : '=';
  out[3] = (block_len > 2) ? k_b64_table[buf & 0x3F] : '=';

  ESP_RETURN_ON_ERROR(ensure_capacity(stream, 4 + 1), "b64", "oom");
  memcpy(stream->buf + stream->len, out, 4);
  stream->len += 4;
  stream->buf[stream->len] = '\0';
  return ESP_OK;
}

esp_err_t base64_stream_append(base64_stream_t *stream, const uint8_t *data,
                               size_t len) {
  if (!stream || !data)
    return ESP_ERR_INVALID_ARG;

  uint8_t block[3];
  size_t block_len = 0;

  if (stream->rem_len > 0) {
    memcpy(block, stream->rem, stream->rem_len);
    block_len = stream->rem_len;
    stream->rem_len = 0;
  }

  while (len > 0) {
    block[block_len++] = *data++;
    len--;
    if (block_len == 3) {
      ESP_RETURN_ON_ERROR(emit_block(stream, block, block_len), "b64",
                          "emit fail");
      block_len = 0;
    }
  }

  if (block_len > 0) {
    memcpy(stream->rem, block, block_len);
    stream->rem_len = block_len;
  }
  return ESP_OK;
}

esp_err_t base64_stream_finish(base64_stream_t *stream) {
  if (!stream)
    return ESP_ERR_INVALID_ARG;
  if (stream->rem_len > 0) {
    ESP_RETURN_ON_ERROR(emit_block(stream, stream->rem, stream->rem_len),
                        "b64", "emit tail");
    stream->rem_len = 0;
  }
  if (stream->buf)
    stream->buf[stream->len] = '\0';
  return ESP_OK;
}

char *base64_stream_claim(base64_stream_t *stream) {
  if (!stream)
    return NULL;
  char *buf = stream->buf;
  stream->buf = NULL;
  stream->cap = 0;
  stream->len = 0;
  return buf;
}

void base64_stream_free(base64_stream_t *stream) {
  if (!stream)
    return;
  free(stream->buf);
  stream->buf = NULL;
  stream->cap = 0;
  stream->len = 0;
  stream->rem_len = 0;
}
