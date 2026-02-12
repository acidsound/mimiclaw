#ifndef MIMI_LOG_REDACT_H
#define MIMI_LOG_REDACT_H

#include <stddef.h>

/**
 * Redacts sensitive information from a string.
 * Replaces tokens, cookies, and secrets with [REDACTED].
 *
 * @param input The input string to redact.
 * @param output The buffer to store the redacted string.
 * @param output_size The size of the output buffer.
 */
void mimi_log_redact(const char *input, char *output, size_t output_size);

#endif // MIMI_LOG_REDACT_H
