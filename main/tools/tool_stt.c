#include "tools/tool_stt.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "llm/llm_stt.h"
#include "mimi_config.h"
#include "media/media_limits.h"
#include "telegram/telegram_bot.h"

#include <stdio.h>

static esp_err_t stt_chunk_cb(const uint8_t *data, size_t len, void *ctx) {
  llm_stt_stream_t *stream = (llm_stt_stream_t *)ctx;
  return llm_stt_stream_write(stream, data, len);
}

esp_err_t tool_stt_execute(const char *input_json, char *output,
                            size_t output_size) {
  if (!output || output_size == 0)
    return ESP_ERR_INVALID_ARG;

  cJSON *root = input_json ? cJSON_Parse(input_json) : NULL;
  cJSON *fid = root ? cJSON_GetObjectItem(root, "file_id") : NULL;
  if (!fid || !cJSON_IsString(fid)) {
    snprintf(output, output_size, "Error: file_id is required");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
  }

  telegram_file_info_t info;
  esp_err_t err = telegram_get_file_info(fid->valuestring, &info);
  if (err != ESP_OK) {
    snprintf(output, output_size, "Error: failed to resolve Telegram file");
    cJSON_Delete(root);
    return err;
  }

  cJSON_Delete(root);

  size_t voice_limit = media_limit_get_voice_bytes();
  if (info.size == 0 || info.size > voice_limit) {
    snprintf(output, output_size,
             "Error: voice file size must be under %d bytes", 
             (int)voice_limit);
    telegram_file_info_free(&info);
    return ESP_ERR_INVALID_SIZE;
  }

  llm_stt_stream_t *stream = NULL;
  err = llm_stt_stream_begin(&stream, info.size);
  if (err != ESP_OK) {
    telegram_file_info_free(&info);
    snprintf(output, output_size, "Error: STT init failed (%s)",
             esp_err_to_name(err));
    return err;
  }

  err = telegram_stream_file(&info, MIMI_MEDIA_STREAM_CHUNK, stt_chunk_cb,
                             stream);
  telegram_file_info_free(&info);
  if (err != ESP_OK) {
    llm_stt_stream_abort(stream);
    snprintf(output, output_size, "Error: Telegram download failed (%s)",
             esp_err_to_name(err));
    return err;
  }

  char transcript[1024] = {0};
  err = llm_stt_stream_complete(stream, transcript, sizeof(transcript));
  if (err == ESP_OK) {
    snprintf(output, output_size, "%s", transcript);
  } else {
    snprintf(output, output_size, "Error: STT failed (%s)",
             esp_err_to_name(err));
  }
  return err;
}
