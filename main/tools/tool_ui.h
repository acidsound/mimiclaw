#ifndef TOOL_UI_H
#define TOOL_UI_H

#include "esp_err.h"
#include <stddef.h>

/*
 * Request a fresh screen capture from websocket helper and wait for result.
 *
 * Input JSON:
 * {
 *   "chat_id": "ws_12",            // optional if injected by caller
 *   "goal": "open settings icon",  // optional hint for helper
 *   "timeout_ms": 12000,           // optional
 *   "max_width": 1080,             // optional
 *   "jpeg_quality": 65             // optional
 * }
 */
esp_err_t tool_ui_capture_execute(const char *input_json, char *output,
                                  size_t output_size);

/*
 * Send one UI action command to websocket helper.
 *
 * Input JSON:
 * {
 *   "chat_id": "ws_12",             // optional if injected by caller
 *   "action": "tap",                // required
 *   "x_norm": 0.73,                 // optional
 *   "y_norm": 0.18,                 // optional
 *   "x2_norm": 0.73,                // optional for drag/swipe
 *   "y2_norm": 0.42,                // optional for drag/swipe
 *   "text": "hello",                // optional for type action
 *   "key": "ENTER"                  // optional for key action
 * }
 */
esp_err_t tool_ui_action_execute(const char *input_json, char *output,
                                 size_t output_size);

/*
 * Ask iOS simulator helper to:
 * 1) open URL in Mobile Safari
 * 2) tap (default center)
 * 3) wait
 * 4) capture screenshot and send it to Telegram via sendPhoto
 *    then return created file_id.
 */
esp_err_t tool_ios_sim_capture_to_telegram_execute(const char *input_json,
                                                   char *output,
                                                   size_t output_size);

#endif /* TOOL_UI_H */
