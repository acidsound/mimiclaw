#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the LLM-powered scheduler.
 * Starts a background task that checks /spiffs/memory/schedule.md every minute.
 */
esp_err_t scheduler_init(void);

/**
 * @brief Manually trigger a scheduler check (e.g. after LLM edits the file).
 */
void scheduler_check_now(void);
