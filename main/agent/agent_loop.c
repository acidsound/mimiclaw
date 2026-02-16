#include "agent_loop.h"
#include "agent/context_builder.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "llm/llm_stt.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include "media/media_limits.h"
#include "gateway/ui_bridge.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_registry.h"

#include "utils/base64_stream.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
static void sanitize_tool_name_display(const char *in, char *out, size_t out_size);
static void append_tool_usage_summary(char *summary, size_t summary_size,
                                     const char *tool_name);
static uint32_t fnv1a_hash32(const char *s);
static bool contains_ci_token(const char *haystack, const char *token);
static bool extract_mac_from_text(const char *text, char *out, size_t out_size);
static bool is_unknown_or_empty(const char *s);
static bool contains_ci_token_bounded(const char *haystack, const char *token);
static bool contains_any_ci_token(const char *text, const char *const *tokens,
                                  size_t token_count);
static bool wol_call_has_selector(const char *input_json);
static bool infer_wol_selector_from_msg(const char *message, char *selector,
                                       size_t selector_size);
static bool infer_wol_tool_input_from_message(const char *message,
                                             char *out_input,
                                             size_t out_input_size);
static bool has_wol_intent_keyword(const char *text);
static bool extract_first_http_url(const char *text, char *out, size_t out_size);
static int infer_wait_after_tap_ms_from_text(const char *text);
static bool has_ios_sim_capture_intent(const char *text);
static bool try_fastpath_ios_sim_capture(const mimi_msg_t *msg, char *tool_output,
                                         size_t tool_output_size,
                                         char *tool_usage_summary,
                                         size_t tool_usage_summary_size);
static const char *normalize_tool_name_for_exec(const char *input_name,
                                              char *out_name,
                                              size_t out_name_size);
static bool enforce_photo_limit(const mimi_msg_t *msg, size_t file_size);
static bool enforce_voice_limit(const mimi_msg_t *msg, size_t file_size);
static bool inject_ui_tool_chat_id(const mimi_msg_t *msg, const char *tool_name,
                                   const char *raw_input, char *out_input,
                                   size_t out_input_size);
static void append_ui_capture_message(cJSON *messages, char **capture_refs,
                                      int *capture_ref_count, int max_refs);
static esp_err_t base64_chunk_cb(const uint8_t *data, size_t len, void *ctx);
static esp_err_t stt_stream_cb(const uint8_t *data, size_t len, void *ctx);
static esp_err_t transcribe_voice_media(const mimi_msg_t *msg, char *out_text,
                                       size_t max_out);
static char *encode_photo_to_base64(const mimi_msg_t *msg,
                                    const telegram_file_info_t *info);

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define STT_FALLBACK_MAX_BYTES (128 * 1024)
#define MAX_TOOL_SIGNATURES (16)
#define MAX_UI_CAPTURE_REFS 6

typedef struct {
  char name[32];
  uint32_t input_hash;
  size_t input_len;
} tool_signature_t;

typedef struct {
  tool_signature_t seen[MAX_TOOL_SIGNATURES];
  int seen_count;
  bool list_devices_called;
  bool wol_scan_started;
  bool wake_on_lan_called;
  int wol_scan_result_calls;
  int web_search_calls;
  int http_request_calls;
  int ui_capture_calls;
  int ui_action_calls;
  int ios_sim_capture_calls;
} tool_turn_state_t;

static bool try_fallback_wol_call(const mimi_msg_t *msg, char *tool_output,
                                 size_t tool_output_size,
                                 char *tool_usage_summary,
                                 size_t tool_usage_summary_size,
                                 tool_turn_state_t *turn_state);

static bool should_execute_tool(const llm_tool_call_t *call,
                               const char *canonical_name,
                               const char *tool_input,
                               tool_turn_state_t *state, char *skip_output,
                               size_t skip_output_size);

static void init_tool_turn_state(tool_turn_state_t *state) {
  if (!state)
    return;
  memset(state, 0, sizeof(*state));
}

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

static void sanitize_tool_name_display(const char *in, char *out, size_t out_size) {
  if (!out || out_size == 0)
    return;

  if (!in) {
    out[0] = '\0';
    return;
  }

  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < out_size; i++) {
    out[o++] = (in[i] == '_') ? '-' : in[i];
  }
  out[o] = '\0';
}

static void append_tool_usage_summary(char *summary, size_t summary_size,
                                     const char *tool_name) {
  if (!summary || !tool_name || summary_size == 0)
    return;

  char tool_name_safe[32];
  sanitize_tool_name_display(tool_name, tool_name_safe, sizeof(tool_name_safe));
  size_t current_len = strlen(summary);
  size_t entry_len = strlen(tool_name_safe) + 3; // brackets + optional space
  if (current_len + entry_len + 1 >= summary_size)
    return;

  if (current_len > 0)
    strcat(summary, " ");
  strcat(summary, "[");
  strcat(summary, tool_name_safe);
  strcat(summary, "]");
}

static uint32_t fnv1a_hash32(const char *s) {
  if (!s)
    return 2166136261u;

  uint32_t hash = 2166136261u;
  for (; *s; s++) {
    hash ^= (uint8_t)*s;
    hash *= 16777619u;
  }
  return hash;
}

static const char *normalize_tool_name_for_exec(const char *input_name,
                                              char *out_name,
                                              size_t out_name_size) {
  if (!input_name || !out_name || out_name_size == 0) {
    return input_name ? input_name : "";
  }

  size_t j = 0;
  bool last_sep = false;
  for (size_t i = 0; input_name[i] != '\0' && j + 1 < out_name_size; i++) {
    unsigned char c = (unsigned char)input_name[i];

    if (isalnum(c)) {
      out_name[j++] = (char)tolower(c);
      last_sep = false;
    } else if (c == '_' || c == '-' || c == ' ' || c == '\t' || c == '\n') {
      if (!last_sep) {
        out_name[j++] = '_';
        last_sep = true;
      }
    }
  }

  if (j > 0 && out_name[j - 1] == '_') {
    out_name[--j] = '\0';
  } else {
    out_name[j] = '\0';
  }

  if (strcmp(out_name, "wakeonlan") == 0) {
    snprintf(out_name, out_name_size, "wake_on_lan");
  } else if (strcmp(out_name, "websearch") == 0) {
    snprintf(out_name, out_name_size, "web_search");
  } else if (strcmp(out_name, "httprequest") == 0) {
    snprintf(out_name, out_name_size, "http_request");
  }

  return out_name;
}

static bool contains_ci_token(const char *haystack, const char *token) {
  if (!haystack || !token)
    return false;

  size_t token_len = strlen(token);
  if (token_len == 0)
    return true;

  for (size_t i = 0; haystack[i] != '\0'; i++) {
    size_t matched = 0;
    while (matched < token_len && haystack[i + matched] != '\0' &&
           tolower((unsigned char)haystack[i + matched]) ==
               tolower((unsigned char)token[matched])) {
      matched++;
    }
    if (matched == token_len) {
      char before = (i > 0) ? haystack[i - 1] : '\0';
      char after = haystack[i + token_len];

      const bool before_ok =
          (i == 0 || !isalnum((unsigned char)before) || before == '_');
      const bool after_ok =
          (after == '\0' || !isalnum((unsigned char)after) || after == '_');
      if (before_ok && after_ok)
        return true;
    }
  }

  return false;
}

static bool extract_mac_from_text(const char *text, char *out, size_t out_size) {
  if (!text || !out || out_size < 18)
    return false;

  for (const char *p = text; *p != '\0'; p++) {
    unsigned int b0, b1, b2, b3, b4, b5;
    int consumed = 0;

    if (sscanf(p, "%2x:%2x:%2x:%2x:%2x:%2x%n", &b0, &b1, &b2, &b3, &b4,
               &b5, &consumed) == 6) {
      char after = p[consumed];
      if (after == '\0' || isspace((unsigned char)after) || after == ',' ||
          after == '.' || after == ')' || after == ']' || after == '}') {
        snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", b0, b1, b2, b3,
                 b4, b5);
        return true;
      }
    }
  }

  return false;
}

static bool is_unknown_or_empty(const char *s) {
  return (!s || s[0] == '\0' || strcasecmp(s, "unknown") == 0);
}

static bool contains_ci_token_bounded(const char *haystack, const char *token) {
  if (!haystack || !token)
    return false;

  size_t token_len = strlen(token);
  if (token_len == 0)
    return true;

  for (size_t i = 0; haystack[i] != '\0'; i++) {
    size_t matched = 0;
    while (matched < token_len && haystack[i + matched] != '\0' &&
           tolower((unsigned char)haystack[i + matched]) ==
               tolower((unsigned char)token[matched])) {
      matched++;
    }
    if (matched == token_len) {
      char before = (i > 0) ? haystack[i - 1] : '\0';
      char after = haystack[i + token_len];

      const bool before_ok =
          (i == 0 || isspace((unsigned char)before) || before == '\0' ||
           before == ',' || before == '.' || before == ')' || before == ']' ||
           before == '}' || before == '!' || before == ':' || before == ';' ||
           before == '-' || before == '_' || before == '/' || before == '?');
      const bool after_ok =
          (after == '\0' || isspace((unsigned char)after) || after == ',' ||
           after == '.' || after == ')' || after == ']' || after == '}' ||
           after == '!' || after == ':' || after == ';' || after == '-' ||
           after == '_' || after == '/' || after == '?');
      if (before_ok && after_ok) {
        return true;
      }
    }
  }

  return false;
}

static bool contains_any_ci_token(const char *text, const char *const *tokens,
                                  size_t token_count) {
  if (!text || !tokens || token_count == 0) {
    return false;
  }
  for (size_t i = 0; i < token_count; i++) {
    if (tokens[i] && contains_ci_token(text, tokens[i])) {
      return true;
    }
  }
  return false;
}

static bool wol_call_has_selector(const char *input_json) {
  if (!input_json) {
    return false;
  }

  cJSON *input = cJSON_Parse(input_json);
  if (!input || !cJSON_IsObject(input)) {
    cJSON_Delete(input);
    return false;
  }

  const char *fields[] = {"mac", "device", "label", "hostname", "ip", NULL};
  bool has_selector = false;
  for (int i = 0; fields[i] && !has_selector; i++) {
    cJSON *node = cJSON_GetObjectItem(input, fields[i]);
    if (node && cJSON_IsString(node) &&
        !is_unknown_or_empty(node->valuestring)) {
      has_selector = true;
    }
  }

  cJSON_Delete(input);
  return has_selector;
}

static bool infer_wol_selector_from_msg(const char *message, char *selector,
                                       size_t selector_size) {
  if (!message || !selector || selector_size == 0) {
    return false;
  }

  char mac[20];
  if (extract_mac_from_text(message, mac, sizeof(mac))) {
    snprintf(selector, selector_size, "%s", mac);
    return true;
  }

  FILE *f = fopen(MIMI_WOL_DEVICES_FILE, "r");
  if (!f) {
    return false;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (file_size <= 0) {
    fclose(f);
    return false;
  }

  char *buf = malloc((size_t)file_size + 1);
  if (!buf) {
    fclose(f);
    return false;
  }

  if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
    free(buf);
    fclose(f);
    return false;
  }
  buf[file_size] = '\0';
  fclose(f);

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root || !cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return false;
  }

  char matched_selector[64] = {0};
  size_t match_count = 0;
  cJSON *item;
  cJSON_ArrayForEach(item, root) {
    if (!cJSON_IsObject(item)) {
      continue;
    }

    const char *fields[] = {"label", "name", "hostname", "ip", NULL};
    for (int i = 0; fields[i]; i++) {
      cJSON *node = cJSON_GetObjectItem(item, fields[i]);
      if (!node || !cJSON_IsString(node) ||
          is_unknown_or_empty(node->valuestring)) {
        continue;
      }
      if (contains_ci_token_bounded(message, node->valuestring)) {
        if (match_count == 0) {
          snprintf(matched_selector, sizeof(matched_selector), "%s",
                   node->valuestring);
        }
        match_count++;
        break;
      }
    }
  }

  cJSON_Delete(root);
  if (match_count == 1) {
    snprintf(selector, selector_size, "%s", matched_selector);
    return true;
  }

  return false;
}

static bool infer_wol_tool_input_from_message(const char *message, char *out_input,
                                             size_t out_input_size) {
  if (!message || !out_input || out_input_size == 0)
    return false;

  char selector[64];
  if (!infer_wol_selector_from_msg(message, selector, sizeof(selector))) {
    return false;
  }

  cJSON *input = cJSON_CreateObject();
  if (!input) {
    return false;
  }

  if (strchr(selector, ':')) {
    cJSON_AddStringToObject(input, "mac", selector);
  } else {
    cJSON_AddStringToObject(input, "device", selector);
  }

  char *json = cJSON_PrintUnformatted(input);
  cJSON_Delete(input);
  if (!json) {
    return false;
  }

  snprintf(out_input, out_input_size, "%s", json);
  free(json);
  return true;
}

static bool has_wol_intent_keyword(const char *text) {
  return contains_ci_token(text, "wol") ||
         contains_ci_token(text, "wake-on-lan") ||
         contains_ci_token(text, "wake on lan") ||
         contains_ci_token(text, "wake") ||
         contains_ci_token(text, "turn on") ||
         contains_ci_token(text, "power on") ||
         contains_ci_token(text, "mac") ||
         contains_ci_token(text, "computer") ||
         contains_ci_token(text, "pc");
}

static bool extract_first_http_url(const char *text, char *out, size_t out_size) {
  if (!text || !out || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  const char *https = strstr(text, "https://");
  const char *http = strstr(text, "http://");
  const char *start = NULL;
  if (https && http) {
    start = (https < http) ? https : http;
  } else {
    start = https ? https : http;
  }
  if (!start) {
    return false;
  }

  const char *end = start;
  while (*end) {
    unsigned char c = (unsigned char)*end;
    if (isspace(c) || c == '"' || c == '\'' || c == '<' || c == '>') {
      break;
    }
    end++;
  }

  size_t len = (size_t)(end - start);
  while (len > 0) {
    char tail = start[len - 1];
    if (tail == '.' || tail == ',' || tail == '!' || tail == '?' || tail == ')' ||
        tail == ']' || tail == '}' || tail == '"' || tail == '\'') {
      len--;
      continue;
    }
    break;
  }
  if (len == 0 || len + 1 > out_size) {
    return false;
  }

  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

static int infer_wait_after_tap_ms_from_text(const char *text) {
  if (!text || text[0] == '\0') {
    return 1000;
  }

  for (int sec = 1; sec <= 15; sec++) {
    char ko_pat1[16];
    char ko_pat2[16];
    char en_pat1[24];
    char en_pat2[24];
    snprintf(ko_pat1, sizeof(ko_pat1), "%d초", sec);
    snprintf(ko_pat2, sizeof(ko_pat2), "%d 초", sec);
    snprintf(en_pat1, sizeof(en_pat1), "%d sec", sec);
    snprintf(en_pat2, sizeof(en_pat2), "%d second", sec);
    if (strstr(text, ko_pat1) || strstr(text, ko_pat2) ||
        contains_ci_token(text, en_pat1) || contains_ci_token(text, en_pat2)) {
      return sec * 1000;
    }
  }

  return 1000;
}

static bool has_ios_sim_capture_intent(const char *text) {
  if (!text || text[0] == '\0') {
    return false;
  }

  static const char *const start_tokens[] = {
      "tap to start", "tap-to-start", "tap start", "start button",
      "tap to play",  "시작 버튼",    "tap 버튼",   "tap to begin"};
  static const char *const action_tokens[] = {"tap", "click", "press", "누르",
                                              "클릭"};
  static const char *const capture_tokens[] = {"화면",      "캡처",      "캡쳐",
                                               "screenshot", "screen shot", "찍어"};
  static const char *const send_tokens[] = {"전송", "보내", "send", "deliver"};
  static const char *const ios_tokens[] = {"아이폰", "iphone", "ios", "mobile",
                                           "safari", "시뮬레이터", "simulator"};

  const bool has_start_cue =
      contains_any_ci_token(text, start_tokens,
                            sizeof(start_tokens) / sizeof(start_tokens[0]));
  const bool has_action =
      contains_any_ci_token(text, action_tokens,
                            sizeof(action_tokens) / sizeof(action_tokens[0]));
  const bool has_capture =
      contains_any_ci_token(text, capture_tokens,
                            sizeof(capture_tokens) / sizeof(capture_tokens[0]));
  const bool has_send =
      contains_any_ci_token(text, send_tokens,
                            sizeof(send_tokens) / sizeof(send_tokens[0]));
  const bool has_ios_target =
      contains_any_ci_token(text, ios_tokens,
                            sizeof(ios_tokens) / sizeof(ios_tokens[0]));

  return has_start_cue && has_action && has_capture && has_send && has_ios_target;
}

static bool try_fastpath_ios_sim_capture(const mimi_msg_t *msg, char *tool_output,
                                         size_t tool_output_size,
                                         char *tool_usage_summary,
                                         size_t tool_usage_summary_size) {
  if (!msg || !tool_output || tool_output_size == 0) {
    return false;
  }
  if (strcmp(msg->channel, MIMI_CHAN_TELEGRAM) != 0 ||
      msg->type != MIMI_MSG_TYPE_TEXT || !msg->content || !msg->chat_id[0]) {
    return false;
  }

  if (!has_ios_sim_capture_intent(msg->content)) {
    return false;
  }

  char url[256] = {0};
  if (!extract_first_http_url(msg->content, url, sizeof(url))) {
    return false;
  }

  int wait_after_tap_ms = infer_wait_after_tap_ms_from_text(msg->content);
  int timeout_ms = 30000;
  int open_wait_ms = 2500;

  cJSON *input = cJSON_CreateObject();
  if (!input) {
    snprintf(tool_output, tool_output_size,
             "Error: failed to build iOS simulator request payload.");
    return true;
  }
  cJSON_AddStringToObject(input, "chat_id", MIMI_UI_DEFAULT_HELPER_CHAT_ID);
  cJSON_AddStringToObject(input, "tg_chat_id", msg->chat_id);
  cJSON_AddStringToObject(input, "url", url);
  cJSON_AddStringToObject(input, "tap_mode", "center");
  cJSON_AddNumberToObject(input, "open_wait_ms", open_wait_ms);
  cJSON_AddNumberToObject(input, "wait_after_tap_ms", wait_after_tap_ms);
  cJSON_AddNumberToObject(input, "timeout_ms", timeout_ms);
  char *input_json = cJSON_PrintUnformatted(input);
  cJSON_Delete(input);
  if (!input_json) {
    snprintf(tool_output, tool_output_size,
             "Error: failed to serialize iOS simulator request payload.");
    return true;
  }

  tool_output[0] = '\0';
  esp_err_t err = tool_registry_execute("ios_sim_capture_to_telegram", input_json,
                                        tool_output, tool_output_size);
  free(input_json);

  if (tool_usage_summary && tool_usage_summary_size > 0) {
    append_tool_usage_summary(tool_usage_summary, tool_usage_summary_size,
                              "ios_sim_capture_to_telegram");
  }

  if (tool_output[0] == '\0') {
    snprintf(tool_output, tool_output_size, "iOS simulator capture request handled.");
  }
  ESP_LOGI(TAG, "Fast-path iOS sim capture executed: %s", esp_err_to_name(err));
  return true;
}

static bool try_fallback_wol_call(const mimi_msg_t *msg, char *tool_output,
                                 size_t tool_output_size,
                                 char *tool_usage_summary,
                                 size_t tool_usage_summary_size,
                                 tool_turn_state_t *turn_state) {
  if (!msg || !tool_output || tool_output_size == 0 || !msg->content)
    return false;

  if (!has_wol_intent_keyword(msg->content))
    return false;

  char mac[20];
  if (!extract_mac_from_text(msg->content, mac, sizeof(mac)))
    return false;

  cJSON *input = cJSON_CreateObject();
  if (!input) {
    snprintf(tool_output, tool_output_size,
             "Error: failed to build fallback WOL input.");
    return true;
  }

  cJSON_AddStringToObject(input, "mac", mac);
  char *input_json = cJSON_PrintUnformatted(input);
  cJSON_Delete(input);
  if (!input_json) {
    snprintf(tool_output, tool_output_size, "Error: failed to build WOL input.");
    return true;
  }

  tool_output[0] = '\0';

  const char *tool_name = "wake_on_lan";
  llm_tool_call_t synthetic = {0};
  strncpy(synthetic.name, tool_name, sizeof(synthetic.name) - 1);
  synthetic.input = input_json;
  synthetic.input_len = strlen(input_json);

  char skip_output[128];
  bool execute =
      should_execute_tool(&synthetic, tool_name, synthetic.input, turn_state,
                         skip_output, sizeof(skip_output));
  if (execute) {
    tool_registry_execute(tool_name, synthetic.input, tool_output,
                         tool_output_size);
    append_tool_usage_summary(tool_usage_summary, tool_usage_summary_size,
                             tool_name);
  } else if (skip_output[0] != '\0') {
    snprintf(tool_output, tool_output_size, "%s", skip_output);
  }

  free(input_json);

  if (tool_output[0] == '\0') {
    snprintf(tool_output, tool_output_size,
             "I found a WOL request but could not execute it from this request.");
  }

  ESP_LOGI(TAG, "Fallback tool attempt: %s -> %s", tool_name, tool_output);
  return true;
}

static bool should_execute_tool(const llm_tool_call_t *call,
                               const char *canonical_name,
                               const char *tool_input,
                               tool_turn_state_t *state, char *skip_output,
                               size_t skip_output_size) {
  if (!call || call->name[0] == '\0') {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size,
               "Error: tool call missing name");
    }
    return false;
  }

  if (!state) {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size, "Error: internal tool execution state");
    }
    return false;
  }

  const char *input = tool_input ? tool_input : (call->input ? call->input : "{}");
  const char *name = canonical_name ? canonical_name : call->name;
  uint32_t in_hash = fnv1a_hash32(input);
  size_t in_len = strlen(input);
  const bool is_web_search = (strcmp(name, "web_search") == 0);
  const bool is_http_request = (strcmp(name, "http_request") == 0);
  const bool is_ui_capture = (strcmp(name, "ui_capture") == 0);
  const bool is_ui_action = (strcmp(name, "ui_action") == 0);
  const bool is_ios_sim_capture =
      (strcmp(name, "ios_sim_capture_to_telegram") == 0);

  for (int i = 0; i < state->seen_count; i++) {
    const tool_signature_t *sig = &state->seen[i];
    if (strncmp(sig->name, name, sizeof(sig->name)) == 0) {
      if (is_ui_capture || is_ui_action || is_ios_sim_capture) {
        continue;
      }
      if ((is_web_search || is_http_request) && sig->input_len == in_len &&
          sig->input_hash == in_hash) {
        if (skip_output && skip_output_size > 0) {
          snprintf(skip_output, skip_output_size,
                   "Skipped duplicate tool call: %s (same name/input in same turn)",
                   name);
        }
        return false;
      }

      if (skip_output && skip_output_size > 0) {
        snprintf(skip_output, skip_output_size,
                 "Skipped duplicate tool call: %s (already called in this turn)",
                 name);
      }
      return false;
    }
  }

  if (is_ui_capture && state->ui_capture_calls >= 4) {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size,
               "Skipped tool call: ui_capture limit reached for this turn");
    }
    return false;
  }

  if (is_ui_action && state->ui_action_calls >= 6) {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size,
               "Skipped tool call: ui_action limit reached for this turn");
    }
    return false;
  }

  if (is_ios_sim_capture && state->ios_sim_capture_calls >= 1) {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size,
               "Skipped tool call: ios_sim_capture_to_telegram limit reached for this turn");
    }
    return false;
  }

  if (is_web_search && state->web_search_calls >= 1) {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size,
               "Skipped duplicate tool call: web_search already used once this "
               "turn");
    }
    return false;
  }

  if (is_http_request && state->http_request_calls >= 1) {
    if (skip_output && skip_output_size > 0) {
      snprintf(skip_output, skip_output_size,
               "Skipped duplicate tool call: http_request already used once this "
               "turn");
    }
    return false;
  }

  if (strcmp(name, "list_devices") == 0) {
    if (state->list_devices_called) {
      if (skip_output && skip_output_size > 0) {
        snprintf(skip_output, skip_output_size,
                 "Skipped duplicate tool call: list_devices already called in this "
                 "turn");
      }
      return false;
    }
    state->list_devices_called = true;
  } else if (strcmp(name, "wol_scan_start") == 0) {
    if (state->wol_scan_started) {
      if (skip_output && skip_output_size > 0) {
        snprintf(skip_output, skip_output_size,
                 "Skipped duplicate tool call: wol_scan_start already called in "
                 "this turn");
      }
      return false;
    }
    state->wol_scan_started = true;
  } else if (strcmp(name, "wol_scan_result") == 0) {
    if (!state->wol_scan_started) {
      if (skip_output && skip_output_size > 0) {
        snprintf(skip_output, skip_output_size,
                 "Skipped tool call: wol_scan_result requires wol_scan_start "
                 "first");
      }
      return false;
    }
    if (state->wol_scan_result_calls >= 1) {
      if (skip_output && skip_output_size > 0) {
        snprintf(skip_output, skip_output_size,
                 "Skipped tool call: wol_scan_result already requested in this turn");
      }
      return false;
    }
    state->wol_scan_result_calls++;
  } else if (strcmp(name, "wake_on_lan") == 0) {
    if (state->wake_on_lan_called) {
      if (skip_output && skip_output_size > 0) {
        snprintf(skip_output, skip_output_size,
                 "Skipped duplicate tool call: wake_on_lan already attempted in "
                 "this turn");
      }
      return false;
    }
    state->wake_on_lan_called = true;
  }

  if (!is_ui_capture && !is_ui_action && !is_ios_sim_capture &&
      state->seen_count < MAX_TOOL_SIGNATURES) {
    tool_signature_t *sig = &state->seen[state->seen_count++];
    strncpy(sig->name, name, sizeof(sig->name) - 1);
    sig->name[sizeof(sig->name) - 1] = '\0';
    sig->input_hash = in_hash;
    sig->input_len = in_len;
  }

  if (skip_output && skip_output_size > 0) {
    skip_output[0] = '\0';
  }

  if (is_web_search) {
    state->web_search_calls++;
  } else if (is_http_request) {
    state->http_request_calls++;
  } else if (is_ui_capture) {
    state->ui_capture_calls++;
  } else if (is_ui_action) {
    state->ui_action_calls++;
  } else if (is_ios_sim_capture) {
    state->ios_sim_capture_calls++;
  }

  return true;
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

static bool inject_ui_tool_chat_id(const mimi_msg_t *msg, const char *tool_name,
                                   const char *raw_input, char *out_input,
                                   size_t out_input_size) {
  if (!msg || !tool_name || !raw_input || !out_input || out_input_size == 0) {
    return false;
  }
  bool is_ui_tool =
      (strcmp(tool_name, "ui_capture") == 0 || strcmp(tool_name, "ui_action") == 0 ||
       strcmp(tool_name, "ios_sim_capture_to_telegram") == 0);
  if (!is_ui_tool) {
    return false;
  }

  cJSON *in = cJSON_Parse(raw_input);
  if (!in || !cJSON_IsObject(in)) {
    if (in)
      cJSON_Delete(in);
    return false;
  }

  cJSON *chat_id = cJSON_GetObjectItem(in, "chat_id");
  if (!cJSON_IsString(chat_id) || chat_id->valuestring[0] == '\0') {
    if (strcmp(msg->channel, MIMI_CHAN_WEBSOCKET) == 0 && msg->chat_id[0] != '\0') {
      cJSON_AddStringToObject(in, "chat_id", msg->chat_id);
    } else {
      cJSON_AddStringToObject(in, "chat_id", MIMI_UI_DEFAULT_HELPER_CHAT_ID);
    }
  }

  cJSON *tg_chat_id = cJSON_GetObjectItem(in, "tg_chat_id");
  if (strcmp(msg->channel, MIMI_CHAN_TELEGRAM) == 0 && msg->chat_id[0] != '\0' &&
      (!cJSON_IsString(tg_chat_id) || tg_chat_id->valuestring[0] == '\0')) {
    cJSON_AddStringToObject(in, "tg_chat_id", msg->chat_id);
  }

  char *patched = cJSON_PrintUnformatted(in);
  cJSON_Delete(in);
  if (!patched) {
    return false;
  }

  size_t n = strlen(patched);
  bool ok = false;
  if (n + 1 <= out_input_size) {
    memcpy(out_input, patched, n + 1);
    ok = true;
  }
  free(patched);
  return ok;
}

static void append_ui_capture_message(cJSON *messages, char **capture_refs,
                                      int *capture_ref_count, int max_refs) {
  if (!messages || !capture_refs || !capture_ref_count || max_refs <= 0) {
    return;
  }

  ui_capture_frame_t frame = {0};
  if (!ui_bridge_take_latest_capture(&frame)) {
    return;
  }

  if (!frame.image_b64 || frame.image_b64[0] == '\0') {
    ui_bridge_free_capture(&frame);
    return;
  }
  if (*capture_ref_count >= max_refs) {
    ESP_LOGW(TAG, "Dropping UI capture (reference limit reached)");
    ui_bridge_free_capture(&frame);
    return;
  }

  cJSON *user_msg = cJSON_CreateObject();
  cJSON *content = cJSON_CreateArray();
  cJSON *text_block = cJSON_CreateObject();
  cJSON *img_block = cJSON_CreateObject();
  cJSON *source = cJSON_CreateObject();
  if (!user_msg || !content || !text_block || !img_block || !source) {
    cJSON_Delete(user_msg);
    cJSON_Delete(content);
    cJSON_Delete(text_block);
    cJSON_Delete(img_block);
    cJSON_Delete(source);
    ui_bridge_free_capture(&frame);
    return;
  }
  cJSON_AddStringToObject(user_msg, "role", "user");

  cJSON_AddStringToObject(text_block, "type", "text");

  char summary[256];
  snprintf(summary, sizeof(summary),
           "New screen capture received. request_id=%s size=%dx%d rotation=%d. "
           "Analyze this image and decide the next UI action using normalized "
           "coordinates (0.0 to 1.0).",
           frame.request_id, frame.width, frame.height, frame.rotation);
  cJSON_AddStringToObject(text_block, "text", summary);
  cJSON_AddItemToArray(content, text_block);

  cJSON_AddStringToObject(img_block, "type", "image");
  cJSON_AddStringToObject(source, "type", "base64");
  cJSON_AddStringToObject(source, "media_type",
                          frame.media_type[0] ? frame.media_type : "image/jpeg");
  cJSON *data_ref = cJSON_CreateStringReference(frame.image_b64);
  if (!data_ref) {
    cJSON_Delete(user_msg);
    cJSON_Delete(content);
    cJSON_Delete(img_block);
    cJSON_Delete(source);
    ui_bridge_free_capture(&frame);
    return;
  }
  cJSON_AddItemToObject(source, "data", data_ref);
  cJSON_AddItemToObject(img_block, "source", source);
  cJSON_AddItemToArray(content, img_block);
  cJSON_AddItemToObject(user_msg, "content", content);
  cJSON_AddItemToArray(messages, user_msg);

  capture_refs[*capture_ref_count] = frame.image_b64;
  (*capture_ref_count)++;
  frame.image_b64 = NULL;
  ui_bridge_free_capture(&frame);
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
static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
                                 char *tool_output, size_t tool_output_size,
                                 bool *web_no_result,
                                 char *web_no_result_output,
                                 size_t web_no_result_output_size,
                                 char *tool_usage_summary,
                                 size_t tool_usage_summary_size,
                                 tool_turn_state_t *tool_state) {
  cJSON *content = cJSON_CreateArray();

  for (int i = 0; i < resp->call_count; i++) {
    const llm_tool_call_t *call = &resp->calls[i];
    char canonical_name[32] = {0};
    const char *tool_name =
        normalize_tool_name_for_exec(call->name, canonical_name,
                                    sizeof(canonical_name));

    const char *raw_input =
        call->input ? call->input : "{}";
    const char *tool_input = raw_input;
    char inferred_input[192];
    char ui_input[1024];
    if (strcmp(tool_name, "wake_on_lan") == 0 &&
        !wol_call_has_selector(raw_input)) {
      if (infer_wol_tool_input_from_message(msg ? msg->content : NULL,
                                           inferred_input,
                                           sizeof(inferred_input))) {
        tool_input = inferred_input;
      } else if (msg && msg->content) {
        ESP_LOGW(TAG, "WOL call missing selector and no selector inferred from msg: %s",
                 msg->content);
      }
    }

    if (inject_ui_tool_chat_id(msg, tool_name, tool_input, ui_input,
                               sizeof(ui_input))) {
      tool_input = ui_input;
    }

    /* Execute tool */
    tool_output[0] = '\0';
    bool execute =
        should_execute_tool(call, tool_name, tool_input, tool_state, tool_output,
                           tool_output_size);
    if (execute) {
      tool_registry_execute(tool_name, tool_input, tool_output,
                            tool_output_size);
      append_tool_usage_summary(tool_usage_summary, tool_usage_summary_size,
                               tool_name);
    }

    ESP_LOGI(TAG, "Tool %s result: %d bytes", tool_name,
             (int)strlen(tool_output));

    if (web_no_result && strcmp(tool_name, "web_search") == 0 &&
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

    /* Fast-path for deterministic iOS simulator capture relay requests. */
    char fast_tool_usage_summary[128] = {0};
    tool_output[0] = '\0';
    if (try_fastpath_ios_sim_capture(&msg, tool_output, TOOL_OUTPUT_SIZE,
                                     fast_tool_usage_summary,
                                     sizeof(fast_tool_usage_summary))) {
      const bool fastpath_success =
          (strstr(tool_output, "iOS sim capture sent to Telegram. file_id=") != NULL);

      /* For success case, helper already delivered the image to Telegram.
       * Avoid sending extra confirmation text to keep UX to a single response. */
      if (fastpath_success) {
        session_append(msg.chat_id, "user", msg.content ? msg.content : "");
        session_append(msg.chat_id, "assistant",
                       "[ios-sim-capture-to-telegram] screenshot sent.");

        if (photo_b64_owned) {
          free(photo_b64_owned);
          photo_b64_owned = NULL;
        }
        free(msg.content);
        if (msg.media_id)
          free(msg.media_id);

        ESP_LOGI(TAG, "Fast-path iOS sim capture completed without extra text response");
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        continue;
      }

      char *response_content = strdup(tool_output[0] ? tool_output
                                                     : "iOS simulator capture request handled.");
      if (response_content && fast_tool_usage_summary[0] != '\0') {
        size_t slen =
            strlen(fast_tool_usage_summary) + strlen(response_content) + 2;
        char *combined = malloc(slen);
        if (combined) {
          snprintf(combined, slen, "%s\n%s", fast_tool_usage_summary,
                   response_content);
          free(response_content);
          response_content = combined;
        }
      }

      if (response_content) {
        session_append(msg.chat_id, "user", msg.content ? msg.content : "");
        session_append(msg.chat_id, "assistant", response_content);
        mimi_msg_t out = {0};
        strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
        out.content = response_content; /* transfer ownership */
        message_bus_push_outbound(&out);
      }

      if (photo_b64_owned) {
        free(photo_b64_owned);
        photo_b64_owned = NULL;
      }
      free(msg.content);
      if (msg.media_id)
        free(msg.media_id);

      ESP_LOGI(TAG, "Free PSRAM: %d bytes",
               (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      continue;
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
    char *ui_capture_refs[MAX_UI_CAPTURE_REFS] = {0};
    int ui_capture_ref_count = 0;

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
    tool_turn_state_t turn_state;
    init_tool_turn_state(&turn_state);

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
        if (iteration == 0 &&
            try_fallback_wol_call(&msg, tool_output, TOOL_OUTPUT_SIZE,
                                  tool_usage_summary,
                                  sizeof(tool_usage_summary), &turn_state)) {
          final_text = strdup(tool_output);
          llm_response_free(&resp);
          break;
        }

        /* Normal completion — save final text and break */
        if (resp.text && resp.text_len > 0) {
          final_text = strdup(resp.text);
        }
        llm_response_free(&resp);
        break;
      }

      if (resp.call_count == 0) {
        ESP_LOGW(TAG, "LLM requested tool use but returned no valid calls");
        final_text = strdup("No valid tool calls were returned by the model.");
        llm_response_free(&resp);
        iteration = MIMI_AGENT_MAX_TOOL_ITER;
        break;
      }

      ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1,
               resp.call_count);

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
          build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE,
                            &web_search_no_result, web_no_result_output,
                            TOOL_OUTPUT_SIZE, tool_usage_summary,
                            sizeof(tool_usage_summary), &turn_state);

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
      append_ui_capture_message(messages, ui_capture_refs, &ui_capture_ref_count,
                                MAX_UI_CAPTURE_REFS);

      llm_response_free(&resp);
      iteration++;
    }

    cJSON_Delete(messages);
    for (int i = 0; i < ui_capture_ref_count; i++) {
      free(ui_capture_refs[i]);
      ui_capture_refs[i] = NULL;
    }
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
