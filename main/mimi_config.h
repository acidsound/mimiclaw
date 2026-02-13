#pragma once

/* MimiClaw Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL ""
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY ""
#endif
#ifndef MIMI_SECRET_PROVIDER
#define MIMI_SECRET_PROVIDER MIMI_LLM_DEFAULT_PROVIDER
#endif
#ifndef MIMI_SECRET_BASE_URL
#define MIMI_SECRET_BASE_URL ""
#endif
#ifndef MIMI_SECRET_GROQ_KEY
#define MIMI_SECRET_GROQ_KEY ""
#endif
#ifndef MIMI_SECRET_STT_PROVIDER
#define MIMI_SECRET_STT_PROVIDER 0
#endif
#ifndef MIMI_SECRET_STT_KEY
#define MIMI_SECRET_STT_KEY MIMI_SECRET_GROQ_KEY
#endif
#ifndef MIMI_SECRET_STT_BASE_URL
#define MIMI_SECRET_STT_BASE_URL "https://api.groq.com/openai/v1"
#endif
#ifndef MIMI_SECRET_STT_MODEL
#define MIMI_SECRET_STT_MODEL "whisper-large-v3"
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY 10
#define MIMI_WIFI_RETRY_BASE_MS 1000
#define MIMI_WIFI_RETRY_MAX_MS 30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S 30
#define MIMI_TG_MAX_MSG_LEN 4096
#define MIMI_TG_POLL_STACK (12 * 1024)
#define MIMI_TG_POLL_PRIO 5
#define MIMI_TG_POLL_CORE 0
#define MIMI_MEDIA_STREAM_CHUNK (16 * 1024)
#define MIMI_MEDIA_MAX_PHOTO_BYTES (1024 * 1024)    /* 1 MB */
#define MIMI_MEDIA_MAX_VOICE_BYTES (400 * 1024)      /* 400 KB */
#define MIMI_MEDIA_MAX_VOICE_SECONDS 10

/* Agent Loop */
#define MIMI_AGENT_STACK (12 * 1024)
#define MIMI_AGENT_PRIO 6
#define MIMI_AGENT_CORE 1
#define MIMI_AGENT_MAX_HISTORY 20
#define MIMI_AGENT_MAX_TOOL_ITER 10
#define MIMI_MAX_TOOL_CALLS 4

/* Timezone (POSIX TZ format) */
/* Timezone (POSIX TZ format): Asia/Seoul is KST-9 */
#define MIMI_TIMEZONE "KST-9"

/* LLM */
/* LLM Providers */
#define MIMI_LLM_PROVIDER_ANTHROPIC 0
#define MIMI_LLM_PROVIDER_OPENAI 1
#define MIMI_STT_PROVIDER_GROQ 0

/* LLM Defaults */
#define MIMI_LLM_DEFAULT_PROVIDER MIMI_LLM_PROVIDER_ANTHROPIC
#define MIMI_LLM_DEFAULT_MODEL "claude-3-opus-20240229"
#define MIMI_LLM_MAX_TOKENS 4096
#define MIMI_LLM_API_URL_ANTHROPIC "https://api.anthropic.com/v1/messages"
#define MIMI_LLM_API_URL_OPENAI "https://api.openai.com/v1/chat/completions"
#define MIMI_STT_DEFAULT_PROVIDER MIMI_STT_PROVIDER_GROQ
#define MIMI_STT_DEFAULT_MODEL MIMI_SECRET_STT_MODEL
#define MIMI_STT_TRANSCRIBE_PATH "/audio/transcriptions"
#define MIMI_LLM_API_VERSION "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE (32 * 1024)

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN 8
#define MIMI_OUTBOUND_STACK (8 * 1024)
#define MIMI_OUTBOUND_PRIO 5
#define MIMI_OUTBOUND_CORE 0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE "/spiffs"
#define MIMI_SPIFFS_PUBLIC_DIR "/spiffs/public"
#define MIMI_SPIFFS_PRIVATE_DIR "/spiffs/private"

/* Public files (LLM accessible) */
#define MIMI_MEMORY_FILE "/spiffs/public/MEMORY.md"
#define MIMI_SOUL_FILE "/spiffs/public/SOUL.md"
#define MIMI_USER_FILE "/spiffs/public/USER.md"

/* Private files (Hidden from LLM) */
#define MIMI_SECRET_FILE "/spiffs/private/SECRET.env"
#define MIMI_SESSION_DB "/spiffs/private/SESSIONS.json"
#define MIMI_WOL_DEVICES_FILE "/spiffs/private/wol_devices.json"
#define MIMI_SPIFFS_HISTORY_DIR "/spiffs/h"

#define MIMI_CONTEXT_BUF_SIZE (16 * 1024)
#define MIMI_SESSION_MAX_MSGS 20

/* WebSocket Gateway */
#define MIMI_WS_PORT 18789
#define MIMI_WS_MAX_CLIENTS 4

/* Serial CLI */
#define MIMI_CLI_STACK (4 * 1024)
#define MIMI_CLI_PRIO 3
#define MIMI_CLI_CORE 0

/* NVS Namespaces */
#define MIMI_NVS_WIFI "wifi_config"
#define MIMI_NVS_TG "tg_config"
#define MIMI_NVS_LLM "llm_config"
#define MIMI_NVS_PROXY "proxy_config"
#define MIMI_NVS_SEARCH "search_config"
#define MIMI_NVS_MEDIA "media_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID "ssid"
#define MIMI_NVS_KEY_PASS "password"
#define MIMI_NVS_KEY_TG_TOKEN "bot_token"
#define MIMI_NVS_KEY_API_KEY "api_key"
#define MIMI_NVS_KEY_MODEL "model"
#define MIMI_NVS_KEY_PROXY_HOST "host"
#define MIMI_NVS_KEY_PROXY_PORT "port"
#define MIMI_NVS_KEY_PROVIDER "provider"
#define MIMI_NVS_KEY_BASE_URL "base_url"
#define MIMI_NVS_KEY_STT_PROVIDER "stt_provider"
#define MIMI_NVS_KEY_STT_BASE_URL "stt_base_url"
#define MIMI_NVS_KEY_STT_MODEL "stt_model"
#define MIMI_NVS_KEY_STT_KEY "stt_key"
#define MIMI_NVS_KEY_TIMEZONE "timezone"
#define MIMI_NVS_KEY_ACTIVE_PF "active_pf"
#define MIMI_NVS_KEY_PF_LIST "pf_list"
#define MIMI_NVS_KEY_MEDIA_PHOTO "photo_limit"
#define MIMI_NVS_KEY_MEDIA_VOICE_BYTES "voice_limit"
#define MIMI_NVS_KEY_MEDIA_VOICE_SECS "voice_secs"

/* Custom Error Codes */
#define ESP_ERR_ADMISSION_CONTROL 0x10B
