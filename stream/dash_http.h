/* Strict numeric Content-Range parsing shared by the typed transports. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

static inline bool mp_dash_http_number(const char **at, uint64_t *value)
{
    const char *p = *at;
    uint64_t n = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = *p++ - '0';
        if (n > ((uint64_t)INT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *at = p; *value = n;
    return true;
}

static inline bool mp_dash_http_range(const char *at, uint64_t start,
                                     uint64_t maximum, int64_t known_total,
                                     uint64_t *length, int64_t *total)
{
    uint64_t first, last, size;
    if (!at || strncmp(at, "bytes ", 6)) return false;
    at += 6;
    if (!mp_dash_http_number(&at, &first) || *at++ != '-' ||
        !mp_dash_http_number(&at, &last) || *at++ != '/' ||
        !mp_dash_http_number(&at, &size) || *at || !size ||
        first != start || last < first || last >= size || last > maximum ||
        (known_total > 0 && size != known_total)) return false;
    *length = last - first + 1; *total = size;
    return true;
}
