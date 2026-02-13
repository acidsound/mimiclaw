#include "tools/tool_discovery.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "mdns.h"
#include "mimi_config.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>

static const char *TAG = "tool_discovery";
static bool s_scan_active = false;
static int s_scan_progress = 0;
static char s_last_job_id[16] = "0";

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_NAMEREQD
#define NI_NAMEREQD 8
#endif
#define WOL_UNKNOWN_VALUE "unknown"

/* Shared helpers */

static bool is_unknown_value(const char *value) {
  return (!value || *value == '\0' ||
          strcasecmp(value, WOL_UNKNOWN_VALUE) == 0);
}

static const char *get_known_string(const cJSON *obj, const char *key) {
  const cJSON *item = cJSON_GetObjectItem(obj, key);
  if (!item || !cJSON_IsString(item) || is_unknown_value(item->valuestring)) {
    return NULL;
  }
  return item->valuestring;
}

static void set_string_field(cJSON *obj, const char *key, const char *value) {
  const char *safe = is_unknown_value(value) ? WOL_UNKNOWN_VALUE : value;
  cJSON *node = cJSON_CreateString(safe);
  if (!node) {
    return;
  }

  if (cJSON_GetObjectItem(obj, key)) {
    cJSON_ReplaceItemInObject(obj, key, node);
  } else {
    cJSON_AddItemToObject(obj, key, node);
  }
}

static void set_number_field(cJSON *obj, const char *key, double value) {
  cJSON *node = cJSON_CreateNumber(value);
  if (!node) {
    return;
  }

  if (cJSON_GetObjectItem(obj, key)) {
    cJSON_ReplaceItemInObject(obj, key, node);
  } else {
    cJSON_AddItemToObject(obj, key, node);
  }
}

static cJSON *load_wol_devices_json(void) {
  FILE *f = fopen(MIMI_WOL_DEVICES_FILE, "r");
  if (!f) {
    return cJSON_CreateArray();
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *data = NULL;
  cJSON *root = NULL;
  if (size > 0) {
    data = malloc(size + 1);
    if (data) {
      fread(data, 1, size, f);
      data[size] = '\0';
      root = cJSON_Parse(data);
      free(data);
    }
  }
  fclose(f);

  if (!root || !cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return cJSON_CreateArray();
  }
  return root;
}

static bool has_known_number(const cJSON *obj, const char *key) {
  const cJSON *item = cJSON_GetObjectItem(obj, key);
  return item && cJSON_IsNumber(item);
}

static void persist_wol_devices(cJSON *root) {
  if (!root || !cJSON_IsArray(root)) {
    return;
  }

  char *out = cJSON_PrintUnformatted(root);
  if (!out) {
    return;
  }

  FILE *f = fopen(MIMI_WOL_DEVICES_FILE, "w");
  if (f) {
    fputs(out, f);
    fclose(f);
  }
  free(out);
}

static bool device_matches_selector(const cJSON *dev, const char *selector) {
  if (!selector || !*selector) {
    return false;
  }

  const char *fields[] = {"label", "name", "hostname", "ip", "mac", NULL};
  for (int i = 0; fields[i]; i++) {
    const char *value = get_known_string(dev, fields[i]);
    if (value && strcasecmp(value, selector) == 0) {
      return true;
    }
  }
  return false;
}

static cJSON *collect_matching_devices(const char *selector) {
  cJSON *devices = load_wol_devices_json();
  cJSON *matches = cJSON_CreateArray();
  if (!matches) {
    cJSON_Delete(devices);
    return NULL;
  }

  if (!selector || !*selector || !cJSON_IsArray(devices)) {
    cJSON_Delete(devices);
    return matches;
  }

  cJSON *item;
  cJSON_ArrayForEach(item, devices) {
    if (!cJSON_IsObject(item)) {
      continue;
    }
    if (!device_matches_selector(item, selector)) {
      continue;
    }

    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
      continue;
    }
    set_string_field(entry, "label", get_known_string(item, "label"));
    set_string_field(entry, "name", get_known_string(item, "name"));
    set_string_field(entry, "hostname", get_known_string(item, "hostname"));
    set_string_field(entry, "ip", get_known_string(item, "ip"));
    set_string_field(entry, "mac", get_known_string(item, "mac"));
    cJSON_AddItemToArray(matches, entry);
  }
  cJSON_Delete(devices);
  return matches;
}

static void ensure_device_defaults(cJSON *dev, const char *ip, const char *mac,
                                 const char *hostname) {
  const char *resolved_label =
      get_known_string(dev, "label"); // May already be manually edited.
  if (!resolved_label && hostname && !is_unknown_value(hostname)) {
    resolved_label = hostname;
  }
  if (!resolved_label && ip && *ip) {
    resolved_label = ip;
  }
  if (!resolved_label) {
    resolved_label = WOL_UNKNOWN_VALUE;
  }

  set_string_field(dev, "label", resolved_label);
  set_string_field(dev, "name", resolved_label);
  set_string_field(dev, "ip", ip);
  if (mac) {
    set_string_field(dev, "mac", mac);
  } else {
    set_string_field(dev, "mac", WOL_UNKNOWN_VALUE);
  }
  if (hostname) {
    set_string_field(dev, "hostname", hostname);
  } else {
    set_string_field(dev, "hostname", WOL_UNKNOWN_VALUE);
  }

  time_t now = time(NULL);
  if (!has_known_number(dev, "discovered_at")) {
    set_number_field(dev, "discovered_at", (double)now);
  }
  set_number_field(dev, "updated_at", (double)now);
}

static bool update_or_create_wol_device(cJSON *devices, const char *label,
                                       const char *mac, const char *ip,
                                       const char *hostname) {
  bool found = false;
  if (!devices || !cJSON_IsArray(devices) || !mac) {
    return false;
  }

  cJSON *item = NULL;
  cJSON_ArrayForEach(item, devices) {
    if (!cJSON_IsObject(item)) {
      continue;
    }

    const char *j_mac = get_known_string(item, "mac");
    if (!j_mac || strcasecmp(j_mac, mac) != 0) {
      continue;
    }

    if (ip) {
      set_string_field(item, "ip", ip);
    }
    if (hostname) {
      set_string_field(item, "hostname", hostname);
    }
    if (label) {
      set_string_field(item, "label", label);
      set_string_field(item, "name", label);
    }
    ensure_device_defaults(item, ip ? ip : get_known_string(item, "ip"), mac,
                          hostname ? hostname : get_known_string(item, "hostname"));
    found = true;
    break;
  }

  if (!found) {
    cJSON *dev = cJSON_CreateObject();
    if (!dev) {
      return false;
    }
    ensure_device_defaults(dev, ip, mac, hostname);
    if (label) {
      set_string_field(dev, "label", label);
      set_string_field(dev, "name", label);
    }
    cJSON_AddItemToArray(devices, dev);
    found = true;
  }

  return found;
}

esp_err_t tool_wol_register_execute(const char *input_json, char *output,
                                   size_t output_size) {
  cJSON *root = cJSON_Parse(input_json);
  if (!root) {
    snprintf(output, output_size, "Error: invalid JSON");
    return ESP_ERR_INVALID_ARG;
  }

  const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mac"));
  const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
  const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
  const char *hostname = cJSON_GetStringValue(cJSON_GetObjectItem(root, "hostname"));

  if (!label && name) {
    label = name;
  }
  if (!mac) {
    cJSON_Delete(root);
    snprintf(output, output_size, "Error: missing required field 'mac'");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t mac_addr[6];
  if (sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac_addr[0], &mac_addr[1],
             &mac_addr[2], &mac_addr[3], &mac_addr[4], &mac_addr[5]) != 6) {
    cJSON_Delete(root);
    snprintf(output, output_size,
             "Error: invalid MAC format (use AA:BB:CC:DD:EE:FF)");
    return ESP_ERR_INVALID_ARG;
  }
  (void)mac_addr; /* parsed for validation only */

  cJSON *devices = load_wol_devices_json();
  if (!devices || !cJSON_IsArray(devices)) {
    cJSON_Delete(root);
    cJSON_Delete(devices);
    snprintf(output, output_size, "Error: failed to load WOL registry");
    return ESP_FAIL;
  }

  if (!update_or_create_wol_device(devices, label, mac, ip, hostname)) {
    cJSON_Delete(root);
    cJSON_Delete(devices);
    snprintf(output, output_size, "Error: failed to update WOL registry");
    return ESP_FAIL;
  }

  persist_wol_devices(devices);
  cJSON_Delete(devices);
  cJSON_Delete(root);

  if (label) {
    snprintf(output, output_size, "OK: registered WOL device %s (%s)", label, mac);
  } else if (ip) {
    snprintf(output, output_size, "OK: updated WOL device %s (%s)", ip, mac);
  } else {
    snprintf(output, output_size, "OK: updated WOL device %s", mac);
  }

  return ESP_OK;
}

/* ── Wake on LAN ───────────────────────────────────────────── */

esp_err_t tool_wol_execute(const char *input_json, char *output,
                           size_t output_size) {
  ESP_LOGI(TAG, "Executing wol_send");
  cJSON *root = cJSON_Parse(input_json);
  if (!root) {
    snprintf(output, output_size, "Error: invalid JSON");
    return ESP_ERR_INVALID_ARG;
  }

  const char *mac_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mac"));
  char resolved_mac_buf[20] = {0};
  const char *selector = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device"));
  if (!selector) {
    selector = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
  }
  if (!selector) {
    selector = cJSON_GetStringValue(cJSON_GetObjectItem(root, "hostname"));
  }
  if (!selector) {
    selector = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
  }
  if (!selector) {
    selector = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  }

  if (!mac_str && selector) {
    cJSON *matches = collect_matching_devices(selector);
    if (!matches) {
      snprintf(output, output_size, "Error: WOL registry unavailable");
      cJSON_Delete(root);
      return ESP_FAIL;
    }

    int match_count = cJSON_GetArraySize(matches);
    if (match_count == 0) {
      cJSON_Delete(matches);
      cJSON_Delete(root);
      snprintf(output, output_size,
               "Error: no device found. Run `list_devices` and use a device "
               "label/hostname/ip exactly.");
      return ESP_ERR_INVALID_ARG;
    }

    if (match_count > 1) {
      char *list_json = cJSON_PrintUnformatted(matches);
      if (list_json) {
        snprintf(output, output_size,
                 "Error: multiple devices matched '%s'. Specify one of: %s",
                 selector, list_json);
        free(list_json);
      } else {
        snprintf(output, output_size,
                 "Error: multiple devices matched '%s'. Specify a unique "
                 "label/hostname/ip.",
                 selector);
      }
      cJSON_Delete(matches);
      cJSON_Delete(root);
      return ESP_ERR_INVALID_ARG;
    }

    cJSON *single = cJSON_GetArrayItem(matches, 0);
    const char *resolved_mac = cJSON_GetStringValue(
        cJSON_GetObjectItem(single, "mac"));
    if (is_unknown_value(resolved_mac)) {
      cJSON_Delete(matches);
      cJSON_Delete(root);
      snprintf(output, output_size,
               "Error: selected device has no known MAC yet. Run a scan and "
               "try again.");
      return ESP_ERR_INVALID_ARG;
    }
    snprintf(resolved_mac_buf, sizeof(resolved_mac_buf), "%s", resolved_mac);
    mac_str = resolved_mac_buf;
    cJSON_Delete(matches);
  }

  if (!mac_str) {
    cJSON_Delete(root);
    snprintf(output, output_size, "Error: missing mac or device/label/hostname/ip");
    return ESP_ERR_INVALID_ARG;
  }

  if (selector && !cJSON_GetObjectItem(root, "mac")) {
    ESP_LOGI(TAG, "Executing wol_send for selector=%s resolved_mac=%s", selector,
             mac_str);
  }

  uint8_t mac[6];
  if (sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1],
             &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
    snprintf(output, output_size,
             "Error: invalid MAC format (use AA:BB:CC:DD:EE:FF)");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
  }

  /* Build Magic Packet: 6x 0xFF + 16x MAC */
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) {
    memcpy(&packet[6 + i * 6], mac, 6);
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    snprintf(output, output_size, "Error: socket creation failed");
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  int broadcast = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9);
  addr.sin_addr.s_addr = INADDR_BROADCAST;

  int sent = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&addr,
                    sizeof(addr));
  close(sock);

  if (sent < 0) {
    snprintf(output, output_size, "Error: failed to send Magic Packet");
  } else {
    if (selector) {
      snprintf(output, output_size, "OK: Magic Packet sent to %s (%s)", selector,
               mac_str);
    } else {
      snprintf(output, output_size, "OK: Magic Packet sent to %s", mac_str);
    }
  }

  cJSON_Delete(root);
  return ESP_OK;
}

/* ── Device Discovery (Background Scanner) ──────────────────── */

static void save_discovered_device(const char *ip, const char *mac,
                                   const char *hostname) {
  cJSON *root = load_wol_devices_json();
  if (!root) {
    root = cJSON_CreateArray();
  }

  bool found = false;
  cJSON *item;
  cJSON_ArrayForEach(item, root) {
    if (!cJSON_IsObject(item)) {
      continue;
    }

    const char *j_ip = get_known_string(item, "ip");
    if (j_ip && strcmp(j_ip, ip) == 0) {
      found = true;
      ensure_device_defaults(item, ip, mac, hostname);
      break;
    }
  }

  if (!found) {
    cJSON *dev = cJSON_CreateObject();
    ensure_device_defaults(dev, ip, mac, hostname);
    cJSON_AddItemToArray(root, dev);
  }

  persist_wol_devices(root);
  cJSON_Delete(root);
}

esp_err_t tool_list_devices_execute(const char *input_json, char *output,
                                    size_t output_size) {
  cJSON *root = load_wol_devices_json();
  if (!root || cJSON_GetArraySize(root) == 0) {
    snprintf(output, output_size, "[]");
    cJSON_Delete(root);
    return ESP_OK;
  }

  char *out = cJSON_PrintUnformatted(root);
  if (out) {
    snprintf(output, output_size, "%s", out);
    free(out);
  } else {
    snprintf(output, output_size, "[]");
  }
  cJSON_Delete(root);

  return ESP_OK;
}

/* Try to resolve MAC from ARP cache */
static void resolve_mac(uint32_t ip, char *mac_buf, size_t mac_len) {
  ip4_addr_t ip_addr;
  ip_addr.addr = ip;
  struct eth_addr *eth_ret;
  const ip4_addr_t *ip_ret;

  /* etharp_find_addr is an lwIP function, safe to call if thread-safe or locked
   * BUT ESP-IDF lwIP is thread-safe. */
  if (etharp_find_addr(NULL, &ip_addr, &eth_ret, &ip_ret) != -1) {
    snprintf(mac_buf, mac_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             eth_ret->addr[0], eth_ret->addr[1], eth_ret->addr[2],
             eth_ret->addr[3], eth_ret->addr[4], eth_ret->addr[5]);
  } else {
    // ARP might not be populated instantly after ping
    strncpy(mac_buf, "unknown", mac_len);
  }
}

/* Try to resolve Hostname via standard DNS/mDNS stub */
static void resolve_hostname(uint32_t ip, char *name_buf, size_t name_len) {
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = ip;
  sa.sin_len = sizeof(sa);

  char host[NI_MAXHOST] = {0};
  if (getnameinfo((struct sockaddr *)&sa, sizeof(sa), host, sizeof(host), NULL,
                  0, NI_NAMEREQD) == 0) {
    /* Ensure null termination */
    host[sizeof(host) - 1] = '\0';
    strncpy(name_buf, host, name_len);
    name_buf[name_len - 1] = '\0';
  } else {
    strncpy(name_buf, "unknown", name_len);
    name_buf[name_len - 1] = '\0';
  }
}

static void ping_probe(uint32_t target_ip) {
  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = target_ip; // Already network byte order? No.
  // wait, target_ip passed from loop is host order or network?
  // In loop: base + i using ntohl(ip) math -> HOST order.
  // So convert to network:
  dest_addr.sin_addr.s_addr = htonl(target_ip);
  dest_addr.sin_family = AF_INET;

  int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sock < 0)
    return;

  struct timeval tv = {.tv_sec = 0, .tv_usec = 200000}; // 200ms
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct icmp_echo_hdr *echo = malloc(sizeof(struct icmp_echo_hdr) + 32);
  memset(echo, 0, sizeof(struct icmp_echo_hdr) + 32);
  echo->type = ICMP_ECHO;
  echo->id = 0xA5A5;
  echo->seqno = 1;
  echo->chksum = inet_chksum(echo, sizeof(struct icmp_echo_hdr) + 32);

  sendto(sock, echo, sizeof(struct icmp_echo_hdr) + 32, 0,
         (struct sockaddr *)&dest_addr, sizeof(dest_addr));

  uint8_t recv_buf[128];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);
  if (recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&from,
               &from_len) > 0) {
    char ip_str[16];
    inet_ntoa_r(from.sin_addr, ip_str, sizeof(ip_str));

    /* Resolve Extra Info */
    char mac_str[20] = {0};
    char host_str[64] = {0};

    /* Use from.sin_addr.s_addr (Network Order) for ARP/mDNS */
    resolve_mac(from.sin_addr.s_addr, mac_str, sizeof(mac_str));

    /* For hostname, getnameinfo expects same layout as sent, but let's check */
    resolve_hostname(from.sin_addr.s_addr, host_str, sizeof(host_str));

    ESP_LOGI(TAG, "Discovered: %s -> MAC: %s, Host: %s", ip_str, mac_str,
             host_str);
    save_discovered_device(ip_str, mac_str, host_str);
  }

  free(echo);
  close(sock);
}

static void discovery_task(void *arg) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_scan_active = true;
    s_scan_progress = 0;

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      uint32_t ip = ntohl(ip_info.ip.addr);
      uint32_t mask = ntohl(ip_info.netmask.addr);
      uint32_t base = ip & mask;
      uint32_t range = ~mask;

      for (uint32_t i = 1; i < range && i < 255; i++) {
        if ((base + i) == ip)
          continue;
        ping_probe(base + i);
        s_scan_progress = (i * 100) / (range < 255 ? range : 255);
        vTaskDelay(pdMS_TO_TICKS(20)); // Slower scan for better ARP Reliability
      }
    }
    s_scan_active = false;
    s_scan_progress = 100;
  }
}

static TaskHandle_t s_discovery_task_handle = NULL;

esp_err_t tool_wol_scan_start_execute(const char *input_json, char *output,
                                      size_t output_size) {
  if (s_scan_active) {
    snprintf(output, output_size, "{\"error\":\"already in progress\"}");
    return ESP_ERR_INVALID_STATE;
  }

  snprintf(s_last_job_id, sizeof(s_last_job_id), "%ld",
           (long)xTaskGetTickCount());
  xTaskNotifyGive(s_discovery_task_handle);

  snprintf(output, output_size, "{\"job_id\":\"%s\"}", s_last_job_id);
  return ESP_OK;
}

esp_err_t tool_wol_scan_result_execute(const char *input_json, char *output,
                                       size_t output_size) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "job_id", s_last_job_id);
  cJSON_AddStringToObject(root, "status",
                          s_scan_active ? "running" : "completed");
  cJSON_AddNumberToObject(root, "progress", s_scan_progress);
  cJSON *devices = load_wol_devices_json();
  if (cJSON_IsArray(devices)) {
    cJSON_AddItemToObject(root, "devices", devices);
  } else {
    cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
  }

  char *json = cJSON_PrintUnformatted(root);
  if (json) {
    snprintf(output, output_size, "%s", json);
    free(json);
  }
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t tool_discovery_init(void) {
  /* Initialize mDNS */
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set("mimiclaw"));
  ESP_ERROR_CHECK(mdns_instance_name_set("MimiClaw AI Agent"));

  xTaskCreate(discovery_task, "discovery_task", 4096, NULL, 4,
              &s_discovery_task_handle);
  return ESP_OK;
}
