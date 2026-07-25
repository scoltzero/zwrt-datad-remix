/*
 * Persistent per-SIM traffic accounting and fixed-offset timezone settings.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZWRT_USAGE_H
#define ZWRT_USAGE_H

#include <stddef.h>
#include <time.h>

void usage_init(void);
void usage_set_identity(const char *iccid, const char *operator_name, int slot);
void usage_sample(const char *traffic_json, time_t now);
void usage_flush(int force);

void usage_build_timezone_json(char *out, size_t outlen);
void usage_build_traffic_json(char *out, size_t outlen);

int usage_set_timezone_from_path(const char *path, char *out, size_t outlen);
int usage_set_plan_from_path(const char *path, char *out, size_t outlen);

int usage_effective_offset_minutes(void);
void usage_localtime(time_t when, struct tm *out);

#endif
