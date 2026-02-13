#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#include "mimi_config.h"

/**
 * Initialize the LLM proxy. Reads API key and model from build-time secrets,
 * then NVS.
 */
esp_err_t llm_proxy_init(void);

/**
 * Save the Anthropic API key to NVS.
 */
esp_err_t llm_set_api_key(const char *api_key);

/**
 * Save the model identifier to NVS.
 */
esp_err_t llm_set_model(const char *model);
/**
 * Save the provider ID to NVS.
 */
esp_err_t llm_set_provider(int provider);

/**
 * Save the base URL to NVS.
 */
esp_err_t llm_set_base_url(const char *url);
esp_err_t llm_set_timezone(const char *tz);

/**
 * Profile Management
 */
esp_err_t llm_profile_use(const char *name);
esp_err_t llm_profile_del(const char *name);
void llm_profile_list(void);
const char *llm_get_active_profile(void);

/**
 * Send a chat completion request to Anthropic Messages API (streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages_json  JSON array of messages:
 * [{"role":"user","content":"..."},...]
 * @param response_buf   Output buffer for the complete response text
 * @param buf_size       Size of response_buf
 * @return ESP_OK on success
 */
esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size);

/* ── Tool Use Support ──────────────────────────────────────────── */

typedef struct {
  char id[64];             /* "toolu_xxx" */
  char name[32];           /* "web_search" */
  char *input;             /* heap-allocated JSON string */
  char *thought_signature; /* for Gemini compatibility */
  size_t input_len;
} llm_tool_call_t;

typedef struct {
  char *text; /* accumulated text blocks */
  size_t text_len;
  char *error_msg; /* heap-allocated error description if any */
  llm_tool_call_t calls[MIMI_MAX_TOOL_CALLS];
  int call_count;
  bool tool_use; /* stop_reason == "tool_use" */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/**
 * Send a chat completion request with tools to Anthropic Messages API
 * (streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages       cJSON array of messages (caller owns)
 * @param tools_json     Pre-built JSON string of tools array, or NULL for no
 * tools
 * @param resp           Output: structured response with text and tool calls
 * @return ESP_OK on success
 */
esp_err_t llm_chat_tools(const char *system_prompt, cJSON *messages,
                         const char *tools_json, llm_response_t *resp);

/**
 * Utility: Base64 encode data (uses PSRAM for output if dst is NULL).
 * @param src  Source data
 * @param slen Source length
 * @return Heap-allocated string (must be freed) or NULL.
 */
char *llm_util_base64_encode(const uint8_t *src, size_t slen);
