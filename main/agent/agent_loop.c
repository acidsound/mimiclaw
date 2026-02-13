#include "agent_loop.h"
#include "agent/context_builder.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "llm/llm_stt.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include "media/media_limits.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_registry.h"

#include "utils/base64_stream.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "agent";
static const char *VOICE_LIMIT_NOTICE =
    "I can only process short voice notes (about 10 seconds). Please resend a "
    "shorter clip.";
static const char *VOICE_STT_UNSET_NOTICE =
    "I cannot transcribe voice notes because STT is not configured. Please set "
    "the STT key first.";
static const char *VOICE_STT_GENERIC_NOTICE =
    "Voice transcription failed. Please send the message again in text or retry "
    "the voice note shortly.";
static const char *PHOTO_LIMIT_NOTICE =
    "That photo is too large for this device. Please resend it using Telegram's "
    "quick method so it stays under 1 MB.";

static void send_user_notice(const char *channel, const char *chat_id,
                             const char *text);
static bool enforce_photo_limit(const mimi_msg_t *msg, size_t file_size);
static bool enforce_voice_limit(const mimi_msg_t *msg, size_t file_size);
static esp_err_t base64_chunk_cb(const uint8_t *data, size_t len, void *ctx);
static esp_err_t stt_stream_cb(const uint8_t *data, size_t len, void *ctx);
static esp_err_t transcribe_voice_media(const mimi_msg_t *msg, char *out_text,
                                       size_t max_out);
static char *encode_photo_to_base64(const mimi_msg_t *msg,
                                    const telegram_file_info_t *info);

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define STT_FALLBACK_MAX_BYTES (128 * 1024)

static void send_user_notice(const char *channel, const char *chat_id,
                             const char *text) {
  if (!channel || !chat_id || !text)
    return;
  mimi_msg_t notice = {0};
  strncpy(notice.channel, channel, sizeof(notice.channel) - 1);
  strncpy(notice.chat_id, chat_id, sizeof(notice.chat_id) - 1);
  notice.type = MIMI_MSG_TYPE_TEXT;
  notice.content = strdup(text);
  if (notice.content)
    message_bus_push_outbound(&notice);
}

static bool enforce_photo_limit(const mimi_msg_t *msg, size_t file_size) {
  size_t size = file_size ? file_size : msg->media_size;
  if (size == 0)
    return true; /* Best effort if Telegram omitted size */
  return size <= media_limit_get_photo_bytes();
}

static bool enforce_voice_limit(const mimi_msg_t *msg, size_t file_size) {
  size_t size = file_size ? file_size : msg->media_size;
  if ((msg->media_duration > 0 &&
       msg->media_duration > media_limit_get_voice_seconds()) ||
      (size > 0 && size > media_limit_get_voice_bytes())) {
    return false;
  }
  return size > 0; /* need size for streaming upload */
}

static esp_err_t base64_chunk_cb(const uint8_t *data, size_t len, void *ctx) {
  base64_stream_t *stream = (base64_stream_t *)ctx;
  return base64_stream_append(stream, data, len);
}

static esp_err_t stt_stream_cb(const uint8_t *data, size_t len, void *ctx) {
  llm_stt_stream_t *stream = (llm_stt_stream_t *)ctx;
  return llm_stt_stream_write(stream, data, len);
}

typedef struct {
  uint8_t *buf;
  size_t len;
  size_t cap;
} stt_voice_buf_t;

static esp_err_t stt_fallback_collect_cb(const uint8_t *data, size_t len,
                                        void *ctx) {
  stt_voice_buf_t *vb = (stt_voice_buf_t *)ctx;
  if (!vb || !data)
    return ESP_ERR_INVALID_ARG;
  if (vb->len + len > vb->cap)
    return ESP_ERR_NO_MEM;
  memcpy(vb->buf + vb->len, data, len);
  vb->len += len;
  return ESP_OK;
}

static esp_err_t transcribe_voice_media_fallback(const telegram_file_info_t *info,
                                                char *out_text,
                                                size_t max_out) {
  if (!info || info->size == 0 || !out_text || max_out == 0)
    return ESP_ERR_INVALID_ARG;
  if (info->size > STT_FALLBACK_MAX_BYTES)
    return ESP_ERR_INVALID_SIZE;

  stt_voice_buf_t vb = {0};
  vb.cap = info->size;
  vb.buf = heap_caps_malloc(vb.cap, MALLOC_CAP_SPIRAM);
  if (!vb.buf) {
    ESP_LOGW(TAG, "Voice STT fallback alloc failed (size=%zu)", vb.cap);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = telegram_stream_file(info, MIMI_MEDIA_STREAM_CHUNK,
                                      stt_fallback_collect_cb, &vb);
  if (err == ESP_OK && vb.len == vb.cap) {
    ESP_LOGI(TAG, "Voice STT fallback collected %zu bytes", vb.len);
    err = llm_stt_transcribe(vb.buf, vb.len, out_text, max_out);
  } else if (err == ESP_OK) {
    ESP_LOGW(TAG, "Voice STT fallback incomplete (collected=%zu expected=%zu)",
             vb.len, vb.cap);
    err = ESP_ERR_INVALID_SIZE;
  }

  free(vb.buf);
  return err;
}

static char *encode_photo_to_base64(const mimi_msg_t *msg,
                                    const telegram_file_info_t *info) {
  base64_stream_t stream;
  size_t hint = info && info->size ? info->size : msg->media_size;
  base64_stream_init(&stream, hint ? (hint * 4 / 3) + 64 : 4096);
  esp_err_t err = telegram_stream_file(info, MIMI_MEDIA_STREAM_CHUNK,
                                       base64_chunk_cb, &stream);
  if (err != ESP_OK || base64_stream_finish(&stream) != ESP_OK) {
    base64_stream_free(&stream);
    return NULL;
  }
  char *data = base64_stream_claim(&stream);
  base64_stream_free(&stream);
  return data;
}

static esp_err_t transcribe_voice_media(const mimi_msg_t *msg,
                                       char *out_text, size_t max_out) {
  if (!msg->media_id)
    return ESP_ERR_INVALID_ARG;

  telegram_file_info_t info;
  esp_err_t err = telegram_get_file_info(msg->media_id, &info);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Voice STT: failed to resolve file info for media_id=%s (%s)",
             msg->media_id, esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "Voice STT: resolved file info: path=%s size=%zu duration=%d",
           info.path ? info.path : "n/a", (size_t)info.size, msg->media_duration);

  size_t size = info.size ? info.size : msg->media_size;
  if (!enforce_voice_limit(msg, size)) {
    telegram_file_info_free(&info);
    ESP_LOGW(TAG, "Voice STT: rejected by media limit (size=%zu, duration=%d)",
             size, (int)msg->media_duration);
    return ESP_ERR_INVALID_SIZE;
  }
  if (size == 0) {
    telegram_file_info_free(&info);
    ESP_LOGW(TAG, "Voice STT: zero-size media");
    return ESP_ERR_INVALID_SIZE;
  }
  bool use_fallback = (size <= STT_FALLBACK_MAX_BYTES);

  llm_stt_stream_t *stream = NULL;
  err = llm_stt_stream_begin(&stream, size);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Voice STT: stream begin failed (%s)", esp_err_to_name(err));
    if (use_fallback) {
      ESP_LOGW(TAG, "Voice STT: retrying via buffered fallback");
      err = transcribe_voice_media_fallback(&info, out_text, max_out);
    }
    telegram_file_info_free(&info);
    return err;
  }

  err = telegram_stream_file(&info, MIMI_MEDIA_STREAM_CHUNK, stt_stream_cb,
                             stream);
  if (err == ESP_OK) {
    err = llm_stt_stream_complete(stream, out_text, max_out);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Voice STT: stream complete ok");
    } else {
      ESP_LOGW(TAG, "Voice STT: stream complete failed (%s)",
               esp_err_to_name(err));
      if (use_fallback) {
        ESP_LOGW(TAG, "Voice STT: retrying via buffered fallback");
        err = transcribe_voice_media_fallback(&info, out_text, max_out);
      }
    }
  } else {
    ESP_LOGW(TAG, "Voice STT: telegram stream failed (%s)",
             esp_err_to_name(err));
    llm_stt_stream_abort(stream);
    if (use_fallback) {
      ESP_LOGW(TAG, "Voice STT: retrying via buffered fallback");
      err = transcribe_voice_media_fallback(&info, out_text, max_out);
    }
  }

  telegram_file_info_free(&info);
  return err;
}

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
                                 size_t tool_output_size, bool *web_no_result,
                                 char *web_no_result_output,
                                 size_t web_no_result_output_size) {
  cJSON *content = cJSON_CreateArray();

  for (int i = 0; i < resp->call_count; i++) {
    const llm_tool_call_t *call = &resp->calls[i];

    /* Execute tool */
    tool_output[0] = '\0';
    tool_registry_execute(call->name, call->input, tool_output,
                          tool_output_size);

    ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name,
             (int)strlen(tool_output));

    if (web_no_result && strcmp(call->name, "web_search") == 0 &&
        strncmp(tool_output, "No web results found for \"", 25) == 0) {
      *web_no_result = true;
      if (web_no_result_output && web_no_result_output_size > 0) {
        snprintf(web_no_result_output, web_no_result_output_size, "%s",
                 tool_output);
      }
    }

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

    char *photo_b64_owned = NULL;
    bool drop_message = false;
    if (msg.type == MIMI_MSG_TYPE_VOICE && msg.media_id) {
      char transcribed[1024] = {0};
      esp_err_t stt_err =
          transcribe_voice_media(&msg, transcribed, sizeof(transcribed));
      if (stt_err == ESP_OK) {
        ESP_LOGI(TAG, "STT Transcription: %s", transcribed);
        char *old_content = msg.content;
        size_t nlen =
            (old_content ? strlen(old_content) : 0) + strlen(transcribed) + 11;
        /* " [Voice: %s]" + nul */
        char *combined = malloc(nlen);
        if (combined) {
          snprintf(combined, nlen, "%s [Voice: %s]",
                   old_content ? old_content : "", transcribed);
          free(old_content);
          msg.content = combined;
        }
      } else if (stt_err == ESP_ERR_INVALID_SIZE) {
        send_user_notice(msg.channel, msg.chat_id, VOICE_LIMIT_NOTICE);
        drop_message = true;
      } else if (stt_err == ESP_ERR_INVALID_STATE) {
        send_user_notice(msg.channel, msg.chat_id, VOICE_STT_UNSET_NOTICE);
        drop_message = true;
      } else {
        ESP_LOGW(TAG,
                 "Voice STT failed: err=%s (media_id=%s size=%zu dur=%d)",
                 esp_err_to_name(stt_err), msg.media_id ? msg.media_id : "n/a",
                 (size_t)msg.media_size, (int)msg.media_duration);
        if (!msg.content || msg.content[0] == '\0') {
          send_user_notice(msg.channel, msg.chat_id, VOICE_STT_GENERIC_NOTICE);
          drop_message = true;
        } else {
          ESP_LOGW(TAG, "STT transcription failed (fallback to text): %s",
                   esp_err_to_name(stt_err));
        }
      }
    }

    if (!drop_message && msg.type == MIMI_MSG_TYPE_PHOTO && msg.media_id) {
      telegram_file_info_t info;
      if (telegram_get_file_info(msg.media_id, &info) == ESP_OK) {
        if (!enforce_photo_limit(&msg, info.size)) {
          send_user_notice(msg.channel, msg.chat_id, PHOTO_LIMIT_NOTICE);
          drop_message = true;
        } else {
          photo_b64_owned = encode_photo_to_base64(&msg, &info);
          if (!photo_b64_owned) {
            ESP_LOGW(TAG, "Failed to encode photo to base64");
          }
        }
        telegram_file_info_free(&info);
      } else {
        ESP_LOGW(TAG, "Failed to fetch Telegram file info for photo");
      }
    }

    if (drop_message) {
      free(msg.content);
      if (msg.media_id)
        free(msg.media_id);
      continue;
    }

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
        if (msg.media_id)
          free(msg.media_id);
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

    if (msg.type == MIMI_MSG_TYPE_PHOTO && msg.media_id && photo_b64_owned) {
      cJSON *content = cJSON_CreateArray();
      cJSON *text_block = cJSON_CreateObject();
      cJSON_AddStringToObject(text_block, "type", "text");
      cJSON_AddStringToObject(text_block, "text",
                              msg.content ? msg.content : "");
      cJSON_AddItemToArray(content, text_block);

      cJSON *img_block = cJSON_CreateObject();
      cJSON_AddStringToObject(img_block, "type", "image");
      cJSON *source = cJSON_CreateObject();
      cJSON_AddStringToObject(source, "type", "base64");
      cJSON_AddStringToObject(source, "media_type", "image/jpeg");
      cJSON *data_ref = cJSON_CreateStringReference(photo_b64_owned);
      if (data_ref) {
        cJSON_AddItemToObject(source, "data", data_ref);
      }
      cJSON_AddItemToObject(img_block, "source", source);
      cJSON_AddItemToArray(content, img_block);

      cJSON_AddItemToObject(user_msg, "content", content);
    } else {
      cJSON_AddStringToObject(user_msg, "content", msg.content);
    }
    cJSON_AddItemToArray(messages, user_msg);
    ESP_LOGI(TAG, "User Message: %s", msg.content);

    /* 4. ReAct loop */
    char *final_text = NULL;
    char *web_no_result_output = heap_caps_malloc(TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);
    if (!web_no_result_output) {
      ESP_LOGW(TAG, "Failed to allocate web search zero-result buffer");
    }
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
      bool web_search_no_result = false;
      if (web_no_result_output) {
        web_no_result_output[0] = '\0';
      }
      cJSON *tool_results =
          build_tool_results(&resp, tool_output, TOOL_OUTPUT_SIZE,
                            &web_search_no_result, web_no_result_output,
                            TOOL_OUTPUT_SIZE);

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

      if (web_search_no_result) {
        ESP_LOGI(TAG, "Stop loop early on web_search zero-result output");
        if (web_no_result_output && web_no_result_output[0] != '\0') {
          final_text = strdup(web_no_result_output);
        } else {
          final_text = strdup("No web results found for your query.");
        }
        cJSON_Delete(result_msg);
        llm_response_free(&resp);
        iteration = MIMI_AGENT_MAX_TOOL_ITER;
        break;
      }

      cJSON_AddItemToArray(messages, result_msg);

      llm_response_free(&resp);
      iteration++;
    }

    cJSON_Delete(messages);
    if (photo_b64_owned) {
      free(photo_b64_owned);
      photo_b64_owned = NULL;
    }

    if (web_no_result_output) {
      free(web_no_result_output);
      web_no_result_output = NULL;
    }

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
    if (msg.media_id)
      free(msg.media_id);

    /* Log memory status */
    ESP_LOGI(TAG, "Free PSRAM: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
}

esp_err_t agent_loop_init(void) {
  ESP_LOGI(TAG, "Agent loop initialized");
  llm_stt_init();
  return ESP_OK;
}

esp_err_t agent_loop_start(void) {
  BaseType_t ret =
      xTaskCreatePinnedToCore(agent_loop_task, "agent_loop", MIMI_AGENT_STACK,
                              NULL, MIMI_AGENT_PRIO, NULL, MIMI_AGENT_CORE);

  return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
