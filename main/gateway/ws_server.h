#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize and start the WebSocket server on MIMI_WS_PORT.
 * Allows external clients to interact with the Agent via JSON messages.
 *
 * Protocol:
 *   Inbound:  {"type":"message","content":"hello","chat_id":"ws_client1"}
 *   Outbound: {"type":"response","content":"Hi!","chat_id":"ws_client1"}
 *   Extra events:
 *     - capture_request / capture_result
 *     - input_action
 */
esp_err_t ws_server_start(void);

/**
 * Send a text message to a specific WebSocket client by chat_id.
 * @param chat_id  Client identifier (assigned on connection)
 * @param text     Message text
 */
esp_err_t ws_server_send(const char *chat_id, const char *text);

/**
 * Send a typed WebSocket event with optional JSON payload.
 * Outbound frame format:
 *   {"type":"<type>","chat_id":"<id>","payload":{...}}
 */
esp_err_t ws_server_send_event(const char *chat_id, const char *type,
                               const char *payload_json);

/**
 * Returns true when the WebSocket server is started.
 */
bool ws_server_is_started(void);

/**
 * Stop the WebSocket server.
 */
esp_err_t ws_server_stop(void);
