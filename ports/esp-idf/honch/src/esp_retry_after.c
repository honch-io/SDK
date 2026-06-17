// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#include "esp_retry_after.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>

static int honch_esp_month_index(const char *month)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (month == NULL) {
        return -1;
    }
    for (int i = 0; i < 12; i++) {
        if (strcasecmp(month, months[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool honch_esp_parse_retry_after_seconds(const char *value, uint64_t *delay_ms)
{
    if (value == NULL || delay_ms == NULL) {
        return false;
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (*value < '0' || *value > '9') {
        return false;
    }

    uint64_t seconds = 0u;
    while (*value >= '0' && *value <= '9') {
        uint64_t digit = (uint64_t)(*value - '0');
        if (seconds > (UINT64_MAX - digit) / 10u) {
            seconds = UINT64_MAX / 1000u;
            break;
        }
        seconds = (seconds * 10u) + digit;
        value++;
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (*value != '\0') {
        return false;
    }

    *delay_ms = seconds > UINT64_MAX / 1000u ? UINT64_MAX : seconds * 1000u;
    return true;
}

static int64_t honch_esp_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2u;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    int adjusted_month = (int)month + ((int)month > 2 ? -3 : 9);
    unsigned day_of_year = (unsigned)((153 * adjusted_month + 2) / 5) + day - 1u;
    unsigned day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool honch_esp_parse_retry_after_http_date(const char *value, uint64_t now_ms, uint64_t *delay_ms)
{
    if (value == NULL || delay_ms == NULL) {
        return false;
    }

    char weekday[4] = {0};
    char month_name[4] = {0};
    char gmt[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    int matched = sscanf(
        value,
        "%3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d %3[A-Za-z]%n",
        weekday,
        &day,
        month_name,
        &year,
        &hour,
        &minute,
        &second,
        gmt,
        &consumed);
    if (matched != 8 || value[consumed] != '\0' || strcasecmp(gmt, "GMT") != 0) {
        return false;
    }

    int month = honch_esp_month_index(month_name);
    if (year < 1970 || month < 0 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }

    int64_t days = honch_esp_days_from_civil(year, (unsigned)month + 1u, (unsigned)day);
    if (days < 0) {
        return false;
    }

    /*
     * The header is a well-formed HTTP-date. If the wall clock is not set
     * (now_ms == 0, e.g. before SNTP sync at boot) we cannot compute the exact
     * delay, but the server has explicitly asked us to back off -- honor it with
     * a conservative fixed fallback rather than ignoring the header.
     */
    if (now_ms == 0u) {
        *delay_ms = HONCH_ESP_RETRY_AFTER_NO_CLOCK_FALLBACK_MS;
        return true;
    }

    uint64_t target_ms = ((uint64_t)days * 86400u + (uint64_t)hour * 3600u +
        (uint64_t)minute * 60u + (uint64_t)second) * 1000u;
    if (target_ms <= now_ms) {
        *delay_ms = 0u;
        return true;
    }
    *delay_ms = target_ms - now_ms;
    return true;
}

bool honch_esp_parse_retry_after(const char *value, uint64_t now_ms, uint64_t *delay_ms)
{
    return honch_esp_parse_retry_after_seconds(value, delay_ms) ||
        honch_esp_parse_retry_after_http_date(value, now_ms, delay_ms);
}
