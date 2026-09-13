/*
 * HTML font compatibility for subtitle cues.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Kept identical in the Torro FFmpeg and mpv forks; verified by
 * tests/subtitle-font-markup.py. No render-time state or allocations.
 */
#ifndef TORRO_HTMLFONT_H
#define TORRO_HTMLFONT_H

#include <limits.h>
#include <string.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/parseutils.h>

struct torro_font {
    char face[128];
    unsigned size;
    int color;
};

struct torro_fonts {
    /* Preserve FFmpeg's existing 16-entry font stack and face limit. */
    struct torro_font stack[16];
    unsigned depth;
    size_t overflow;
};

static inline size_t torro_font_space(const char *p, const char *end)
{
    if (p == end)
        return 0;
    if (av_isspace((unsigned char)*p))
        return 1;
    /* Tolerate UTF-8 NBSP as an attribute separator, not in quoted values. */
    return end - p >= 2 && (unsigned char)p[0] == 0xc2 &&
           (unsigned char)p[1] == 0xa0 ? 2 : 0;
}

static inline const char *torro_font_skip_space(const char *p, const char *end)
{
    size_t n;
    while ((n = torro_font_space(p, end)))
        p += n;
    return p;
}

static inline int torro_font_equal(const char *p, size_t n, const char *name)
{
    return n == strlen(name) && !av_strncasecmp(p, name, n);
}

/* Return consumed bytes, or zero for ordinary text/incomplete tags.
 * Kodi 21.2 DVDSubtitleTagSami.cpp:72-76,142-168 recognizes font tags
 * without a 127-byte limit and accepts whitespace around '=' and both
 * quote forms. Retain FFmpeg's additional size/face and nested restoration.
 * Scan spans directly: long tags need no temporary allocation or new cap.
 */
static inline size_t torro_font_tag(AVBPrint *dst, const char *in, size_t len,
                                   struct torro_fonts *fonts, void *log_ctx)
{
    const char *end = in + len, *p, *tag_end, *name;
    int closing, quote = 0;
    if (!len || *in != '<')
        return 0;
    p = torro_font_skip_space(in + 1, end);
    closing = p != end && *p == '/';
    if (closing)
        p = torro_font_skip_space(p + 1, end);
    if (end - p < 4 || av_strncasecmp(p, "font", 4))
        return 0;
    p += 4;
    if (p == end || (*p != '>' && *p != '/' && !torro_font_space(p, end)))
        return 0;
    for (tag_end = p; tag_end != end; tag_end++) {
        // A new '<' ends a malformed tag even inside a missing quote.
        // This also keeps repeated incomplete tags linear in cue length.
        if (*tag_end == '<')
            return 0;
        if (quote) {
            if (*tag_end == quote)
                quote = 0;
        } else if (*tag_end == '\'' || *tag_end == '"') {
            quote = *tag_end;
        } else if (*tag_end == '>') {
            break;

        }
    }
    if (tag_end == end)
        return 0;
    if (closing) {
        if (fonts->overflow) {
            fonts->overflow--;
        } else if (fonts->depth) {
            struct torro_font *cur = &fonts->stack[fonts->depth--];
            struct torro_font *prev = &fonts->stack[fonts->depth];
            if (cur->size != prev->size) {
                if (prev->size) av_bprintf(dst, "{\\fs%u}", prev->size);
                else av_bprintf(dst, "{\\fs}");
            }
            if (cur->color != prev->color) {
                if (prev->color) av_bprintf(dst, "{\\c&H%X&}", (unsigned)(prev->color & 0xffffff));
                else av_bprintf(dst, "{\\c}");
            }
            if (strcmp(cur->face, prev->face))
                av_bprintf(dst, "{\\fn%s}", prev->face);
        }
    } else if (fonts->overflow || fonts->depth == 15) {
        /* Ignored excess opens must not pop a real outer font on close. */
        fonts->overflow++;
    } else {
        struct torro_font *font = &fonts->stack[fonts->depth + 1];
        *font = fonts->stack[fonts->depth++];
        while ((p = torro_font_skip_space(p, tag_end)) != tag_end) {
            const char *value, *value_end;
            size_t name_len;
            name = p;
            while (p != tag_end && (av_tolower((unsigned char)*p) >= 'a' && av_tolower((unsigned char)*p) <= 'z'))
                p++;
            name_len = p - name;
            if (!name_len) { p++; continue; }
            p = torro_font_skip_space(p, tag_end);
            if (p == tag_end || *p != '=')
                continue;
            p = torro_font_skip_space(p + 1, tag_end);
            quote = p != tag_end && (*p == '"' || *p == '\'') ? *p++ : 0;
            value = p;
            while (p != tag_end && (quote ? *p != quote : !torro_font_space(p, tag_end)))
                p++;
            value_end = p;
            if (quote && p != tag_end)
                p++;
            if (torro_font_equal(name, name_len, "color")) {
                uint8_t rgba[4];
                while (value_end - value > 1 && value[0] == '#' && value[1] == '#')
                    value++;
                if (value != value_end && value_end - value <= INT_MAX &&
                    av_parse_color(rgba, value, value_end - value, log_ctx) >= 0) {
                    font->color = 0x1000000 | rgba[0] | rgba[1] << 8 | rgba[2] << 16;
                    av_bprintf(dst, "{\\c&H%X&}", (unsigned)(font->color & 0xffffff));
                }
            } else if (torro_font_equal(name, name_len, "size")) {
                unsigned size = 0;
                const char *digit = value;
                if (digit != value_end && *digit == '+') digit++;
                const char *first_digit = digit;
                while (digit != value_end && av_isdigit(*digit) &&
                       size <= (INT_MAX - (*digit - '0')) / 10U)
                    size = size * 10 + (*digit++ - '0');
                if (digit == value_end && digit != first_digit) {
                    font->size = size;
                    av_bprintf(dst, "{\\fs%u}", size);
                }
            } else if (torro_font_equal(name, name_len, "face")) {
                size_t n = value_end - value;
                /* A font family is data, never an ASS override block. */
                if (!memchr(value, '{', n) && !memchr(value, '}', n) &&
                    !memchr(value, '\\', n)) {
                    if (n >= sizeof(font->face)) n = sizeof(font->face) - 1;
                    memcpy(font->face, value, n);
                    font->face[n] = 0;
                    av_bprintf(dst, "{\\fn%s}", font->face);
                }
            }
        }
    }
    return tag_end + 1 - in;
}

/* ASS bypasses FFmpeg's SRT decoder. Convert only font tags in event Text;
 * keep native ASS overrides, line breaks, spacing and all other text intact.
 * Kodi 21.2 DVDSubtitleTagSami.cpp:142-168 supplies font compatibility;
 * this restricted mode preserves ASS instead of applying SRT normalization.
 * Caller owns dst. Return 1 if changed, 0 if unchanged, negative on OOM.
 */
static inline int torro_font_ass(AVBPrint *dst, const char *text)
{
    struct torro_fonts fonts = {0};
    const char *p = text, *end;
    int changed = 0;
    if (!strchr(text, '<'))
        return 0;
    end = text + strlen(text);
    while (p != end) {
        size_t n = 0;
        if (*p == '{') {
            const char *close = memchr(p, '}', end - p);
            n = close ? close + 1 - p : end - p;
            av_bprint_append_data(dst, p, n);
        } else if (*p == '<' && (n = torro_font_tag(dst, p, end - p, &fonts, NULL))) {
            changed = 1;
        } else {
            n = 1;
            av_bprint_chars(dst, *p, 1);
        }
        p += n;
    }
    return av_bprint_is_complete(dst) ? changed : AVERROR(ENOMEM);
}
#endif
