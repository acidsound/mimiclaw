#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stddef.h>

/* Channel identifiers */
#define MIMI_CHAN_TELEGRAM "telegram"
#define MIMI_CHAN_WEBSOCKET "websocket"
#define MIMI_CHAN_CLI "cli"

/* Message types on the bus */
typedef enum {
  MIMI_MSG_TYPE_TEXT = 0,
  MIMI_MSG_TYPE_PHOTO,
  MIMI_MSG_TYPE_VOICE
} mimi_msg_type_t;

typedef struct {
  char channel[16];     /* "telegram", "websocket", "cli" */
  char chat_id[32];     /* Telegram chat_id or WS client id */
  mimi_msg_type_t type; /* Media type */
  char *media_id;       /* Media identifier (e.g. Telegram file_id) */
  char *content;        /* Heap-allocated message text or caption */
  size_t media_size;    /* Bytes reported by Telegram (if any) */
  int media_duration;   /* Duration in seconds (voice notes) */
} mimi_msg_t;

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
esp_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_inbound(const mimi_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_outbound(const mimi_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms);
