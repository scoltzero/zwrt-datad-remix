/*
 * usage.c - low-write persistent traffic accounting keyed by SIM ICCID.
 *
 * SPDX-License-Identifier: MIT
 */
#include "usage.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define USAGE_VERSION 1
#define USAGE_MAX_SIMS 16
#define USAGE_PERSIST_SEC 300
#define USAGE_JSON_MAX 65536
#define USAGE_DEFAULT_DIR "/data/plugins/zwrt-datad"
#define USAGE_SYSTEM_TIMEZONE_FILE "/etc/config/zwrt_zte_sntp"

struct usage_buf {
    char *data;
    size_t cap;
    size_t len;
};

struct usage_plan {
    char id[24];
    int enabled;
    uint64_t allowance_bytes;
    int reset_day;
};

struct usage_record {
    char id[24];
    char iccid[48];
    char operator_name[64];
    int slot;
    int baseline_valid;
    uint64_t source_rx;
    uint64_t source_tx;
    uint64_t source_session;
    uint64_t lifetime_rx;
    uint64_t lifetime_tx;
    uint64_t day_rx;
    uint64_t day_tx;
    int day_key;
    uint64_t cycle_rx;
    uint64_t cycle_tx;
    int cycle_key;
    int64_t cycle_start;
    int64_t cycle_end;
    int64_t last_seen;
};

static struct usage_record g_records[USAGE_MAX_SIMS];
static int g_record_count;
static struct usage_plan g_plans[USAGE_MAX_SIMS];
static int g_plan_count;

static char g_data_dir[PATH_MAX] = USAGE_DEFAULT_DIR;
static char g_state_path[PATH_MAX];
static char g_plan_path[PATH_MAX];
static char g_timezone_path[PATH_MAX];

static char g_active_id[24];
static char g_active_iccid[48];
static char g_active_operator[64];
static int g_active_slot;
static char g_sample_identity[24];
static int g_identity_initialized;

static int g_offset_minutes = 480;
static int g_dst_minutes;
static int g_system_offset_minutes;
static int g_dirty;
static time_t g_last_persist;

static void ub_append(struct usage_buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room;

    if (!b || b->len >= b->cap) return;
    room = b->cap - b->len;
    va_start(ap, fmt);
    n = vsnprintf(b->data + b->len, room, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= room) {
        b->len = b->cap - 1;
        b->data[b->len] = 0;
    } else {
        b->len += (size_t)n;
    }
}

static void ub_json_string(struct usage_buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    ub_append(b, "\"");
    while (*p) {
        if (*p == '"' || *p == '\\') ub_append(b, "\\%c", *p);
        else if (*p == '\n') ub_append(b, "\\n");
        else if (*p == '\r') ub_append(b, "\\r");
        else if (*p == '\t') ub_append(b, "\\t");
        else if (*p < 0x20) ub_append(b, "\\u%04x", *p);
        else ub_append(b, "%c", *p);
        p++;
    }
    ub_append(b, "\"");
}

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    const unsigned char *p = (const unsigned char *)s;
    while (p && *p) {
        h ^= *p++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void make_sim_id(char *out, size_t outlen, const char *iccid, int slot)
{
    char key[80];
    if (iccid && *iccid) snprintf(key, sizeof key, "iccid:%s", iccid);
    else if (slot > 0) snprintf(key, sizeof key, "slot:%d", slot);
    else {
        out[0] = 0;
        return;
    }
    snprintf(out, outlen, "%016" PRIx64, fnv1a64(key));
}

static int query_value(const char *path, const char *name, char *out, size_t outlen)
{
    const char *q;
    size_t name_len;
    if (!path || !name || !out || outlen == 0) return 0;
    q = strchr(path, '?');
    if (!q) return 0;
    q++;
    name_len = strlen(name);
    while (*q) {
        const char *end = strchr(q, '&');
        size_t len = end ? (size_t)(end - q) : strlen(q);
        if (len > name_len && !strncmp(q, name, name_len) && q[name_len] == '=') {
            const char *v = q + name_len + 1;
            size_t vlen = len - name_len - 1;
            if (vlen >= outlen) vlen = outlen - 1;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return 1;
        }
        if (!end) break;
        q = end + 1;
    }
    return 0;
}

static int query_int(const char *path, const char *name, int *out)
{
    char value[32];
    char *end;
    long n;
    if (!query_value(path, name, value, sizeof value)) return 0;
    errno = 0;
    n = strtol(value, &end, 10);
    if (errno || end == value || *end || n < INT_MIN || n > INT_MAX) return -1;
    *out = (int)n;
    return 1;
}

static int query_u64(const char *path, const char *name, uint64_t *out)
{
    char value[48];
    char *end;
    unsigned long long n;
    if (!query_value(path, name, value, sizeof value)) return 0;
    if (value[0] == '-') return -1;
    errno = 0;
    n = strtoull(value, &end, 10);
    if (errno || end == value || *end) return -1;
    *out = (uint64_t)n;
    return 1;
}

static int valid_sim_id(const char *id)
{
    size_t n;
    if (!id || !(n = strlen(id)) || n >= sizeof g_active_id) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)id[i])) return 0;
    return 1;
}

static int read_file(const char *path, char *out, size_t outlen)
{
    FILE *fp;
    size_t n;
    if (!path || !out || outlen < 2) return 0;
    fp = fopen(path, "r");
    if (!fp) return 0;
    n = fread(out, 1, outlen - 1, fp);
    out[n] = 0;
    fclose(fp);
    return n > 0;
}

static int uci_option_value(const char *text, const char *name, char *out, size_t outlen)
{
    const char *line = text;
    size_t name_len = strlen(name);

    while (line && *line) {
        const char *end = strchr(line, '\n');
        const char *p = line;
        const char *value_end;
        char quote = 0;
        size_t len;

        if (!end) end = line + strlen(line);
        while (p < end && isspace((unsigned char)*p)) p++;
        if ((size_t)(end - p) > 6 && !strncmp(p, "option", 6) &&
            isspace((unsigned char)p[6])) {
            p += 6;
            while (p < end && isspace((unsigned char)*p)) p++;
            if ((size_t)(end - p) >= name_len && !strncmp(p, name, name_len) &&
                (p + name_len == end || isspace((unsigned char)p[name_len]))) {
                p += name_len;
                while (p < end && isspace((unsigned char)*p)) p++;
                if (p < end && (*p == '\'' || *p == '"')) quote = *p++;
                value_end = p;
                if (quote) {
                    while (value_end < end && *value_end != quote) value_end++;
                } else {
                    while (value_end < end && !isspace((unsigned char)*value_end) &&
                           *value_end != '#') value_end++;
                }
                len = (size_t)(value_end - p);
                if (len && len < outlen) {
                    memcpy(out, p, len);
                    out[len] = 0;
                    return 1;
                }
            }
        }
        line = *end ? end + 1 : NULL;
    }
    return 0;
}

static int parse_utc_offset_minutes(const char *value, int *out)
{
    char *end;
    double hours;
    int minutes;

    if (!value || !*value || !out) return 0;
    errno = 0;
    hours = strtod(value, &end);
    while (end && isspace((unsigned char)*end)) end++;
    if (errno || end == value || (end && *end) || hours < -12.0 || hours > 14.0)
        return 0;
    minutes = (int)(hours * 60.0 + (hours < 0 ? -0.5 : 0.5));
    if (minutes < -720 || minutes > 840) return 0;
    *out = minutes;
    return 1;
}

static void load_system_clock_offset(void)
{
    const char *path = getenv("ZWRT_DATAD_SYSTEM_TIMEZONE_FILE");
    char config[4096], value[64];
    int offset;

    g_system_offset_minutes = 0;
    if (!path || !*path) path = USAGE_SYSTEM_TIMEZONE_FILE;
    if (!read_file(path, config, sizeof config)) return;
    if (uci_option_value(config, "time_from_utc", value, sizeof value) &&
        parse_utc_offset_minutes(value, &offset)) {
        g_system_offset_minutes = offset;
        return;
    }
    if (uci_option_value(config, "timezone", value, sizeof value) &&
        parse_utc_offset_minutes(value, &offset))
        g_system_offset_minutes = offset;
}

static int atomic_write(const char *path, const char *data, size_t len)
{
    char tmp[PATH_MAX], backup[PATH_MAX];
    FILE *fp;
    int fd;
    int had_old;

    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    snprintf(backup, sizeof backup, "%s.bak", path);
    fp = fopen(tmp, "w");
    if (!fp) return 0;
    if (fwrite(data, 1, len, fp) != len || fflush(fp) != 0) {
        fclose(fp);
        unlink(tmp);
        return 0;
    }
    fd = fileno(fp);
    if (fd >= 0) {
        (void)fchmod(fd, 0600);
        (void)fsync(fd);
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        return 0;
    }

    had_old = access(path, F_OK) == 0;
    if (had_old) {
        unlink(backup);
        if (rename(path, backup) != 0) {
            unlink(tmp);
            return 0;
        }
    }
    if (rename(tmp, path) != 0) {
        if (had_old) (void)rename(backup, path);
        unlink(tmp);
        return 0;
    }
    return 1;
}

static const char *next_object(const char *p, char *out, size_t outlen)
{
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    const char *start;
    size_t len;

    if (!p) return NULL;
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;
    start = p;
    for (; *p; p++) {
        char c = *p;
        if (in_string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') in_string = 1;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) {
            p++;
            len = (size_t)(p - start);
            if (len >= outlen) len = outlen - 1;
            memcpy(out, start, len);
            out[len] = 0;
            return p;
        }
    }
    return NULL;
}

static uint64_t object_u64(const char *json, const char *key, uint64_t def)
{
    char value[48];
    char *end;
    unsigned long long n;
    if (!json_get(json, key, value, sizeof value) || value[0] == '-') return def;
    errno = 0;
    n = strtoull(value, &end, 10);
    return errno || end == value || *end ? def : (uint64_t)n;
}

static int64_t object_i64(const char *json, const char *key, int64_t def)
{
    char value[48];
    char *end;
    long long n;
    if (!json_get(json, key, value, sizeof value)) return def;
    errno = 0;
    n = strtoll(value, &end, 10);
    return errno || end == value ? def : (int64_t)n;
}

static int load_json_file(const char *path, char *out, size_t outlen)
{
    char backup[PATH_MAX];
    if (read_file(path, out, outlen)) return 1;
    snprintf(backup, sizeof backup, "%s.bak", path);
    return read_file(backup, out, outlen);
}

static void load_timezone(void)
{
    char json[1024];
    int offset, dst;
    if (!load_json_file(g_timezone_path, json, sizeof json)) return;
    offset = (int)json_get_int(json, "offset_minutes", 480);
    dst = (int)json_get_int(json, "dst_minutes", 0);
    if (offset >= -720 && offset <= 840 && offset % 15 == 0 &&
        (dst == 0 || dst == 60) && offset + dst >= -720 && offset + dst <= 840) {
        g_offset_minutes = offset;
        g_dst_minutes = dst;
    }
}

static void load_records(void)
{
    char json[USAGE_JSON_MAX], object[2048];
    const char *p;
    if (!load_json_file(g_state_path, json, sizeof json)) return;
    p = strstr(json, "\"records\"");
    if (!p || !(p = strchr(p, '['))) return;
    p++;
    while (g_record_count < USAGE_MAX_SIMS && (p = next_object(p, object, sizeof object))) {
        struct usage_record *r = &g_records[g_record_count];
        memset(r, 0, sizeof *r);
        if (!json_get(object, "id", r->id, sizeof r->id) || !valid_sim_id(r->id)) continue;
        json_get(object, "iccid", r->iccid, sizeof r->iccid);
        json_get(object, "operator", r->operator_name, sizeof r->operator_name);
        r->slot = (int)json_get_int(object, "slot", 0);
        r->baseline_valid = (int)json_get_int(object, "baseline_valid", 0) == 1;
        r->source_rx = object_u64(object, "source_rx", 0);
        r->source_tx = object_u64(object, "source_tx", 0);
        r->source_session = object_u64(object, "source_session", 0);
        r->lifetime_rx = object_u64(object, "lifetime_rx", 0);
        r->lifetime_tx = object_u64(object, "lifetime_tx", 0);
        r->day_rx = object_u64(object, "day_rx", 0);
        r->day_tx = object_u64(object, "day_tx", 0);
        r->day_key = (int)json_get_int(object, "day_key", 0);
        r->cycle_rx = object_u64(object, "cycle_rx", 0);
        r->cycle_tx = object_u64(object, "cycle_tx", 0);
        r->cycle_key = (int)json_get_int(object, "cycle_key", 0);
        r->cycle_start = object_i64(object, "cycle_start", 0);
        r->cycle_end = object_i64(object, "cycle_end", 0);
        r->last_seen = object_i64(object, "last_seen", 0);
        g_record_count++;
    }
}

static void load_plans(void)
{
    char json[USAGE_JSON_MAX], object[1024];
    const char *p;
    if (!load_json_file(g_plan_path, json, sizeof json)) return;
    p = strstr(json, "\"plans\"");
    if (!p || !(p = strchr(p, '['))) return;
    p++;
    while (g_plan_count < USAGE_MAX_SIMS && (p = next_object(p, object, sizeof object))) {
        struct usage_plan *plan = &g_plans[g_plan_count];
        memset(plan, 0, sizeof *plan);
        if (!json_get(object, "id", plan->id, sizeof plan->id) || !valid_sim_id(plan->id)) continue;
        plan->enabled = (int)json_get_int(object, "enabled", 0) == 1;
        plan->allowance_bytes = object_u64(object, "allowance_bytes", 0);
        plan->reset_day = (int)json_get_int(object, "reset_day", 1);
        if (plan->reset_day < 1 || plan->reset_day > 31) plan->reset_day = 1;
        g_plan_count++;
    }
}

static struct usage_record *find_record(const char *id, int create)
{
    int replace = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < g_record_count; i++) {
        if (!strcmp(g_records[i].id, id)) return &g_records[i];
        if (g_records[i].last_seen < oldest) {
            oldest = g_records[i].last_seen;
            replace = i;
        }
    }
    if (!create) return NULL;
    if (g_record_count < USAGE_MAX_SIMS) replace = g_record_count++;
    if (replace < 0) return NULL;
    memset(&g_records[replace], 0, sizeof g_records[replace]);
    snprintf(g_records[replace].id, sizeof g_records[replace].id, "%s", id);
    return &g_records[replace];
}

static struct usage_plan *find_plan(const char *id, int create)
{
    for (int i = 0; i < g_plan_count; i++)
        if (!strcmp(g_plans[i].id, id)) return &g_plans[i];
    if (!create || g_plan_count >= USAGE_MAX_SIMS) return NULL;
    memset(&g_plans[g_plan_count], 0, sizeof g_plans[g_plan_count]);
    snprintf(g_plans[g_plan_count].id, sizeof g_plans[g_plan_count].id, "%s", id);
    g_plans[g_plan_count].reset_day = 1;
    return &g_plans[g_plan_count++];
}

int usage_effective_offset_minutes(void)
{
    return g_offset_minutes + g_dst_minutes;
}

static int usage_clock_adjust_minutes(void)
{
    return usage_effective_offset_minutes() - g_system_offset_minutes;
}

void usage_localtime(time_t when, struct tm *out)
{
    time_t adjusted = when + (time_t)usage_clock_adjust_minutes() * 60;
    gmtime_r(&adjusted, out);
}

static int days_in_month(int year, int month)
{
    static const int days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int leap;
    if (month == 2) {
        leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return 28 + leap;
    }
    return days[month - 1];
}

static void previous_month(int *year, int *month)
{
    if (--*month == 0) {
        *month = 12;
        (*year)--;
    }
}

static void next_month(int *year, int *month)
{
    if (++*month == 13) {
        *month = 1;
        (*year)++;
    }
}

static time_t local_midnight_epoch(int year, int month, int day)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    return timegm(&tmv) - (time_t)usage_clock_adjust_minutes() * 60;
}

static void cycle_window(time_t now, int reset_day, int *key, int64_t *start, int64_t *end)
{
    struct tm local;
    int year, month, day;
    int start_year, start_month, start_day;
    int end_year, end_month, end_day;
    usage_localtime(now, &local);
    year = local.tm_year + 1900;
    month = local.tm_mon + 1;
    day = reset_day < days_in_month(year, month) ? reset_day : days_in_month(year, month);

    start_year = year;
    start_month = month;
    if (local.tm_mday < day) previous_month(&start_year, &start_month);
    start_day = reset_day < days_in_month(start_year, start_month) ?
                reset_day : days_in_month(start_year, start_month);

    end_year = start_year;
    end_month = start_month;
    next_month(&end_year, &end_month);
    end_day = reset_day < days_in_month(end_year, end_month) ?
              reset_day : days_in_month(end_year, end_month);

    *key = start_year * 100 + start_month;
    *start = (int64_t)local_midnight_epoch(start_year, start_month, start_day);
    *end = (int64_t)local_midnight_epoch(end_year, end_month, end_day);
}

static int current_day_key(time_t now)
{
    struct tm local;
    usage_localtime(now, &local);
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

static void prepare_windows(struct usage_record *r, time_t now)
{
    struct usage_plan *plan = find_plan(r->id, 0);
    int reset_day = plan ? plan->reset_day : 1;
    int day_key = current_day_key(now);
    int cycle_key;
    int64_t cycle_start, cycle_end;

    if (r->day_key != day_key) {
        r->day_key = day_key;
        r->day_rx = r->day_tx = 0;
        g_dirty = 1;
    }
    cycle_window(now, reset_day, &cycle_key, &cycle_start, &cycle_end);
    if (r->cycle_key != cycle_key) {
        r->cycle_key = cycle_key;
        r->cycle_rx = r->cycle_tx = 0;
        g_dirty = 1;
    }
    if (r->cycle_start != cycle_start || r->cycle_end != cycle_end) {
        r->cycle_start = cycle_start;
        r->cycle_end = cycle_end;
        g_dirty = 1;
    }
}

static int save_timezone(void)
{
    char json[256];
    int n = snprintf(json, sizeof json,
                     "{\"version\":1,\"offset_minutes\":%d,\"dst_minutes\":%d}\n",
                     g_offset_minutes, g_dst_minutes);
    return n > 0 && (size_t)n < sizeof json && atomic_write(g_timezone_path, json, (size_t)n);
}

static int save_plans(void)
{
    char json[USAGE_JSON_MAX];
    struct usage_buf b = { json, sizeof json, 0 };
    ub_append(&b, "{\"version\":1,\"plans\":[");
    for (int i = 0; i < g_plan_count; i++) {
        struct usage_plan *p = &g_plans[i];
        if (i) ub_append(&b, ",");
        ub_append(&b, "{\"id\":"); ub_json_string(&b, p->id);
        ub_append(&b, ",\"enabled\":%d,\"allowance_bytes\":%" PRIu64 ",\"reset_day\":%d}",
                  p->enabled, p->allowance_bytes, p->reset_day);
    }
    ub_append(&b, "]}\n");
    return b.len < b.cap - 1 && atomic_write(g_plan_path, json, b.len);
}

static int save_records(void)
{
    char json[USAGE_JSON_MAX];
    struct usage_buf b = { json, sizeof json, 0 };
    ub_append(&b, "{\"version\":1,\"saved_at\":%lld,\"records\":[",
              (long long)time(NULL));
    for (int i = 0; i < g_record_count; i++) {
        struct usage_record *r = &g_records[i];
        if (i) ub_append(&b, ",");
        ub_append(&b, "{\"id\":"); ub_json_string(&b, r->id);
        ub_append(&b, ",\"iccid\":"); ub_json_string(&b, r->iccid);
        ub_append(&b, ",\"operator\":"); ub_json_string(&b, r->operator_name);
        ub_append(&b,
                  ",\"slot\":%d,\"baseline_valid\":%d,\"source_rx\":%" PRIu64
                  ",\"source_tx\":%" PRIu64 ",\"source_session\":%" PRIu64
                  ",\"lifetime_rx\":%" PRIu64 ",\"lifetime_tx\":%" PRIu64
                  ",\"day_rx\":%" PRIu64 ",\"day_tx\":%" PRIu64 ",\"day_key\":%d"
                  ",\"cycle_rx\":%" PRIu64 ",\"cycle_tx\":%" PRIu64 ",\"cycle_key\":%d"
                  ",\"cycle_start\":%lld,\"cycle_end\":%lld,\"last_seen\":%lld}",
                  r->slot, r->baseline_valid, r->source_rx, r->source_tx, r->source_session,
                  r->lifetime_rx, r->lifetime_tx, r->day_rx, r->day_tx, r->day_key,
                  r->cycle_rx, r->cycle_tx, r->cycle_key,
                  (long long)r->cycle_start, (long long)r->cycle_end, (long long)r->last_seen);
    }
    ub_append(&b, "]}\n");
    if (b.len >= b.cap - 1 || !atomic_write(g_state_path, json, b.len)) return 0;
    g_last_persist = time(NULL);
    g_dirty = 0;
    return 1;
}

void usage_init(void)
{
    const char *dir = getenv("ZWRT_DATAD_DATA_DIR");
    if (dir && *dir && strlen(dir) < sizeof g_data_dir)
        snprintf(g_data_dir, sizeof g_data_dir, "%s", dir);
    (void)mkdir(g_data_dir, 0755);
    snprintf(g_state_path, sizeof g_state_path, "%s/traffic-state.json", g_data_dir);
    snprintf(g_plan_path, sizeof g_plan_path, "%s/traffic-config.json", g_data_dir);
    snprintf(g_timezone_path, sizeof g_timezone_path, "%s/timezone.json", g_data_dir);
    load_system_clock_offset();
    load_timezone();
    load_plans();
    load_records();
    g_last_persist = time(NULL);
}

void usage_set_identity(const char *iccid, const char *operator_name, int slot)
{
    char id[24];
    make_sim_id(id, sizeof id, iccid, slot);
    if (!g_identity_initialized) {
        snprintf(g_active_id, sizeof g_active_id, "%s", id);
        snprintf(g_sample_identity, sizeof g_sample_identity, "%s", id);
        g_identity_initialized = 1;
    } else if (strcmp(id, g_active_id) != 0) {
        usage_flush(1);
        snprintf(g_active_id, sizeof g_active_id, "%s", id);
        g_sample_identity[0] = 0;
    }
    snprintf(g_active_iccid, sizeof g_active_iccid, "%s", iccid ? iccid : "");
    snprintf(g_active_operator, sizeof g_active_operator, "%s", operator_name ? operator_name : "");
    g_active_slot = slot;
}

void usage_sample(const char *traffic_json, time_t now)
{
    uint64_t rx, tx, session = 0;
    struct usage_record *r;
    uint64_t drx = 0, dtx = 0;

    if (!g_active_id[0] || !traffic_json) return;
    if (!json_get_u64(traffic_json, "real_rx_bytes", &rx) ||
        !json_get_u64(traffic_json, "real_tx_bytes", &tx)) return;
    (void)json_get_u64(traffic_json, "real_time", &session);

    r = find_record(g_active_id, 1);
    if (!r) return;
    prepare_windows(r, now);
    snprintf(r->iccid, sizeof r->iccid, "%s", g_active_iccid);
    snprintf(r->operator_name, sizeof r->operator_name, "%s", g_active_operator);
    r->slot = g_active_slot;
    r->last_seen = now;

    if (strcmp(g_sample_identity, g_active_id) != 0 || !r->baseline_valid) {
        snprintf(g_sample_identity, sizeof g_sample_identity, "%s", g_active_id);
        r->source_rx = rx;
        r->source_tx = tx;
        r->source_session = session;
        r->baseline_valid = 1;
        g_dirty = 1;
        return;
    }

    if (rx >= r->source_rx && tx >= r->source_tx && session >= r->source_session) {
        drx = rx - r->source_rx;
        dtx = tx - r->source_tx;
    }
    r->source_rx = rx;
    r->source_tx = tx;
    r->source_session = session;
    if (drx || dtx) {
        r->lifetime_rx += drx;
        r->lifetime_tx += dtx;
        r->day_rx += drx;
        r->day_tx += dtx;
        r->cycle_rx += drx;
        r->cycle_tx += dtx;
        g_dirty = 1;
    }
    usage_flush(0);
}

void usage_flush(int force)
{
    time_t now = time(NULL);
    if (!g_dirty) return;
    if (!force && now - g_last_persist < USAGE_PERSIST_SEC) return;
    (void)save_records();
}

static void timezone_label(char *out, size_t outlen)
{
    int minutes = usage_effective_offset_minutes();
    char sign = minutes < 0 ? '-' : '+';
    int abs_minutes = minutes < 0 ? -minutes : minutes;
    snprintf(out, outlen, "UTC%c%02d:%02d", sign, abs_minutes / 60, abs_minutes % 60);
}

void usage_build_timezone_json(char *out, size_t outlen)
{
    char label[24];
    struct usage_buf b = { out, outlen, 0 };
    timezone_label(label, sizeof label);
    ub_append(&b, "{\"mode\":\"fixed_offset\",\"offset_minutes\":%d,\"dst_minutes\":%d,"
              "\"effective_offset_minutes\":%d,\"system_offset_minutes\":%d,"
              "\"clock_adjust_minutes\":%d,\"label\":",
              g_offset_minutes, g_dst_minutes, usage_effective_offset_minutes(),
              g_system_offset_minutes, usage_clock_adjust_minutes());
    ub_json_string(&b, label);
    ub_append(&b, "}");
}

static void iccid_tail(char *out, size_t outlen, const char *iccid)
{
    size_t n = iccid ? strlen(iccid) : 0;
    if (!n) snprintf(out, outlen, "-");
    else snprintf(out, outlen, "%s", iccid + (n > 4 ? n - 4 : 0));
}

void usage_build_traffic_json(char *out, size_t outlen)
{
    struct usage_buf b = { out, outlen, 0 };
    char tail[12];
    ub_append(&b, "{\"available\":true,\"source\":\"zwrt_data.get_wwandst\",\"active_id\":");
    ub_json_string(&b, g_active_id);
    ub_append(&b, ",\"sims\":[");
    for (int i = 0; i < g_record_count; i++) {
        struct usage_record *r = &g_records[i];
        struct usage_plan *p = find_plan(r->id, 0);
        uint64_t today = r->day_rx + r->day_tx;
        uint64_t cycle = r->cycle_rx + r->cycle_tx;
        uint64_t lifetime = r->lifetime_rx + r->lifetime_tx;
        uint64_t remaining = p && p->enabled && p->allowance_bytes > cycle ? p->allowance_bytes - cycle : 0;
        if (i) ub_append(&b, ",");
        iccid_tail(tail, sizeof tail, r->iccid);
        ub_append(&b, "{\"id\":"); ub_json_string(&b, r->id);
        ub_append(&b, ",\"slot\":%d,\"iccid_tail\":", r->slot); ub_json_string(&b, tail);
        ub_append(&b, ",\"operator\":"); ub_json_string(&b, r->operator_name);
        ub_append(&b,
                  ",\"active\":%s,\"today_rx_bytes\":%" PRIu64 ",\"today_tx_bytes\":%" PRIu64
                  ",\"today_bytes\":%" PRIu64 ",\"cycle_rx_bytes\":%" PRIu64
                  ",\"cycle_tx_bytes\":%" PRIu64 ",\"cycle_bytes\":%" PRIu64
                  ",\"lifetime_bytes\":%" PRIu64 ",\"package_enabled\":%s"
                  ",\"allowance_bytes\":%" PRIu64 ",",
                  !strcmp(r->id, g_active_id) ? "true" : "false",
                  r->day_rx, r->day_tx, today, r->cycle_rx, r->cycle_tx, cycle, lifetime,
                  p && p->enabled ? "true" : "false", p ? p->allowance_bytes : 0);
        if (p && p->enabled) ub_append(&b, "\"remaining_bytes\":%" PRIu64 ",", remaining);
        else ub_append(&b, "\"remaining_bytes\":null,");
        ub_append(&b, "\"reset_day\":%d,\"cycle_start\":%lld,\"cycle_end\":%lld,\"last_seen\":%lld}",
                  p ? p->reset_day : 1, (long long)r->cycle_start,
                  (long long)r->cycle_end, (long long)r->last_seen);
    }
    ub_append(&b, "]}");
}

int usage_set_timezone_from_path(const char *path, char *out, size_t outlen)
{
    int offset, dst;
    int got_offset = query_int(path, "offset_minutes", &offset);
    int got_dst = query_int(path, "dst_minutes", &dst);
    if (got_offset <= 0 || got_dst <= 0 || offset < -720 || offset > 840 || offset % 15 ||
        (dst != 0 && dst != 60) || offset + dst < -720 || offset + dst > 840) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"invalid_timezone\"}");
        return 400;
    }
    g_offset_minutes = offset;
    g_dst_minutes = dst;
    if (!save_timezone()) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"persist_failed\"}");
        return 500;
    }
    for (int i = 0; i < g_record_count; i++) prepare_windows(&g_records[i], time(NULL));
    usage_flush(1);
    usage_build_timezone_json(out, outlen);
    return 200;
}

int usage_set_plan_from_path(const char *path, char *out, size_t outlen)
{
    char id[24];
    int enabled, reset_day;
    uint64_t allowance;
    struct usage_plan *plan;
    if (!query_value(path, "sim_id", id, sizeof id) || !valid_sim_id(id) ||
        query_int(path, "enabled", &enabled) <= 0 ||
        query_u64(path, "allowance_bytes", &allowance) <= 0 ||
        query_int(path, "reset_day", &reset_day) <= 0 ||
        (enabled != 0 && enabled != 1) || reset_day < 1 || reset_day > 31 ||
        !find_record(id, 0)) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"invalid_plan\"}");
        return 400;
    }
    plan = find_plan(id, 1);
    if (!plan) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"plan_limit\"}");
        return 409;
    }
    plan->enabled = enabled;
    plan->allowance_bytes = allowance;
    plan->reset_day = reset_day;
    if (!save_plans()) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"persist_failed\"}");
        return 500;
    }
    prepare_windows(find_record(id, 0), time(NULL));
    usage_flush(1);
    snprintf(out, outlen,
             "{\"ok\":true,\"sim_id\":\"%s\",\"enabled\":%s,"
             "\"allowance_bytes\":%" PRIu64 ",\"reset_day\":%d}",
             id, enabled ? "true" : "false", allowance, reset_day);
    return 200;
}
