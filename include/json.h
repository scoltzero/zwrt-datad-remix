/*
 * json.h - tiny read-only JSON value extractor for flat/shallow objects.
 *
 * Not a full parser: it locates "key" and returns the following scalar,
 * string (unquoted), or balanced {..}/[..] substring. Sufficient for the
 * flat objects ubus returns.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_JSON_H
#define U60_JSON_H

#include <stddef.h>
#include <stdint.h>

/* Copy the value of `key` into out (NUL-terminated). Returns 1 if found. */
int  json_get(const char *json, const char *key, char *out, size_t outlen);

/* Integer value of `key`, or `def` if missing/unparseable. */
long json_get_int(const char *json, const char *key, long def);

/* Unsigned 64-bit integer value. Returns 1 on success. */
int json_get_u64(const char *json, const char *key, uint64_t *out);

#endif /* U60_JSON_H */
