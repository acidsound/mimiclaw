#include "scheduler.h"
#include "bus/message_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h> // For malloc/free
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "scheduler";
static const char *SCHEDULE_FILE = "/spiffs/public/schedule.md";

static void trigger_task(const char *task_desc) {
  ESP_LOGI(TAG, "Triggering scheduled task: %s", task_desc);

  mimi_msg_t msg = {0};
  strcpy(msg.channel, "scheduled");
  strcpy(msg.chat_id, "system");

  size_t len = strlen(task_desc) + 32;
  msg.content = malloc(len);
  if (msg.content) {
    snprintf(msg.content, len, "[SCHEDULED_EVENT] %s", task_desc);
    message_bus_push_inbound(&msg);
  }
}

static void check_schedule(void) {
  FILE *f = fopen(SCHEDULE_FILE, "r");
  if (!f)
    return;

  time_t now_t;
  struct tm now_tm;
  time(&now_t);
  localtime_r(&now_t, &now_tm);

  char line[256];
  char *new_content = malloc(4096); // Assuming schedule file is small
  if (!new_content) {
    fclose(f);
    return;
  }
  new_content[0] = '\0';
  bool modified = false;

  while (fgets(line, sizeof(line), f)) {
    // Simple parser: "- [YYYY-MM-DD HH:MM] description"
    // Or "- [EVERY 60m] description"
    // Also skip already done: "- [x] ..."

    if (strncmp(line, "- [x]", 5) == 0) {
      strcat(new_content, line);
      continue;
    }

    char *start = strchr(line, '[');
    char *end = strchr(line, ']');
    if (start && end && start < end) {
      char time_buf[32] = {0};
      size_t t_len = end - start - 1;
      if (t_len < sizeof(time_buf)) {
        memcpy(time_buf, start + 1, t_len);

        char *desc = end + 2;
        while (*desc == ' ')
          desc++;
        char *newline = strchr(desc, '\n');
        if (newline)
          *newline = '\0';

        bool trigger = false;
        if (strncmp(time_buf, "EVERY ", 6) == 0) {
          // Periodic: [EVERY 60m]
          // For simplicity, we trigger if (now_min % period == 0)
          // and we haven't triggered this minute yet.
          // But wait, that's not robust.
          // Better: check if current time matches.
          // For "EVERY Nm", we might need a "last run" timestamp in the line.
          // For now, let's focus on one-time tasks first.
        } else {
          // One-time: [2026-02-13 14:00]
          int y, m, d, hh, mm;
          if (sscanf(time_buf, "%d-%d-%d %d:%d", &y, &m, &d, &hh, &mm) == 5) {
            if (now_tm.tm_year + 1900 == y && now_tm.tm_mon + 1 == m &&
                now_tm.tm_mday == d && now_tm.tm_hour == hh &&
                now_tm.tm_min == mm) {
              trigger = true;
            } else if (now_tm.tm_year + 1900 > y ||
                       (now_tm.tm_year + 1900 == y && now_tm.tm_mon + 1 > m) ||
                       (now_tm.tm_year + 1900 == y && now_tm.tm_mon + 1 == m &&
                        now_tm.tm_mday > d) ||
                       (now_tm.tm_year + 1900 == y && now_tm.tm_mon + 1 == m &&
                        now_tm.tm_mday == d && now_tm.tm_hour > hh) ||
                       (now_tm.tm_year + 1900 == y && now_tm.tm_mon + 1 == m &&
                        now_tm.tm_mday == d && now_tm.tm_hour == hh &&
                        now_tm.tm_min > mm)) {
              // Outdated task, mark as missed?
              // trigger = false;
            }
          }
        }

        if (trigger) {
          trigger_task(desc);
          // Mark as done
          char updated_line[300];
          snprintf(updated_line, sizeof(updated_line), "- [x] [%s] %s\n",
                   time_buf, desc);
          strcat(new_content, updated_line);
          modified = true;
          continue;
        }
      }
    }

    // Re-add newline if it was removed by parser
    if (strchr(line, '\n') == NULL)
      strcat(line, "\n");
    strcat(new_content, line);
  }
  fclose(f);

  if (modified) {
    ESP_LOGI(TAG, "Schedule modified, updating file");
    f = fopen(SCHEDULE_FILE, "w");
    if (f) {
      fputs(new_content, f);
      fclose(f);
    }
  }
  free(new_content);
}

static void scheduler_task(void *pvParameters) {
  ESP_LOGI(TAG, "Scheduler task started");

  while (1) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    /* Only check if time is actually set (year > 2025) */
    if (timeinfo.tm_year > (2025 - 1900)) {
      check_schedule();
    } else {
      static bool warned = false;
      if (!warned) {
        ESP_LOGW(TAG, "System time not synced. Scheduler waiting...");
        warned = true;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(60000)); // Check every minute
  }
}

esp_err_t scheduler_init(void) {
  xTaskCreate(scheduler_task, "scheduler_task", 4096, NULL, 5, NULL);
  return ESP_OK;
}

void scheduler_check_now(void) {
  // Force immediate check if needed
}
