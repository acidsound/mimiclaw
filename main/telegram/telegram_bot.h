#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize the Telegram bot.
 */
esp_err_t telegram_bot_init(void);

/**
 * Start the Telegram polling task (long polling on Core 0).
 */
esp_err_t telegram_bot_start(void);

/**
 * Send a text message to a Telegram chat.
 * Automatically splits messages longer than 4096 chars.
 * @param chat_id  Telegram chat ID (numeric string)
 * @param text     Message text (supports Markdown)
 */
esp_err_t telegram_send_message(const char *chat_id, const char *text);

/**
 * Save the Telegram bot token to NVS.
 */
esp_err_t telegram_set_token(const char *token);

/**
 * Authorization management.
 */
esp_err_t telegram_auth_add(int64_t chat_id);
esp_err_t telegram_auth_remove(int64_t chat_id);
void telegram_auth_list(void);
esp_err_t telegram_admin_add(int64_t chat_id);
esp_err_t telegram_admin_remove(int64_t chat_id);
void telegram_admin_list(void);

typedef struct {
  char *path;     /* Heap-allocated; caller frees */
  size_t size;    /* Bytes reported by Telegram */
} telegram_file_info_t;

typedef esp_err_t (*telegram_media_chunk_cb_t)(const uint8_t *data, size_t len,
                                               void *ctx);

esp_err_t telegram_get_file_info(const char *file_id,
                                 telegram_file_info_t *info);
void telegram_file_info_free(telegram_file_info_t *info);
esp_err_t telegram_stream_file(const telegram_file_info_t *info,
                               size_t chunk_size,
                               telegram_media_chunk_cb_t cb, void *ctx);
esp_err_t telegram_stream_file_by_id(const char *file_id, size_t chunk_size,
                                     telegram_media_chunk_cb_t cb, void *ctx);
