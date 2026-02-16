#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

typedef struct {
  char request_id[32];
  char media_type[24];
  int width;
  int height;
  int rotation;
  char *image_b64; /* owned by caller after take_latest_capture */
} ui_capture_frame_t;

esp_err_t ui_bridge_init(void);

/*
 * Handle WebSocket event payloads related to UI capture loop.
 * Returns true when the event was recognized and consumed.
 */
bool ui_bridge_handle_ws_event(const char *chat_id, const cJSON *root);

/*
 * Send capture request to a connected websocket helper and wait for result.
 * On success, latest capture frame is stored internally and can be consumed
 * with ui_bridge_take_latest_capture().
 */
esp_err_t ui_bridge_request_capture(const char *chat_id, const char *goal,
                                    int timeout_ms, int max_width,
                                    int jpeg_quality, char *output,
                                    size_t output_size);

esp_err_t ui_bridge_request_ios_sim_capture(const char *helper_chat_id,
                                            const char *tg_chat_id,
                                            const char *url, int open_wait_ms,
                                            int wait_after_tap_ms,
                                            const char *tap_mode, int timeout_ms,
                                            char *output, size_t output_size);

bool ui_bridge_take_latest_capture(ui_capture_frame_t *out);
void ui_bridge_free_capture(ui_capture_frame_t *frame);
