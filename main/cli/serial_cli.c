#include "serial_cli.h"
#include "llm/llm_proxy.h"
#include "llm/llm_stt.h"
#include "media/media_limits.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "wifi/wifi_manager.h"

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char *TAG = "cli";

static void resolve_path(const char *input, char *output, size_t size) {
  if (input[0] == '/') {
    snprintf(output, size, "%s", input);
  } else {
    snprintf(output, size, "%s/%s", MIMI_SPIFFS_BASE, input);
  }
}

/* --- wifi_set command --- */
static struct {
  struct arg_str *ssid;
  struct arg_str *password;
  struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, wifi_set_args.end, argv[0]);
    return 1;
  }
  wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                               wifi_set_args.password->sval[0]);
  printf("WiFi credentials saved. Restart to apply.\n");
  return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv) {
  printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
  printf("IP: %s\n", wifi_manager_get_ip());
  return 0;
}

/* --- wifi_reset command --- */
static int cmd_wifi_reset(int argc, char **argv) {
  esp_err_t err = wifi_manager_reset_credentials();
  if (err != ESP_OK) {
    printf("Failed to reset WiFi credentials: %s\n", esp_err_to_name(err));
    return 1;
  }

  printf("WiFi credentials cleared (including secret fallback disable).\n");
  printf("Use 'wifi_portal' or 'wifi_set <ssid> <password>'.\n");
  return 0;
}

/* --- wifi_portal command --- */
static struct {
  struct arg_int *timeout_sec;
  struct arg_end *end;
} wifi_portal_args;

static int cmd_wifi_portal(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&wifi_portal_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, wifi_portal_args.end, argv[0]);
    return 1;
  }

  uint32_t timeout_ms = MIMI_WIFI_PROV_TIMEOUT_MS;
  if (wifi_portal_args.timeout_sec->count > 0) {
    int sec = wifi_portal_args.timeout_sec->ival[0];
    if (sec <= 0) {
      printf("timeout_sec must be > 0\n");
      return 1;
    }
    timeout_ms = (uint32_t)sec * 1000U;
  }

  printf("Starting WiFi provisioning portal (timeout=%u ms)...\n",
         (unsigned)timeout_ms);
  esp_err_t err = wifi_manager_run_provisioning_portal(timeout_ms);
  if (err == ESP_OK && wifi_manager_is_connected()) {
    printf("Provisioning successful. IP: %s\n", wifi_manager_get_ip());
    return 0;
  }

  printf("Provisioning not completed.\n");
  return 1;
}

/* --- set_tg_token command --- */
static struct {
  struct arg_str *token;
  struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, tg_token_args.end, argv[0]);
    return 1;
  }
  telegram_set_token(tg_token_args.token->sval[0]);
  printf("Telegram bot token saved.\n");
  return 0;
}

/* --- set_api_key command --- */
static struct {
  struct arg_str *key;
  struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, api_key_args.end, argv[0]);
    return 1;
  }
  llm_set_api_key(api_key_args.key->sval[0]);
  printf("API key saved.\n");
  return 0;
}

/* --- set_model command --- */
static struct {
  struct arg_str *model;
  struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&model_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, model_args.end, argv[0]);
    return 1;
  }
  llm_set_model(model_args.model->sval[0]);
  printf("Model set.\n");
  return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv) {
  char *buf = malloc(4096);
  if (!buf) {
    printf("Out of memory.\n");
    return 1;
  }
  if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
    printf("=== MEMORY.md ===\n%s\n=================\n", buf);
  } else {
    printf("MEMORY.md is empty or not found.\n");
  }
  free(buf);
  return 0;
}

/* --- memory_write command --- */
static struct {
  struct arg_str *content;
  struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, memory_write_args.end, argv[0]);
    return 1;
  }
  memory_write_long_term(memory_write_args.content->sval[0]);
  printf("MEMORY.md updated.\n");
  return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv) {
  printf("Sessions:\n");
  session_list();
  return 0;
}

/* --- session_clear command --- */
static struct {
  struct arg_str *chat_id;
  struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, session_clear_args.end, argv[0]);
    return 1;
  }
  if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
    printf("Session cleared.\n");
  } else {
    printf("Session not found.\n");
  }
  return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv) {
  printf("Internal free: %d bytes\n",
         (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  printf("PSRAM free:    %d bytes\n",
         (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  printf("Total free:    %d bytes\n", (int)esp_get_free_heap_size());
  return 0;
}

/* --- set_proxy command --- */
static struct {
  struct arg_str *host;
  struct arg_int *port;
  struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, proxy_args.end, argv[0]);
    return 1;
  }
  http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
  printf("Proxy set. Restart to apply.\n");
  return 0;
}

/* --- set_stt_key command --- */
static struct {
  struct arg_str *key;
  struct arg_end *end;
} stt_key_args;

static int cmd_set_stt_key(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&stt_key_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, stt_key_args.end, argv[0]);
    return 1;
  }
  llm_stt_set_key(stt_key_args.key->sval[0]);
  printf("STT key saved.\n");
  return 0;
}

/* --- set_stt_model command --- */
static struct {
  struct arg_str *model;
  struct arg_end *end;
} stt_model_args;

static int cmd_set_stt_model(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&stt_model_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, stt_model_args.end, argv[0]);
    return 1;
  }
  llm_stt_set_model(stt_model_args.model->sval[0]);
  printf("STT model saved.\n");
  return 0;
}

/* --- set_stt_base_url command --- */
static struct {
  struct arg_str *url;
  struct arg_end *end;
} stt_base_url_args;

static int cmd_set_stt_base_url(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&stt_base_url_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, stt_base_url_args.end, argv[0]);
    return 1;
  }
  llm_stt_set_base_url(stt_base_url_args.url->sval[0]);
  printf("STT base URL saved.\n");
  return 0;
}

/* --- set_stt_provider command --- */
static struct {
  struct arg_str *provider;
  struct arg_end *end;
} stt_provider_args;

static int cmd_set_stt_provider(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&stt_provider_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, stt_provider_args.end, argv[0]);
    return 1;
  }
  const char *val = stt_provider_args.provider->sval[0];
  int p = -1;
  if (strcasecmp(val, "groq") == 0 || strcasecmp(val, "g") == 0) {
    p = MIMI_STT_PROVIDER_GROQ;
  } else {
    printf("Invalid provider. Use 'groq'.\n");
    return 1;
  }
  if (llm_stt_set_provider(p) != ESP_OK) {
    printf("Failed to save STT provider.\n");
    return 1;
  }
  printf("STT provider set to %s (%d).\n", val, p);
  return 0;
}

/* --- set_media_limits command --- */
static struct {
  struct arg_int *photo_kb;
  struct arg_int *voice_kb;
  struct arg_int *voice_secs;
  struct arg_end *end;
} media_limit_args;

static int cmd_set_media_limits(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&media_limit_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, media_limit_args.end, argv[0]);
    return 1;
  }
  size_t photo_bytes = (size_t)media_limit_args.photo_kb->ival[0] * 1024;
  size_t voice_bytes = (size_t)media_limit_args.voice_kb->ival[0] * 1024;
  int voice_secs = media_limit_args.voice_secs->ival[0];
  media_limit_set_photo_bytes(photo_bytes);
  media_limit_set_voice_bytes(voice_bytes);
  media_limit_set_voice_seconds(voice_secs);
  printf("Media limits updated: photo=%d KB, voice=%d KB/%d s\n",
         media_limit_args.photo_kb->ival[0], media_limit_args.voice_kb->ival[0],
         voice_secs);
  return 0;
}
static int cmd_clear_proxy(int argc, char **argv) {
  http_proxy_clear();
  printf("Proxy cleared. Restart to apply.\n");
  return 0;
}

/* --- set_search_key command --- */
static struct {
  struct arg_str *key;
  struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, search_key_args.end, argv[0]);
    return 1;
  }
  tool_web_search_set_key(search_key_args.key->sval[0]);
  printf("Search API key saved.\n");
  return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask) {
  char nvs_val[128] = {0};
  const char *source = "not set";
  const char *display = "(empty)";
  nvs_handle_t nvs;

  /* 1. Try Active Profile first if it's an LLM-related namespace */
  bool is_llm =
      (strcmp(ns, MIMI_NVS_LLM) == 0 || strcmp(ns, MIMI_NVS_PROXY) == 0);
  if (is_llm) {
    char pf_ns[16];
    snprintf(pf_ns, sizeof(pf_ns), "pf_%.12s", llm_get_active_profile());
    if (nvs_open(pf_ns, NVS_READONLY, &nvs) == ESP_OK) {
      size_t len = sizeof(nvs_val);
      if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
        source = "PF"; // Profile
        display = nvs_val;
      }
      nvs_close(nvs);
    }
  }

  /* 2. Try Global NVS (backward compat) if not found in PF */
  if (strcmp(source, "not set") == 0) {
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
      size_t len = sizeof(nvs_val);
      if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
        source = "NVS";
        display = nvs_val;
      }
      nvs_close(nvs);
    }
  }

  /* 3. Fall back to build-time value */
  if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
    source = "build";
    display = build_val;
  }

  if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
    printf("  %-14s: %.4s****  [%s]\n", label, display, source);
  } else {
    printf("  %-14s: %s  [%s]\n", label, display, source);
  }
}

static int cmd_config_show(int argc, char **argv) {
  printf("=== Current Configuration ===\n");
  print_config("WiFi SSID", MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID,
               MIMI_SECRET_WIFI_SSID, false);
  print_config("WiFi Pass", MIMI_NVS_WIFI, MIMI_NVS_KEY_PASS,
               MIMI_SECRET_WIFI_PASS, true);
  print_config("TG Token", MIMI_NVS_TG, MIMI_NVS_KEY_TG_TOKEN,
               MIMI_SECRET_TG_TOKEN, true);

  printf("--- LLM Profile: %s ---\n", llm_get_active_profile());
  print_config("API Key", MIMI_NVS_LLM, MIMI_NVS_KEY_API_KEY,
               MIMI_SECRET_API_KEY, true);

  print_config("Model", MIMI_NVS_LLM, MIMI_NVS_KEY_MODEL, MIMI_SECRET_MODEL,
               false);
  print_config("Proxy Host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST,
               MIMI_SECRET_PROXY_HOST, false);
  print_config("Proxy Port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT,
               MIMI_SECRET_PROXY_PORT, false);

  /* Provider & URL */
  char prov_str[16];
  int p = MIMI_LLM_DEFAULT_PROVIDER;
#ifdef MIMI_SECRET_PROVIDER
  p = MIMI_SECRET_PROVIDER;
#endif
  /* Check NVS for provider */
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    int32_t val;
    if (nvs_get_i32(nvs, MIMI_NVS_KEY_PROVIDER, &val) == ESP_OK)
      p = (int)val;
    nvs_close(nvs);
  }
  snprintf(prov_str, sizeof(prov_str), "%s",
           (p == MIMI_LLM_PROVIDER_ANTHROPIC) ? "Anthropic" : "OpenAI/Kimi");
  printf("  %-14s: %s  [%s]\n", "Provider", prov_str, "mixed");

  print_config("Base URL", MIMI_NVS_LLM, MIMI_NVS_KEY_BASE_URL,
               MIMI_SECRET_BASE_URL, false);
  print_config("Timezone", MIMI_NVS_LLM, MIMI_NVS_KEY_TIMEZONE, MIMI_TIMEZONE,
               false);
  printf("--- STT Profile ---\n");
  print_config("STT Key", MIMI_NVS_LLM, MIMI_NVS_KEY_STT_KEY,
               MIMI_SECRET_STT_KEY, true);
  print_config("STT Model", MIMI_NVS_LLM, MIMI_NVS_KEY_STT_MODEL,
               MIMI_SECRET_STT_MODEL, false);
  print_config("STT Base URL", MIMI_NVS_LLM, MIMI_NVS_KEY_STT_BASE_URL,
               MIMI_SECRET_STT_BASE_URL, false);
  char stt_provider_str[16];
  const char *stt_source = "build";
  int stt_provider = MIMI_STT_DEFAULT_PROVIDER;
#ifdef MIMI_SECRET_STT_PROVIDER
  stt_provider = MIMI_SECRET_STT_PROVIDER;
#endif
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    int32_t val;
    if (nvs_get_i32(nvs, MIMI_NVS_KEY_STT_PROVIDER, &val) == ESP_OK) {
      stt_provider = (int)val;
      stt_source = "NVS";
    }
    nvs_close(nvs);
  }
  snprintf(stt_provider_str, sizeof(stt_provider_str), "%s (%d)",
           (stt_provider == MIMI_STT_PROVIDER_GROQ) ? "Groq" : "Unknown",
           stt_provider);
  printf("  %-14s: %s  [%s]\n", "STT Provider", stt_provider_str,
         stt_source);
  print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,
               MIMI_SECRET_SEARCH_KEY, true);
  printf("=============================\n");
  return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv) {
  const char *namespaces[] = {MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM,
                              MIMI_NVS_PROXY, MIMI_NVS_SEARCH};
  for (int i = 0; i < 5; i++) {
    nvs_handle_t nvs;
    if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_erase_all(nvs);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }
  printf(
      "All NVS config cleared. Build-time defaults will be used on restart.\n");
  return 0;
}

/* --- tg_auth_add command --- */
static struct {
  struct arg_str *chat_id;
  struct arg_end *end;
} tg_auth_add_args;

static int cmd_tg_auth_add(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&tg_auth_add_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, tg_auth_add_args.end, argv[0]);
    return 1;
  }
  int64_t cid = atoll(tg_auth_add_args.chat_id->sval[0]);
  telegram_auth_add(cid);
  printf("Chat %lld authorized.\n", cid);
  return 0;
}

/* --- tg_auth_remove command --- */
static struct {
  struct arg_str *chat_id;
  struct arg_end *end;
} tg_auth_remove_args;

static int cmd_tg_auth_remove(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&tg_auth_remove_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, tg_auth_remove_args.end, argv[0]);
    return 1;
  }
  int64_t cid = atoll(tg_auth_remove_args.chat_id->sval[0]);
  telegram_auth_remove(cid);
  printf("Chat %lld deauthorized.\n", cid);
  return 0;
}

/* --- tg_auth_list command --- */
static int cmd_tg_auth_list(int argc, char **argv) {
  telegram_auth_list();
  return 0;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv) {
  printf("Restarting...\n");
  esp_restart();
  return 0; /* unreachable */
}

/* --- set_provider command --- */
static struct {
  struct arg_str *provider;
  struct arg_end *end;
} provider_args;

static int cmd_set_provider(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&provider_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, provider_args.end, argv[0]);
    return 1;
  }
  const char *val = provider_args.provider->sval[0];
  int p = -1;
  if (strcasecmp(val, "anthropic") == 0 || strcasecmp(val, "ant") == 0)
    p = MIMI_LLM_PROVIDER_ANTHROPIC;
  else if (strcasecmp(val, "openai") == 0 || strcasecmp(val, "oa") == 0)
    p = MIMI_LLM_PROVIDER_OPENAI;
  else if (strcasecmp(val, "kimi") == 0 || strcasecmp(val, "gemini") == 0 ||
           strcasecmp(val, "deepseek") == 0 || strcasecmp(val, "ds") == 0)
    p = MIMI_LLM_PROVIDER_OPENAI;

  if (p >= 0) {
    llm_set_provider(p);
    printf("Provider for '%s' set to %s (%d).\n", llm_get_active_profile(), val,
           p);
  } else {
    printf("Invalid provider. Use 'ant' or 'oa' or aliases (gemini, ds).\n");
  }
  return 0;
}

/* --- set_base_url command --- */
static struct {
  struct arg_str *url;
  struct arg_end *end;
} base_url_args;

static int cmd_set_base_url(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&base_url_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, base_url_args.end, argv[0]);
    return 1;
  }
  llm_set_base_url(base_url_args.url->sval[0]);
  printf("Base URL saved.\n");
  return 0;
}

/* --- set_timezone command --- */
static struct {
  struct arg_str *tz;
  struct arg_end *end;
} timezone_args;

static int cmd_set_timezone(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&timezone_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, timezone_args.end, argv[0]);
    return 1;
  }
  llm_set_timezone(timezone_args.tz->sval[0]);
  printf("Timezone set to %s. Restart to apply to SNTP.\n",
         timezone_args.tz->sval[0]);
  return 0;
}

/* --- pf_use command --- */
static struct {
  struct arg_str *name;
  struct arg_end *end;
} pf_use_args;

static int cmd_pf_use(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&pf_use_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, pf_use_args.end, argv[0]);
    return 1;
  }
  if (llm_profile_use(pf_use_args.name->sval[0]) == ESP_OK) {
    printf("Switched to profile: %s\n", pf_use_args.name->sval[0]);
  } else {
    printf("Error: Profile name too long or invalid.\n");
  }
  return 0;
}

/* --- pf_ls command --- */
static int cmd_pf_ls(int argc, char **argv) {
  llm_profile_list();
  return 0;
}

/* --- pf_del command --- */
static struct {
  struct arg_str *name;
  struct arg_end *end;
} pf_del_args;

static int cmd_pf_del(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&pf_del_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, pf_del_args.end, argv[0]);
    return 1;
  }
  if (llm_profile_del(pf_del_args.name->sval[0]) == ESP_OK) {
    printf("Profile deleted: %s\n", pf_del_args.name->sval[0]);
  } else {
    printf("Error: Profile not found or protected.\n");
  }
  return 0;
}

/* --- clear_history command --- */
static int cmd_clear_history(int argc, char **argv) {
  DIR *dir = opendir(MIMI_SPIFFS_BASE "/h");
  if (!dir) {
    printf("Error: history directory not found.\n");
    return 1;
  }
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_type == DT_REG) {
      char path[300];
      snprintf(path, sizeof(path), MIMI_SPIFFS_BASE "/h/%s", ent->d_name);
      unlink(path);
    }
  }
  closedir(dir);
  printf("Session history cleared.\n");
  return 0;
}

/* --- ls command --- */
static struct {
  struct arg_str *path;
  struct arg_end *end;
} ls_args;

static int cmd_ls(int argc, char **argv) {
  arg_parse(argc, argv, (void **)&ls_args);
  char target_dir[256];
  if (ls_args.path->count > 0) {
    resolve_path(ls_args.path->sval[0], target_dir, sizeof(target_dir));
  } else {
    strcpy(target_dir, MIMI_SPIFFS_BASE);
  }

  /* SPIFFS is often flat, but we simulate directories by prefix filtering */
  DIR *dir = opendir(MIMI_SPIFFS_BASE);
  if (!dir) {
    perror("ls failed");
    return 1;
  }
  struct dirent *ent;
  printf("Listing %s:\n", target_dir);
  while ((ent = readdir(dir)) != NULL) {
    struct stat st;
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE,
             ent->d_name);

    /* If target_dir is not the root, filter by prefix */
    if (strcmp(target_dir, MIMI_SPIFFS_BASE) != 0) {
      if (strncmp(full_path, target_dir, strlen(target_dir)) != 0) {
        continue;
      }
    }

    stat(full_path, &st);
    if (S_ISDIR(st.st_mode)) {
      printf("  [DIR]  %s\n", ent->d_name);
    } else {
      printf("  %-16s  %ld bytes\n", ent->d_name, (long)st.st_size);
    }
  }
  closedir(dir);
  return 0;
}

/* --- cat command (with paging) --- */
static struct {
  struct arg_str *path;
  struct arg_end *end;
} cat_args;

static int cmd_cat(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&cat_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, cat_args.end, argv[0]);
    return 1;
  }

  char full_path[512];
  resolve_path(cat_args.path->sval[0], full_path, sizeof(full_path));

  FILE *f = fopen(full_path, "r");
  if (!f) {
    perror("cat failed");
    return 1;
  }

  char buf[512];
  size_t read_bytes;
  int lines_printed = 0;
  const int PAGE_LINES = 20;

  while ((read_bytes = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
    buf[read_bytes] = '\0';
    for (size_t i = 0; i < read_bytes; i++) {
      putchar(buf[i]);
      if (buf[i] == '\n') {
        lines_printed++;
        if (lines_printed >= PAGE_LINES) {
          printf("\n-- More -- (Space/Enter: next, q: quit)");
          fflush(stdout);
          int c = getchar();
          if (c == 'q' || c == 'Q') {
            printf("\n");
            fclose(f);
            return 0;
          }
          lines_printed = 0;
        }
      }
    }
  }

  fclose(f);
  return 0;
}

/* --- rm command --- */
static struct {
  struct arg_str *path;
  struct arg_end *end;
} rm_args;

static int cmd_rm(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&rm_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, rm_args.end, argv[0]);
    return 1;
  }
  char full_path[512];
  resolve_path(rm_args.path->sval[0], full_path, sizeof(full_path));
  if (unlink(full_path) == 0) {
    printf("Deleted: %s\n", full_path);
  } else {
    perror("rm failed");
  }
  return 0;
}

/* --- set_clock command --- */
static struct {
  struct arg_str *date;
  struct arg_str *time;
  struct arg_end *end;
} set_clock_args;

static int cmd_set_clock(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&set_clock_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, set_clock_args.end, argv[0]);
    return 1;
  }
  int year, mon, day, hour, min, sec;
  if (sscanf(set_clock_args.date->sval[0], "%d-%d-%d", &year, &mon, &day) !=
          3 ||
      sscanf(set_clock_args.time->sval[0], "%d:%d:%d", &hour, &min, &sec) !=
          3) {
    printf("Error: format must be YYYY-MM-DD HH:MM:SS\n");
    return 1;
  }
  struct tm tm = {.tm_year = year - 1900,
                  .tm_mon = mon - 1,
                  .tm_mday = day,
                  .tm_hour = hour,
                  .tm_min = min,
                  .tm_sec = sec};
  time_t t = mktime(&tm);
  struct timeval tv = {.tv_sec = t};
  settimeofday(&tv, NULL);
  printf("System clock updated manually.\n");
  return 0;
}
/* --- tool_exec command --- */
static struct {
  struct arg_str *name;
  struct arg_str *input;
  struct arg_end *end;
} tool_exec_args;

static int cmd_tool_exec(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&tool_exec_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, tool_exec_args.end, argv[0]);
    return 1;
  }

  char *output = malloc(8192);
  if (!output) {
    printf("Out of memory.\n");
    return 1;
  }

  tool_registry_execute(tool_exec_args.name->sval[0],
                        tool_exec_args.input->sval[0], output, 8192);
  printf("Tool output:\n%s\n", output);

  free(output);
  return 0;
}

esp_err_t serial_cli_init(void) {
  esp_console_repl_t *repl = NULL;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "mimi> ";
  repl_config.max_cmdline_length = 256;

  /* USB Serial JTAG */
  esp_console_dev_usb_serial_jtag_config_t hw_config =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

  ESP_ERROR_CHECK(
      esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

  /* Register commands */
  esp_console_register_help_command();

  /* wifi_set */
  wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
  wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
  wifi_set_args.end = arg_end(2);
  esp_console_cmd_t wifi_set_cmd = {
      .command = "wifi_set",
      .help = "Set WiFi SSID and password",
      .func = &cmd_wifi_set,
      .argtable = &wifi_set_args,
  };
  esp_console_cmd_register(&wifi_set_cmd);

  /* wifi_status */
  esp_console_cmd_t wifi_status_cmd = {
      .command = "wifi_status",
      .help = "Show WiFi connection status",
      .func = &cmd_wifi_status,
  };
  esp_console_cmd_register(&wifi_status_cmd);

  /* wifi_reset */
  esp_console_cmd_t wifi_reset_cmd = {
      .command = "wifi_reset",
      .help = "Reset WiFi creds (NVS + disable secret fallback)",
      .func = &cmd_wifi_reset,
  };
  esp_console_cmd_register(&wifi_reset_cmd);

  /* wifi_portal */
  wifi_portal_args.timeout_sec =
      arg_int0(NULL, NULL, "[timeout_sec]", "Provisioning timeout in seconds");
  wifi_portal_args.end = arg_end(1);
  esp_console_cmd_t wifi_portal_cmd = {
      .command = "wifi_portal",
      .help = "Start SoftAP WiFi provisioning portal",
      .func = &cmd_wifi_portal,
      .argtable = &wifi_portal_args,
  };
  esp_console_cmd_register(&wifi_portal_cmd);

  /* set_tg_token */
  tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
  tg_token_args.end = arg_end(1);
  esp_console_cmd_t tg_token_cmd = {
      .command = "set_tg_token",
      .help = "Set Telegram bot token",
      .func = &cmd_set_tg_token,
      .argtable = &tg_token_args,
  };
  esp_console_cmd_register(&tg_token_cmd);

  /* set_api_key */
  api_key_args.key = arg_str1(NULL, NULL, "<key>", "Anthropic API key");
  api_key_args.end = arg_end(1);
  esp_console_cmd_t api_key_cmd = {
      .command = "set_api_key",
      .help = "Set Claude API key",
      .func = &cmd_set_api_key,
      .argtable = &api_key_args,
  };
  esp_console_cmd_register(&api_key_cmd);

  /* set_model */
  model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
  model_args.end = arg_end(1);
  esp_console_cmd_t model_cmd = {
      .command = "set_model",
      .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
      .func = &cmd_set_model,
      .argtable = &model_args,
  };
  esp_console_cmd_register(&model_cmd);

  /* set_provider */
  provider_args.provider =
      arg_str1(NULL, NULL, "<provider>", "anthropic | openai");
  provider_args.end = arg_end(1);
  esp_console_cmd_t provider_cmd = {
      .command = "set_provider",
      .help = "Set LLM provider",
      .func = &cmd_set_provider,
      .argtable = &provider_args,
  };
  esp_console_cmd_register(&provider_cmd);

  /* set_base_url */
  base_url_args.url = arg_str1(NULL, NULL, "<url>", "API Base URL");
  base_url_args.end = arg_end(1);
  esp_console_cmd_t base_url_cmd = {
      .command = "set_base_url",
      .help = "Set API Base URL",
      .func = &cmd_set_base_url,
      .argtable = &base_url_args,
  };
  esp_console_cmd_register(&base_url_cmd);

  /* pf_use */
  pf_use_args.name = arg_str1(NULL, NULL, "<name>", "Profile name (short)");
  pf_use_args.end = arg_end(1);
  esp_console_cmd_t pf_use_cmd = {
      .command = "pf_use",
      .help = "Switch to LLM profile",
      .func = &cmd_pf_use,
      .argtable = &pf_use_args,
  };
  esp_console_cmd_register(&pf_use_cmd);

  /* pf_ls */
  esp_console_cmd_t pf_ls_cmd = {
      .command = "pf_ls",
      .help = "List LLM profiles",
      .func = &cmd_pf_ls,
  };
  esp_console_cmd_register(&pf_ls_cmd);

  /* pf_del */
  pf_del_args.name = arg_str1(NULL, NULL, "<name>", "Profile to delete");
  pf_del_args.end = arg_end(1);
  esp_console_cmd_t pf_del_cmd = {
      .command = "pf_del",
      .help = "Delete LLM profile",
      .func = &cmd_pf_del,
      .argtable = &pf_del_args,
  };
  esp_console_cmd_register(&pf_del_cmd);

  /* set_timezone */
  timezone_args.tz =
      arg_str1(NULL, NULL, "<tz>", "POSIX TZ string (e.g. KST-9)");
  timezone_args.end = arg_end(1);
  esp_console_cmd_t timezone_cmd = {
      .command = "set_timezone",
      .help = "Set system timezone",
      .func = &cmd_set_timezone,
      .argtable = &timezone_args,
  };
  esp_console_cmd_register(&timezone_cmd);

  /* memory_read */
  esp_console_cmd_t mem_read_cmd = {
      .command = "memory_read",
      .help = "Read MEMORY.md",
      .func = &cmd_memory_read,
  };
  esp_console_cmd_register(&mem_read_cmd);

  /* memory_write */
  memory_write_args.content =
      arg_str1(NULL, NULL, "<content>", "Content to write");
  memory_write_args.end = arg_end(1);
  esp_console_cmd_t mem_write_cmd = {
      .command = "memory_write",
      .help = "Write to MEMORY.md",
      .func = &cmd_memory_write,
      .argtable = &memory_write_args,
  };
  esp_console_cmd_register(&mem_write_cmd);

  /* session_list */
  esp_console_cmd_t sess_list_cmd = {
      .command = "session_list",
      .help = "List all sessions",
      .func = &cmd_session_list,
  };
  esp_console_cmd_register(&sess_list_cmd);

  /* session_clear */
  session_clear_args.chat_id =
      arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
  session_clear_args.end = arg_end(1);
  esp_console_cmd_t sess_clear_cmd = {
      .command = "session_clear",
      .help = "Clear a session",
      .func = &cmd_session_clear,
      .argtable = &session_clear_args,
  };
  esp_console_cmd_register(&sess_clear_cmd);

  /* heap_info */
  esp_console_cmd_t heap_cmd = {
      .command = "heap_info",
      .help = "Show heap memory usage",
      .func = &cmd_heap_info,
  };
  esp_console_cmd_register(&heap_cmd);

  /* set_search_key */
  search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
  search_key_args.end = arg_end(1);
  esp_console_cmd_t search_key_cmd = {
      .command = "set_search_key",
      .help = "Set Brave Search API key for web_search tool",
      .func = &cmd_set_search_key,
      .argtable = &search_key_args,
  };
  esp_console_cmd_register(&search_key_cmd);

  /* set_proxy */
  proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
  proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
  proxy_args.end = arg_end(2);
  esp_console_cmd_t proxy_cmd = {
      .command = "set_proxy",
      .help = "Set HTTP proxy (e.g. set_proxy 192.168.1.83 7897)",
      .func = &cmd_set_proxy,
      .argtable = &proxy_args,
  };
  esp_console_cmd_register(&proxy_cmd);

  /* clear_proxy */
  esp_console_cmd_t clear_proxy_cmd = {
      .command = "clear_proxy",
      .help = "Remove proxy configuration",
      .func = &cmd_clear_proxy,
  };
  esp_console_cmd_register(&clear_proxy_cmd);

  /* config_show */
  esp_console_cmd_t config_show_cmd = {
      .command = "config_show",
      .help = "Show current configuration (build-time + NVS)",
      .func = &cmd_config_show,
  };
  esp_console_cmd_register(&config_show_cmd);

  /* config_reset */
  esp_console_cmd_t config_reset_cmd = {
      .command = "config_reset",
      .help = "Clear all NVS overrides, revert to build-time defaults",
      .func = &cmd_config_reset,
  };
  esp_console_cmd_register(&config_reset_cmd);

  /* tg_auth_add */
  tg_auth_add_args.chat_id =
      arg_str1(NULL, NULL, "<chat_id>", "Telegram Chat ID");
  tg_auth_add_args.end = arg_end(1);
  esp_console_cmd_t tg_auth_add_cmd = {
      .command = "tg_auth_add",
      .help = "Authorize a Telegram Chat ID",
      .func = &cmd_tg_auth_add,
      .argtable = &tg_auth_add_args,
  };
  esp_console_cmd_register(&tg_auth_add_cmd);

  /* tg_auth_remove */
  tg_auth_remove_args.chat_id =
      arg_str1(NULL, NULL, "<chat_id>", "Telegram Chat ID");
  tg_auth_remove_args.end = arg_end(1);
  esp_console_cmd_t tg_auth_remove_cmd = {
      .command = "tg_auth_remove",
      .help = "Deauthorize a Telegram Chat ID",
      .func = &cmd_tg_auth_remove,
      .argtable = &tg_auth_remove_args,
  };
  esp_console_cmd_register(&tg_auth_remove_cmd);

  /* tg_auth_list */
  esp_console_cmd_t tg_auth_list_cmd = {
      .command = "tg_auth_list",
      .help = "List authorized Telegram chats",
      .func = &cmd_tg_auth_list,
  };
  esp_console_cmd_register(&tg_auth_list_cmd);

  /* tool_exec */
  tool_exec_args.name = arg_str1(NULL, NULL, "<name>", "Tool name");
  tool_exec_args.input = arg_str1(NULL, NULL, "<input>", "JSON input");
  tool_exec_args.end = arg_end(2);
  esp_console_cmd_t tool_exec_cmd = {
      .command = "tool_exec",
      .help = "Execute a tool manually",
      .func = &cmd_tool_exec,
      .argtable = &tool_exec_args,
  };
  esp_console_cmd_register(&tool_exec_cmd);

  /* restart */
  esp_console_cmd_t restart_cmd = {
      .command = "restart",
      .help = "Restart the device",
      .func = &cmd_restart,
  };
  esp_console_cmd_register(&restart_cmd);

  /* Start REPL */
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
  ESP_LOGI(TAG, "Serial CLI started");

  /* clear_history */
  esp_console_cmd_t clear_hist_cmd = {
      .command = "clear_history",
      .help = "Delete all session history files",
      .func = &cmd_clear_history,
  };
  esp_console_cmd_register(&clear_hist_cmd);

  /* set_clock */
  set_clock_args.date = arg_str1(NULL, NULL, "<YYYY-MM-DD>", "Date");
  set_clock_args.time = arg_str1(NULL, NULL, "<HH:MM:SS>", "Time");
  set_clock_args.end = arg_end(2);
  esp_console_cmd_t set_clock_cmd = {
      .command = "set_clock",
      .help = "Set system clock manually",
      .func = &cmd_set_clock,
      .argtable = &set_clock_args,
  };
  esp_console_cmd_register(&set_clock_cmd);

  /* rm */
  rm_args.path = arg_str1(NULL, NULL, "<path>", "File path to delete");
  rm_args.end = arg_end(1);
  esp_console_cmd_t rm_cmd = {
      .command = "rm",
      .help = "Delete a file from SPIFFS",
      .func = &cmd_rm,
      .argtable = &rm_args,
  };
  esp_console_cmd_register(&rm_cmd);

  /* ls */
  ls_args.path = arg_str0(NULL, NULL, "[path]", "Directory path to list");
  ls_args.end = arg_end(1);
  esp_console_cmd_t ls_cmd = {
      .command = "ls",
      .help = "List files in SPIFFS",
      .func = &cmd_ls,
      .argtable = &ls_args,
  };
  esp_console_cmd_register(&ls_cmd);

  /* cat */
  cat_args.path = arg_str1(NULL, NULL, "<path>", "File path to read");
  cat_args.end = arg_end(1);
  esp_console_cmd_t cat_cmd = {
      .command = "cat",
      .help = "Read a file from SPIFFS with paging",
      .func = &cmd_cat,
      .argtable = &cat_args,
  };
  esp_console_cmd_register(&cat_cmd);

  /* set_stt_key */
  stt_key_args.key = arg_str1(NULL, NULL, "<key>", "STT API key");
  stt_key_args.end = arg_end(1);
  esp_console_cmd_t stt_key_cmd = {
      .command = "set_stt_key",
      .help = "Set STT API key",
      .func = &cmd_set_stt_key,
      .argtable = &stt_key_args,
  };
  esp_console_cmd_register(&stt_key_cmd);

  /* set_stt_model */
  stt_model_args.model = arg_str1(NULL, NULL, "<model>",
                                  "STT model for transcriptions");
  stt_model_args.end = arg_end(1);
  esp_console_cmd_t stt_model_cmd = {
      .command = "set_stt_model",
      .help = "Set STT model (default: " MIMI_STT_DEFAULT_MODEL ")",
      .func = &cmd_set_stt_model,
      .argtable = &stt_model_args,
  };
  esp_console_cmd_register(&stt_model_cmd);

  /* set_stt_base_url */
  stt_base_url_args.url =
      arg_str1(NULL, NULL, "<url>", "STT API base URL or endpoint");
  stt_base_url_args.end = arg_end(1);
  esp_console_cmd_t stt_base_url_cmd = {
      .command = "set_stt_base_url",
      .help = "Set STT base URL",
      .func = &cmd_set_stt_base_url,
      .argtable = &stt_base_url_args,
  };
  esp_console_cmd_register(&stt_base_url_cmd);

  /* set_stt_provider */
  stt_provider_args.provider =
      arg_str1(NULL, NULL, "<provider>", "groq");
  stt_provider_args.end = arg_end(1);
  esp_console_cmd_t stt_provider_cmd = {
      .command = "set_stt_provider",
      .help = "Set STT provider",
      .func = &cmd_set_stt_provider,
      .argtable = &stt_provider_args,
  };
  esp_console_cmd_register(&stt_provider_cmd);

  media_limit_args.photo_kb = arg_int1(NULL, NULL, "<photo_kb>",
                                       "Max photo size in KB");
  media_limit_args.voice_kb = arg_int1(NULL, NULL, "<voice_kb>",
                                       "Max voice size in KB");
  media_limit_args.voice_secs =
      arg_int1(NULL, NULL, "<voice_secs>", "Max voice duration in seconds");
  media_limit_args.end = arg_end(3);
  esp_console_cmd_t media_limits_cmd = {
      .command = "set_media_limits",
      .help = "Set media limits: set_media_limits <photo_kb> <voice_kb> <voice_secs>",
      .func = &cmd_set_media_limits,
      .argtable = &media_limit_args,
  };
  esp_console_cmd_register(&media_limits_cmd);

  return ESP_OK;
}
