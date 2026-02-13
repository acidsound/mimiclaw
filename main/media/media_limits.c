#include "media/media_limits.h"

#include "esp_log.h"
#include "mimi_config.h"
#include "nvs.h"

static const char *TAG = "media_limits";

static size_t s_photo_limit = MIMI_MEDIA_MAX_PHOTO_BYTES;
static size_t s_voice_limit = MIMI_MEDIA_MAX_VOICE_BYTES;
static int s_voice_seconds = MIMI_MEDIA_MAX_VOICE_SECONDS;

static void media_limits_load(void) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_MEDIA, NVS_READONLY, &nvs) != ESP_OK)
    return;

  uint32_t val = 0;
  if (nvs_get_u32(nvs, MIMI_NVS_KEY_MEDIA_PHOTO, &val) == ESP_OK && val)
    s_photo_limit = val;
  if (nvs_get_u32(nvs, MIMI_NVS_KEY_MEDIA_VOICE_BYTES, &val) == ESP_OK && val)
    s_voice_limit = val;
  int32_t secs = 0;
  if (nvs_get_i32(nvs, MIMI_NVS_KEY_MEDIA_VOICE_SECS, &secs) == ESP_OK && secs)
    s_voice_seconds = secs;
  nvs_close(nvs);
}

esp_err_t media_limits_init(void) {
  media_limits_load();
  ESP_LOGI(TAG, "Media limits: photo=%d bytes voice=%d bytes/%d s",
           (int)s_photo_limit, (int)s_voice_limit, s_voice_seconds);
  return ESP_OK;
}

size_t media_limit_get_photo_bytes(void) { return s_photo_limit; }
size_t media_limit_get_voice_bytes(void) { return s_voice_limit; }
int media_limit_get_voice_seconds(void) { return s_voice_seconds; }

static esp_err_t save_u32(const char *key, uint32_t value) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(MIMI_NVS_MEDIA, NVS_READWRITE, &nvs);
  if (err != ESP_OK)
    return err;
  err = nvs_set_u32(nvs, key, value);
  if (err == ESP_OK)
    err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

static esp_err_t save_i32(const char *key, int32_t value) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(MIMI_NVS_MEDIA, NVS_READWRITE, &nvs);
  if (err != ESP_OK)
    return err;
  err = nvs_set_i32(nvs, key, value);
  if (err == ESP_OK)
    err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

esp_err_t media_limit_set_photo_bytes(size_t bytes) {
  s_photo_limit = bytes;
  return save_u32(MIMI_NVS_KEY_MEDIA_PHOTO, (uint32_t)bytes);
}

esp_err_t media_limit_set_voice_bytes(size_t bytes) {
  s_voice_limit = bytes;
  return save_u32(MIMI_NVS_KEY_MEDIA_VOICE_BYTES, (uint32_t)bytes);
}

esp_err_t media_limit_set_voice_seconds(int seconds) {
  s_voice_seconds = seconds;
  return save_i32(MIMI_NVS_KEY_MEDIA_VOICE_SECS, seconds);
}
