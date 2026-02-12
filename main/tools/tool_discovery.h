#ifndef TOOL_DISCOVERY_H
#define TOOL_DISCOVERY_H

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute the wake_on_lan tool.
 * Sends a Magic Packet to the specified MAC address.
 */
esp_err_t tool_wol_execute(const char *input_json, char *output,
                           size_t output_size);

/**
 * Execute the list_devices tool.
 * Returns a list of known devices found via background scanning.
 */
esp_err_t tool_list_devices_execute(const char *input_json, char *output,
                                    size_t output_size);

/**
 * Initialize the discovery module and start background scanning.
 */
esp_err_t tool_discovery_init(void);

#endif // TOOL_DISCOVERY_H
