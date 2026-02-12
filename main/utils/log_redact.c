#include "utils/log_redact.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void mimi_log_redact(const char *input, char *output, size_t output_size) {
  if (!input || !output || output_size == 0)
    return;

  static const char *sensitive_patterns[] = {
      "bot",      "Bearer ", "Cookie: ", "Set-Cookie: ",
      "api_key=", "token=",  "{{SECRET:"};
  static const size_t pattern_count =
      sizeof(sensitive_patterns) / sizeof(sensitive_patterns[0]);

  size_t in_pos = 0;
  size_t out_pos = 0;
  size_t in_len = strlen(input);

  while (in_pos < in_len && out_pos < output_size - 1) {
    bool pattern_found = false;
    for (size_t i = 0; i < pattern_count; i++) {
      size_t p_len = strlen(sensitive_patterns[i]);
      if (in_pos + p_len <= in_len &&
          strncmp(&input[in_pos], sensitive_patterns[i], p_len) == 0) {
        // Copy the pattern prefix
        for (size_t j = 0; j < p_len && out_pos < output_size - 1; j++) {
          output[out_pos++] = sensitive_patterns[i][j];
        }
        in_pos += p_len;

        // Redact the sensitive part
        const char *redacted = "[REDACTED]";
        size_t r_len = strlen(redacted);
        for (size_t j = 0; j < r_len && out_pos < output_size - 1; j++) {
          output[out_pos++] = redacted[j];
        }

        // Skip characters in input until we hit a delimiter
        while (in_pos < in_len) {
          char c = input[in_pos];
          if (c == '/' || c == ' ' || c == '&' || c == ';' || c == '"' ||
              c == '\n' || c == '\r' ||
              (sensitive_patterns[i][0] == '{' && c == '}')) {
            if (sensitive_patterns[i][0] == '{' && c == '}') {
              // Skip closing brace for secret placeholders if we want to redact
              // the whole thing Actually, plan says redact "resolved secret
              // values". But if we are logging the resolved string,
              // "{{SECRET:KEY}}" is replaced by the value. So we should redact
              // any value that looks like it came from a secret. This helper is
              // for raw strings that MIGHT contain secrets.
            }
            break;
          }
          in_pos++;
        }
        pattern_found = true;
        break;
      }
    }

    if (!pattern_found) {
      output[out_pos++] = input[in_pos++];
    }
  }
  output[out_pos] = '\0';
}
