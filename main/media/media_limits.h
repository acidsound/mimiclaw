#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t media_limits_init(void);

size_t media_limit_get_photo_bytes(void);
size_t media_limit_get_voice_bytes(void);
int media_limit_get_voice_seconds(void);

esp_err_t media_limit_set_photo_bytes(size_t bytes);
esp_err_t media_limit_set_voice_bytes(size_t bytes);
esp_err_t media_limit_set_voice_seconds(int seconds);
