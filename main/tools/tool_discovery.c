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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  if (!mac_str) {
    snprintf(output, output_size, "Error: missing mac parameter");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
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
    snprintf(output, output_size, "OK: Magic Packet sent to %s", mac_str);
  }

  cJSON_Delete(root);
  return ESP_OK;
}

/* ── Device Discovery (Background Scanner) ──────────────────── */

static void save_discovered_device(const char *ip, const char *mac,
                                   const char *hostname) {
  char *data = NULL;
  FILE *f = fopen(MIMI_WOL_DEVICES_FILE, "r");
  if (f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(size + 1);
    if (data) {
      fread(data, 1, size, f);
      data[size] = '\0';
    }
    fclose(f);
  }

  cJSON *root = data ? cJSON_Parse(data) : cJSON_CreateArray();
  free(data);
  if (!root)
    root = cJSON_CreateArray();

  bool found = false;
  cJSON *item;
  cJSON_ArrayForEach(item, root) {
    cJSON *j_ip = cJSON_GetObjectItem(item, "ip");
    if (j_ip && strcmp(j_ip->valuestring, ip) == 0) {
      if (mac && strlen(mac) > 0 && strcmp(mac, "unknown") != 0)
        cJSON_ReplaceItemInObject(item, "mac", cJSON_CreateString(mac));
      if (hostname && strlen(hostname) > 0 && strcmp(hostname, "unknown") != 0)
        cJSON_ReplaceItemInObject(item, "hostname",
                                  cJSON_CreateString(hostname));
      found = true;
      break;
    }
  }

  if (!found) {
    cJSON *dev = cJSON_CreateObject();
    cJSON_AddStringToObject(dev, "ip", ip);
    cJSON_AddStringToObject(dev, "mac", mac ? mac : "unknown");
    cJSON_AddStringToObject(dev, "hostname", hostname ? hostname : "unknown");
    cJSON_AddItemToArray(root, dev);
  }

  char *out = cJSON_PrintUnformatted(root);
  if (out) {
    f = fopen(MIMI_WOL_DEVICES_FILE, "w");
    if (f) {
      fputs(out, f);
      fclose(f);
    }
    free(out);
  }
  cJSON_Delete(root);
}

esp_err_t tool_list_devices_execute(const char *input_json, char *output,
                                    size_t output_size) {
  FILE *f = fopen(MIMI_WOL_DEVICES_FILE, "r");
  if (!f) {
    snprintf(output, output_size, "[]");
    return ESP_OK;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *data = malloc(size + 1);
  if (data) {
    fread(data, 1, size, f);
    data[size] = '\0';
    snprintf(output, output_size, "%s", data);
    free(data);
  }
  fclose(f);

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

  /* List recently discovered devices */
  FILE *f = fopen(MIMI_WOL_DEVICES_FILE, "r");
  if (f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(size + 1);
    if (data) {
      fread(data, 1, size, f);
      data[size] = '\0';
      cJSON_AddItemToObject(root, "devices", cJSON_Parse(data));
      free(data);
    }
    fclose(f);
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
