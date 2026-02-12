#include "agent_loop.h"
#include "agent/context_builder.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include "tools/tool_registry.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE (8 * 1024)

/* Build the assistant content array from llm_response_t for the messages
 * history. Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp) {
  cJSON *content = cJSON_CreateArray();

  /* Text block */
  if (resp->text && resp->text_len > 0) {
    cJSON *text_block = cJSON_CreateObject();
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", resp->text);
    cJSON_AddItemToArray(content, text_block);
  }

  /* Tool use blocks */
  for (int i = 0; i < resp->call_count; i++) {
    const llm_tool_call_t *call = &resp->calls[i];
    cJSON *tool_block = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_block, "type", "tool_use");
    cJSON_AddStringToObject(tool_block, "id", call->id);
    cJSON_AddStringToObject(tool_block, "name", call->name);

    cJSON *input = cJSON_Parse(call->input);
    if (input) {
      cJSON_AddItemToObject(tool_block, "input", input);
    } else {
      cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
    }

    if (call->thought_signature) {
      cJSON_AddStringToObject(tool_block, "thought_signature",
                              call->thought_signature);
    }
    cJSON_AddItemToArray(content, tool_block);
  }

  return content;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, char *tool_output,
                                 size_t tool_output_size) {
  cJSON *content = cJSON_CreateArray();

  for (int i = 0; i < resp->call_count; i++) {
    const llm_tool_call_t *call = &resp->calls[i];

    /* Execute tool */
    tool_output[0] = '\0';
    tool_registry_execute(call->name, call->input, tool_output,
                          tool_output_size);

    ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name,
             (int)strlen(tool_output));

    /* Build tool_result block */
    cJSON *result_block = cJSON_CreateObject();
    cJSON_AddStringToObject(result_block, "type", "tool_result");
    cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
    cJSON_AddStringToObject(result_block, "content", tool_output);
    cJSON_AddItemToArray(content, result_block);
  }

  return content;
}

static void agent_loop_task(void *arg) {
  ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

  /* Allocate large buffers from PSRAM */
  char *system_prompt =
      heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
  char *history_json =
      heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
  char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

  if (!system_prompt || !history_json || !tool_output) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
    vTaskDelete(NULL);
    return;
  }

  const char *tools_json = tool_registry_get_tools_json();

  while (1) {
    mimi_msg_t msg;
    esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
    if (err != ESP_OK)
      continue;

    ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);

    /* Store last active user channel/chat_id for scheduled events */
    static char last_active_channel[16] = MIMI_CHAN_TELEGRAM;
    static char last_active_chat_id[32] = "";

    if (strcmp(msg.channel, "scheduled") == 0) {
      if (last_active_chat_id[0] != '\0') {
        strncpy(msg.channel, last_active_channel, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, last_active_chat_id, sizeof(msg.chat_id) - 1);
      } else {
        ESP_LOGW(TAG, "Scheduled event triggered but no active chat_id");
        free(msg.content);
        continue;
      }
    } else {
      strncpy(last_active_channel, msg.channel,
              sizeof(last_active_channel) - 1);
      strncpy(last_active_chat_id, msg.chat_id,
              sizeof(last_active_chat_id) - 1);
    }

    /* 1. Build system prompt */
    context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);
    ESP_LOGI(TAG, "System Prompt (first 100 bytes): %.*s...", 100,
             system_prompt);

    /* 2. Load session history into cJSON array */
    session_get_history_json(msg.chat_id, history_json,
                             MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);

    cJSON *messages = cJSON_Parse(history_json);
    if (!messages)
      messages = cJSON_CreateArray();

    /* 3. Append current user message */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", msg.content);
    cJSON_AddItemToArray(messages, user_msg);
    ESP_LOGI(TAG, "User Message: %s", msg.content);

    /* 4. ReAct loop */
    char *final_text = NULL;
    int iteration = 0;
    char tool_usage_summary[256] = {0};
    llm_response_t resp;
    memset(&resp, 0, sizeof(resp));

    while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
      /* Send "working" indicator before each API call */
      {
        static const char *working_phrases[] = {
            "mimi\xF0\x9F\x98\x97is working...",
            "mimi\xF0\x9F\x90\xBE is thinking...",
            "mimi\xF0\x9F\x92\xAD is pondering...",
            "mimi\xF0\x9F\x8C\x99 is on it...",
            "mimi\xE2\x9C\xA8 is cooking...",
        };
        const int phrase_count =
            sizeof(working_phrases) / sizeof(working_phrases[0]);
        mimi_msg_t status = {0};
        strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
        strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
        status.content = strdup(working_phrases[esp_random() % phrase_count]);
        if (status.content)
          message_bus_push_outbound(&status);
      }

      err = llm_chat_tools(system_prompt, messages, tools_json, &resp);

      if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM call failed at iter %d: %s", iteration,
                 esp_err_to_name(err));
        break;
      }

      if (!resp.tool_use) {
        /* Normal completion — save final text and break */
        if (resp.text && resp.text_len > 0) {
          final_text = strdup(resp.text);
        }
        llm_response_free(&resp);
        break;
      }

      ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1,
               resp.call_count);

      /* Accumulate tool names for summary */
      for (int i = 0; i < resp.call_count; i++) {
        size_t current_len = strlen(tool_usage_summary);
        size_t name_len = strlen(resp.calls[i].name) + 4; // "[] " + null
        if (current_len + name_len < sizeof(tool_usage_summary)) {
          if (current_len > 0)
            strcat(tool_usage_summary, " ");
          strcat(tool_usage_summary, "[");
          strcat(tool_usage_summary, resp.calls[i].name);
          strcat(tool_usage_summary, "]");
        }
      }

      /* Append assistant message with content array */
      cJSON *asst_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(asst_msg, "role", "assistant");
      cJSON_AddItemToObject(asst_msg, "content",
                            build_assistant_content(&resp));
      cJSON_AddItemToArray(messages, asst_msg);

      /* Execute tools and append results */
      cJSON *tool_results =
          build_tool_results(&resp, tool_output, TOOL_OUTPUT_SIZE);

      // Log tool results
      char *res_str = cJSON_PrintUnformatted(tool_results);
      if (res_str) {
        ESP_LOGI(TAG, "Iteration %d Tool Results: %.*s...", iteration + 1, 200,
                 res_str);
        free(res_str);
      }

      cJSON *result_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(result_msg, "role", "user");
      cJSON_AddItemToObject(result_msg, "content", tool_results);
      cJSON_AddItemToArray(messages, result_msg);

      llm_response_free(&resp);
      iteration++;
    }

    cJSON_Delete(messages);

    /* 5. Send response */
    if (final_text && final_text[0]) {
      /* Prepend tool usage summary if exists */
      char *response_content = final_text;
      if (tool_usage_summary[0] != '\0') {
        ESP_LOGI(TAG, "Tool usage summary: %s", tool_usage_summary);
        size_t slen = strlen(tool_usage_summary) + strlen(final_text) + 2;
        char *combined = malloc(slen);
        if (combined) {
          snprintf(combined, slen, "%s\n%s", tool_usage_summary, final_text);
          free(final_text);
          response_content = combined;
        }
      }

      /* Save to session (only user text + final assistant text) */
      session_append(msg.chat_id, "user", msg.content);
      session_append(msg.chat_id, "assistant", response_content);

      /* Push response to outbound */
      mimi_msg_t out = {0};
      strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
      strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
      out.content = response_content; /* transfer ownership */
      message_bus_push_outbound(&out);
    } else {
      mimi_msg_t out = {0};
      strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
      strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
      if (resp.error_msg) {
        ESP_LOGI(TAG, "Sending LLM error to user: %s", resp.error_msg);
        size_t elen = strlen(resp.error_msg) + 64;
        out.content = malloc(elen);
        if (out.content) {
          snprintf(out.content, elen, "LLM Error: %s", resp.error_msg);
        }
      } else if (iteration >= MIMI_AGENT_MAX_TOOL_ITER) {
        ESP_LOGW(TAG, "Agent reached max tool iterations (%d)",
                 MIMI_AGENT_MAX_TOOL_ITER);
        out.content =
            strdup("Sorry, I reached the maximum tool reasoning depth (10 "
                   "steps) and could not finish my response.");
      } else {
        out.content = strdup("Sorry, I encountered an empty or malformed "
                             "response from the LLM.");
      }
      if (out.content) {
        message_bus_push_outbound(&out);
      }
      llm_response_free(&resp);
      if (final_text)
        free(final_text);
    }

    /* Free inbound message content */
    free(msg.content);

    /* Log memory status */
    ESP_LOGI(TAG, "Free PSRAM: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
}

esp_err_t agent_loop_init(void) {
  ESP_LOGI(TAG, "Agent loop initialized");
  return ESP_OK;
}

esp_err_t agent_loop_start(void) {
  BaseType_t ret =
      xTaskCreatePinnedToCore(agent_loop_task, "agent_loop", MIMI_AGENT_STACK,
                              NULL, MIMI_AGENT_PRIO, NULL, MIMI_AGENT_CORE);

  return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
