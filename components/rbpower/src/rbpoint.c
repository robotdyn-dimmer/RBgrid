/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file  rbpoint.c
 * @brief Point-slug utilities and source-to-capability mapping (pure; covered by the logic self-test suite_point_slug).
 */
#include "rbpoint.h"
#include <string.h>
#include <stdio.h>

bool rbpoint_slug_valid(const char* s)
{
    if (!s || !s[0]) return false;
    size_t n = strlen(s);
    if (n > RBPOINT_SLUG_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/** Transliterate one 2-byte UTF-8 Cyrillic sequence (U+0410..U+044F plus U+0401/U+0451) to Latin,
 *  folded to lowercase. Returns "" for the hard/soft signs, NULL if the sequence is not Cyrillic. */
static const char* translit_cyr(uint8_t b1, uint8_t b2)
{
    static const char* tab[64] = {
        /* D0 90..D0 AF: U+0410..U+042F uppercase (folded to lowercase) */
        "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
        "r","s","t","u","f","h","ts","ch","sh","sch","","y","","e","yu","ya",
        /* D0 B0..D0 BF: U+0430..U+043F lowercase, first half */
        "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
        /* D1 80..D1 8F: U+0440..U+044F lowercase, second half */
        "r","s","t","u","f","h","ts","ch","sh","sch","","y","","e","yu","ya",
    };
    if (b1 == 0xD0 && b2 >= 0x90 && b2 <= 0xBF) return tab[b2 - 0x90];
    /* The U+0440..U+044F row lives at tab[48..63]: rows are uppercase (0..31), lowercase first
     * half (32..47), lowercase second half (48..63). Indexing 32+(b2-0x80) would hit the wrong row. */
    if (b1 == 0xD1 && b2 >= 0x80 && b2 <= 0x8F) return tab[48 + (b2 - 0x80)];
    if (b1 == 0xD0 && b2 == 0x81) return "e";   /* U+0401 */
    if (b1 == 0xD1 && b2 == 0x91) return "e";   /* U+0451 */
    return NULL;
}

void rbpoint_slug_from_name(const char* name, char* out)
{
    size_t o = 0;
    bool last_sep = true;   /* suppress leading separators */
    for (const uint8_t* p = (const uint8_t*)(name ? name : ""); *p && o < RBPOINT_SLUG_MAX; ) {
        const char* rep = NULL;
        char c = 0;
        if ((*p == 0xD0 || *p == 0xD1) && p[1]) {
            rep = translit_cyr(p[0], p[1]);
            p += 2;
            if (rep && !rep[0]) continue;       /* hard/soft sign: dropped */
        } else {
            c = (char)*p++;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        if (rep) {
            for (const char* r = rep; *r && o < RBPOINT_SLUG_MAX; r++) out[o++] = *r;
            last_sep = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '-') {
            out[o++] = c;
            last_sep = false;
        } else {
            if (!last_sep) { out[o++] = '_'; last_sep = true; }   /* squeeze runs */
        }
    }
    while (o > 0 && (out[o-1] == '_' )) o--;    /* trim trailing separator */
    out[o] = '\0';
    if (!out[0]) strcpy(out, "unit");
}

void rbpoint_slug_uniquify(const char* base, char* out,
                           bool (*taken)(const char* slug, void* arg), void* arg)
{
    char b[RBPOINT_SLUG_MAX + 1];
    strncpy(b, base, RBPOINT_SLUG_MAX); b[RBPOINT_SLUG_MAX] = '\0';
    if (!taken || !taken(b, arg)) { strcpy(out, b); return; }
    for (int n = 2; n < 1000; n++) {
        char suf[8];
        int sl = snprintf(suf, sizeof(suf), "-%d", n);
        int keep = RBPOINT_SLUG_MAX - sl;
        char cand[RBPOINT_SLUG_MAX + 1];
        snprintf(cand, sizeof(cand), "%.*s%s", keep, b, suf);
        if (!taken(cand, arg)) { strcpy(out, cand); return; }
    }
    strcpy(out, b);   /* pathological: give up on uniqueness, caller's validation still runs */
}

uint32_t rbpoint_caps_for_source(rbpower_source_type_t source_type,
                                 rbpower_metric_type_t metric_type,
                                 bool bidirectional)
{
    uint32_t caps = 0;
    if (metric_type == RBPOWER_METRIC_VOLTAGE_BUS) {
        caps |= RBUNIT_CAP_VOLTAGE;
    } else {
        caps |= RBUNIT_CAP_CURRENT | RBUNIT_CAP_ACTIVE_POWER | RBUNIT_CAP_ENERGY;
    }
    switch (source_type) {
        case RBPOWER_SOURCE_I2C_SENSOR:        /* rbAmp: PF + line frequency on-device */
            caps |= RBUNIT_CAP_PF | RBUNIT_CAP_FREQ;
            break;
        case RBPOWER_SOURCE_ESP_NOW:
            caps |= RBUNIT_CAP_PF | RBUNIT_CAP_FREQ | RBUNIT_CAP_REMOTE;
            break;
        case RBPOWER_SOURCE_VIRTUAL_CALC:
        case RBPOWER_SOURCE_AGGREGATED:
            caps |= RBUNIT_CAP_VIRTUAL;
            break;
        default:                               /* other sources: the base set only */
            break;
    }
    if (bidirectional) caps |= RBUNIT_CAP_BIDIRECTIONAL;
    return caps;
}
