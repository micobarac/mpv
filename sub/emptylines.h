/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef TORRO_EMPTYLINES_H
#define TORRO_EMPTYLINES_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum torro_ass_token_kind { TORRO_INK, TORRO_SPACE, TORRO_TAG, TORRO_BREAK };

static inline bool torro_ass_tag_space(char c)
{
    return c == ' ' || c == '\t';
}

/* Mirror libass ass_parse.c:282-369 argument boundaries, including ignored
 * parenthesized arguments and tags inside transforms (:669-724). We only
 * interpret q; all bytes, including every positioning tag, are retained.
 * Like libass, recursion is at most one level: the nested span ends at the
 * first closing parenthesis. Further nested transforms use the tail path.
 */
static inline void torro_ass_wrap_tags(const char *p, const char *end,
                                       int default_wrap, int *wrap)
{
    const char *closing = memchr(p, ')', end - p);
    while (p < end) {
        if (*p++ != '\\') continue;
        while (p < end && torro_ass_tag_space(*p)) p++;
        const char *name = p;
        while (p < end && *p != '(' && *p != '\\') p++;
        const char *name_end = p;
        if (name == name_end) continue;
        const char *first = NULL, *last = NULL, *last_end = NULL;
        int nargs = 0;
        bool backslash_arg = false;
        if (p < end && *p == '(') {
            p++;
            for (;;) {
                while (p < end && torro_ass_tag_space(*p)) p++;
                const char *arg = p;
                while (p < end && *p != ',' && *p != '\\' && *p != ')') p++;
                if (p < end && *p == '\\') {
                    backslash_arg = true;
                    // Cache the next ')' across tail-parsed transforms;
                    // rescanning the same suffix would be quadratic.
                    if (closing && closing < p)
                        closing = memchr(p, ')', end - p);
                    p = closing ? closing : end;
                }
                const char *arg_end = p;
                while (arg_end > arg && torro_ass_tag_space(arg_end[-1])) arg_end--;
                if (arg != arg_end) {
                    if (!first) first = arg;
                    last = arg;
                    last_end = arg_end;
                    if (nargs < 5) nargs++;
                }
                if (p == end) break;
                if (*p++ != ',') break;
            }
        }
        if (*name == 'q') {
            const char *arg = name + 1;
            while (arg < name_end && torro_ass_tag_space(*arg)) arg++;
            if (!first && arg < name_end) first = arg;
            // A present but nonnumeric argument means zero, as argtoi32;
            // only a missing argument (or out-of-range value) resets q.
            long value = first ? strtol(first, NULL, 10) : -1;
            *wrap = value < 0 || value > 3 ? default_wrap : (int)value;
        } else if (*name == 't' && backslash_arg && nargs >= 1 && nargs <= 4) {
            if (last_end < end) {
                torro_ass_wrap_tags(last, last_end, default_wrap, wrap);
            } else {
                p = last; // tail call, no growth for unterminated transforms
            }
        }
    }
}

/* Tokenize only layout boundaries; copy override blocks verbatim. The escape
 * rules match libass ass_parse.c:1116-1150 (ass_get_next_char). q controls
 * whether a soft break is a space or a row, as in ass_parse.c:905-909.
 * A zero length denotes a malformed block, which we leave untouched.
 */
static inline size_t torro_ass_token(const char *p, int default_wrap, int *wrap,
                                    enum torro_ass_token_kind *kind)
{
    *kind = TORRO_INK;
    if (*p == '{') {
        const char *end = strchr(p, '}');
        if (!end) return 0;
        *kind = TORRO_TAG;
        torro_ass_wrap_tags(p + 1, end, default_wrap, wrap);
        return end + 1 - p;
    }
    if (*p == '\\') {
        if (p[1] == 'N' || (p[1] == 'n' && *wrap == 2)) {
            *kind = TORRO_BREAK;
            return 2;
        }
        if (p[1] == 'h' || p[1] == 'n') {
            *kind = TORRO_SPACE;
            return 2;
        }
        if (p[1] == '{' || p[1] == '}') return 2;
    }
    if (*p == '\n') {
        *kind = TORRO_BREAK;
        return 1;
    }
    if (*p == ' ' || *p == '\t' || *p == '\r') {
        *kind = TORRO_SPACE;
        return 1;
    }
    /* UTF-8 Unicode spaces, including NBSP produced by SRT/SAMI decoders.
     * Do not treat arbitrary non-ASCII or transparent text as empty.
     */
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] == 0xc2 && u[1] == 0xa0) {
        *kind = TORRO_SPACE;
        return 2;
    }
    if ((u[0] == 0xe1 && u[1] == 0x9a && u[2] == 0x80) ||
        (u[0] == 0xe2 && u[1] == 0x80 &&
         ((u[2] >= 0x80 && u[2] <= 0x8a) || u[2] == 0xaf)) ||
        (u[0] == 0xe2 && u[1] == 0x81 && u[2] == 0x9f) ||
        (u[0] == 0xe3 && u[1] == 0x80 && u[2] == 0x80)) {
        *kind = TORRO_SPACE;
        return 3;
    }
    return 1;
}

/* Kodi 21.2 DVDSubtitleTagSami.cpp:82-84 trims input, :211-221 recognizes
 * hard spaces and :225-229 discards an empty SAMI paragraph. User-requested
 * extension: remove whitespace-only ASS rows too, including hard spaces,
 * after format conversion. Preserve every override (pos/move/an, styles,
 * drawing and animation), timing, and spacing within nonempty rows.
 * Operates once per accepted cue, in place, O(text length), no allocation
 * or retained state. This is not a positioning override or render-loop job.
 */
static inline void torro_ass_remove_empty_lines(char *text, int default_wrap,
                                               const char *effect)
{
    enum torro_ass_token_kind kind;
    // libass ass_parse.c:940-971 starts Banner events in wrap mode 2;
    // an explicit/bare q still overrides/resets to the track's WrapStyle.
    int initial_wrap = effect && !strncmp(effect, "Banner;", 7) ? 2 : default_wrap;
    int wrap = initial_wrap;
    // Validate before writing, so a malformed suffix preserves the whole cue.
    for (const char *p = text; *p;) {
        size_t n = torro_ass_token(p, default_wrap, &wrap, &kind);
        if (!n) return;
        p += n;
    }
    wrap = initial_wrap;
    char *read = text, *write = text;
    bool have_ink_row = false;
    size_t separator = 0;
    while (*read) {
        char *start = read;
        int start_wrap = wrap;
        bool ink = false;
        size_t boundary = 0;
        while (*read) {
            size_t n = torro_ass_token(read, default_wrap, &wrap, &kind);
            if (kind == TORRO_BREAK) { boundary = n; break; }
            ink |= kind == TORRO_INK;
            read += n;
        }
        char *end = read;
        read += boundary;
        if (ink) {
            if (have_ink_row && separator) {
                // Use a hard break: tags retained from a removed row may
                // have changed q since the original separator was parsed.
                if (separator == 2) *write++ = '\\';
                *write++ = separator == 2 ? 'N' : '\n';
            }
            memmove(write, start, end - start);
            write += end - start;
            have_ink_row = true;
            separator = boundary;
        } else {
            // Whitespace goes; overrides keep their original ordering and
            // still affect subsequent text (or the event's global position).
            for (char *p = start; p < end;) {
                size_t n = torro_ass_token(p, default_wrap, &start_wrap, &kind);
                if (kind == TORRO_TAG) {
                    memmove(write, p, n);
                    write += n;
                }
                p += n;
            }
        }
    }
    *write = '\0';
}
#endif
