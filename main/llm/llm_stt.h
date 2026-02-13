#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize the STT module.
 */
esp_err_t llm_stt_init(void);

/**
 * Transcribe audio data using STT provider API.
 * @param audio_data Binary audio data (e.g. OGG/Opus from Telegram)
 * @param audio_len  Length of audio data
 * @param out_text   Buffer to store transcribed text
 * @param max_out    Size of out_text
 * @return ESP_OK on success
 */
esp_err_t llm_stt_transcribe(const uint8_t *audio_data, size_t audio_len,
                             char *out_text, size_t max_out);

typedef struct llm_stt_stream llm_stt_stream_t;

esp_err_t llm_stt_stream_begin(llm_stt_stream_t **handle, size_t audio_size);
esp_err_t llm_stt_stream_write(llm_stt_stream_t *handle, const uint8_t *data,
                               size_t len);
esp_err_t llm_stt_stream_complete(llm_stt_stream_t *handle, char *out_text,
                                  size_t max_out);
void llm_stt_stream_abort(llm_stt_stream_t *handle);

/**
 * Save STT API key to NVS.
 */
esp_err_t llm_stt_set_key(const char *key);
/**
 * Save STT model to NVS.
 */
esp_err_t llm_stt_set_model(const char *model);
/**
 * Save STT base URL to NVS.
 */
esp_err_t llm_stt_set_base_url(const char *url);
/**
 * Save STT provider to NVS (currently only Groq supported).
 */
esp_err_t llm_stt_set_provider(int provider);
