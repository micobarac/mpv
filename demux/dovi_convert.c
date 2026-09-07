/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <libdovi/rpu_parser.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mem.h>

#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "mpv_talloc.h"

#include "dovi_convert.h"

#define HEVC_NAL_UNSPEC62 62
#define HEVC_NAL_UNSPEC63 63

// Base-layer packets held while their EL twin is outstanding. EL blocks
// follow their BL block within the same cluster in every remux seen, so
// this is a few frames deep in practice.
#define HOLD_MAX 8
// PTS match tolerance between the two tracks (same timebase, same frame).
#define PTS_TOLERANCE 0.0001

struct held_rpu {
    double pts;
    uint8_t *nalu;   // complete escaped NAL unit, talloc'd under the converter
    size_t len;
};

struct mp_dovi_convert {
    struct mp_log *log;
    struct demuxer *demuxer;
    struct sh_stream *bl;
    bool two_track;
    int nal_len;                        // hvcC NAL length-prefix size, 1..4

    struct demux_packet *bl_q[HOLD_MAX];
    int num_bl_q;
    struct held_rpu rpu_q[HOLD_MAX];
    int num_rpu_q;

    struct demux_packet *out[HOLD_MAX * 2];
    int num_out;

    bool warned_convert;
    bool warned_unpaired;
};

static int nal_length_size(struct mp_codec_params *codec)
{
    // hvcC: byte 21, low two bits = lengthSizeMinusOne.
    if (codec->extradata_size >= 22 && codec->extradata[0] == 1)
        return (codec->extradata[21] & 3) + 1;
    return 4;
}

struct mp_dovi_convert *mp_dovi_convert_create(struct demuxer *demuxer,
                                               struct sh_stream *bl,
                                               bool two_track)
{
    if (!bl || bl->type != STREAM_VIDEO || !bl->codec || !bl->codec->codec ||
        strcmp(bl->codec->codec, "hevc") != 0)
        return NULL;

    struct mp_dovi_convert *s = talloc_zero(demuxer, struct mp_dovi_convert);
    s->log = demuxer->log;
    s->demuxer = demuxer;
    s->bl = bl;
    s->two_track = two_track;
    s->nal_len = nal_length_size(bl->codec);

    MP_VERBOSE(demuxer, "Dolby Vision profile 7 -> 8.1 conversion on stream %d "
               "(%s layout).\n", bl->index, two_track ? "two-track" : "interleaved");
    return s;
}

void mp_dovi_convert_fix_codec(struct mp_codec_params *codec)
{
    codec->dovi = true;
    codec->dv_profile = 8;
    codec->dv_el_present = false;

    if (!codec->lav_codecpar)
        return;

    size_t size;
    AVDOVIDecoderConfigurationRecord *cfg = av_dovi_alloc(&size);
    if (!cfg)
        return;

    const AVPacketSideData *old = av_packet_side_data_get(
        codec->lav_codecpar->coded_side_data,
        codec->lav_codecpar->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (old && old->size >= size)
        memcpy(cfg, old->data, size);

    cfg->dv_version_major = 1;
    cfg->dv_version_minor = 0;
    cfg->dv_profile = 8;
    cfg->rpu_present_flag = 1;
    cfg->el_present_flag = 0;
    cfg->bl_present_flag = 1;
    // 8.1: HDR10-compatible base layer.
    cfg->dv_bl_signal_compatibility_id = 1;

    av_packet_side_data_remove(codec->lav_codecpar->coded_side_data,
                               &codec->lav_codecpar->nb_coded_side_data,
                               AV_PKT_DATA_DOVI_CONF);
    if (!av_packet_side_data_add(&codec->lav_codecpar->coded_side_data,
                                 &codec->lav_codecpar->nb_coded_side_data,
                                 AV_PKT_DATA_DOVI_CONF, cfg, size, 0))
        av_free(cfg);
}

// Convert one RPU NAL unit (with header, escaped) to profile 8.1. Returns
// a talloc'd copy of the rewritten NAL unit, or NULL on failure.
static uint8_t *convert_rpu(struct mp_dovi_convert *s, const uint8_t *nal,
                            size_t len, size_t *out_len)
{
    DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, len);
    if (!rpu)
        return NULL;

    uint8_t *res = NULL;
    const DoviRpuDataHeader *hdr = dovi_rpu_get_header(rpu);
    if (!hdr) {
        if (!s->warned_convert) {
            const char *err = dovi_rpu_get_error(rpu);
            MP_WARN(s, "Dolby Vision RPU parse failed: %s\n", err ? err : "?");
            s->warned_convert = true;
        }
        goto done;
    }
    bool is_p7 = hdr->guessed_profile == 7;
    dovi_rpu_free_header(hdr);

    if (is_p7 && dovi_convert_rpu_with_mode(rpu, 2) < 0) {
        if (!s->warned_convert) {
            const char *err = dovi_rpu_get_error(rpu);
            MP_WARN(s, "Dolby Vision RPU conversion failed: %s\n", err ? err : "?");
            s->warned_convert = true;
        }
        goto done;
    }

    const DoviData *data = dovi_write_unspec62_nalu(rpu);
    if (!data)
        goto done;
    res = talloc_memdup(s, (void *)data->data, data->len);
    *out_len = data->len;
    dovi_data_free(data);

done:
    dovi_rpu_free(rpu);
    return res;
}

static uint32_t read_len(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static void write_len(uint8_t *p, int n, uint32_t v)
{
    for (int i = n - 1; i >= 0; i--) {
        p[i] = v & 0xff;
        v >>= 8;
    }
}

// Rewrite a base-layer packet: drop UNSPEC63, convert an inline UNSPEC62,
// and append `ext_rpu` (an already converted NAL unit from the EL track)
// when the packet carried no RPU of its own.
static struct demux_packet *rewrite_bl(struct mp_dovi_convert *s,
                                       struct demux_packet *dp,
                                       const uint8_t *ext_rpu, size_t ext_len)
{
    const int nl = s->nal_len;
    const uint8_t *src = dp->buffer;
    size_t src_len = dp->len;

    // Worst case: the converted RPU can be larger than the original, plus
    // one appended NAL.
    size_t cap = src_len + ext_len + nl + 4096;
    uint8_t *dst = talloc_size(NULL, cap);
    size_t dst_len = 0;
    bool had_rpu = false;
    bool changed = false;

    size_t pos = 0;
    while (pos + nl <= src_len) {
        uint32_t len = read_len(src + pos, nl);
        if (len == 0 || pos + nl + len > src_len)
            break; // malformed; keep whatever remains untouched below
        const uint8_t *nal = src + pos + nl;
        int type = (nal[0] >> 1) & 0x3f;

        if (type == HEVC_NAL_UNSPEC63) {
            changed = true;
        } else if (type == HEVC_NAL_UNSPEC62) {
            had_rpu = true;
            size_t conv_len = 0;
            uint8_t *conv = convert_rpu(s, nal, len, &conv_len);
            if (conv) {
                if (dst_len + nl + conv_len > cap) {
                    cap = dst_len + nl + conv_len + 4096;
                    dst = talloc_realloc_size(NULL, dst, cap);
                }
                write_len(dst + dst_len, nl, conv_len);
                memcpy(dst + dst_len + nl, conv, conv_len);
                dst_len += nl + conv_len;
                talloc_free(conv);
                changed = true;
            } else {
                memcpy(dst + dst_len, src + pos, nl + len);
                dst_len += nl + len;
            }
        } else {
            memcpy(dst + dst_len, src + pos, nl + len);
            dst_len += nl + len;
        }
        pos += nl + len;
    }
    if (pos < src_len) {
        memcpy(dst + dst_len, src + pos, src_len - pos);
        dst_len += src_len - pos;
    }

    if (!had_rpu && ext_rpu && ext_len) {
        write_len(dst + dst_len, nl, ext_len);
        memcpy(dst + dst_len + nl, ext_rpu, ext_len);
        dst_len += nl + ext_len;
        changed = true;
    }

    if (!changed) {
        talloc_free(dst);
        return dp;
    }

    struct demux_packet *res =
        new_demux_packet_from(s->demuxer->packet_pool, dst, dst_len);
    talloc_free(dst);
    if (!res)
        return dp;
    demux_packet_copy_attribs(res, dp);
    talloc_free(dp);
    return res;
}

static void emit(struct mp_dovi_convert *s, struct demux_packet *dp)
{
    if (s->num_out < (int)MP_ARRAY_SIZE(s->out)) {
        s->out[s->num_out++] = dp;
    } else {
        talloc_free(dp); // cannot happen: callers drain after every push
    }
}

static void drop_rpu(struct mp_dovi_convert *s, int i)
{
    talloc_free(s->rpu_q[i].nalu);
    MP_TARRAY_REMOVE_AT(s->rpu_q, s->num_rpu_q, i);
}

// Pair queued base-layer packets with converted RPUs, in arrival order.
static void pair(struct mp_dovi_convert *s, bool flush)
{
    while (s->num_bl_q) {
        struct demux_packet *bl = s->bl_q[0];
        int match = -1;
        bool later_rpu = false;
        for (int i = 0; i < s->num_rpu_q; i++) {
            double d = s->rpu_q[i].pts - bl->pts;
            if (fabs(d) <= PTS_TOLERANCE) {
                match = i;
                break;
            }
            if (d > PTS_TOLERANCE)
                later_rpu = true;
        }

        if (match >= 0) {
            struct held_rpu *r = &s->rpu_q[match];
            emit(s, rewrite_bl(s, bl, r->nalu, r->len));
            drop_rpu(s, match);
        } else if (flush || later_rpu || s->num_bl_q >= HOLD_MAX) {
            // Its EL twin is not coming: play the frame on the base layer.
            if (!s->warned_unpaired && !flush) {
                MP_WARN(s, "Dolby Vision: no enhancement-layer packet for pts %f; "
                        "frame emitted without RPU.\n", bl->pts);
                s->warned_unpaired = true;
            }
            emit(s, rewrite_bl(s, bl, NULL, 0));
        } else {
            break;
        }
        MP_TARRAY_REMOVE_AT(s->bl_q, s->num_bl_q, 0);

        // RPUs older than the newest emitted frame belong to frames already
        // gone.
        for (int i = 0; i < s->num_rpu_q;) {
            if (s->rpu_q[i].pts < bl->pts - PTS_TOLERANCE)
                drop_rpu(s, i);
            else
                i++;
        }
    }
}

void mp_dovi_convert_push_bl(struct mp_dovi_convert *s, struct demux_packet *dp)
{
    if (!s || !dp)
        return;
    if (!s->two_track) {
        emit(s, rewrite_bl(s, dp, NULL, 0));
        return;
    }
    if (s->num_bl_q >= HOLD_MAX)
        pair(s, true);
    s->bl_q[s->num_bl_q++] = dp;
    pair(s, false);
}

void mp_dovi_convert_push_el(struct mp_dovi_convert *s, struct demux_packet *dp)
{
    if (!s || !dp)
        return;
    const int nl = s->nal_len;
    size_t pos = 0;
    while (pos + nl <= dp->len) {
        uint32_t len = read_len(dp->buffer + pos, nl);
        if (len == 0 || pos + nl + len > dp->len)
            break;
        const uint8_t *nal = dp->buffer + pos + nl;
        if (((nal[0] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62) {
            size_t conv_len = 0;
            uint8_t *conv = convert_rpu(s, nal, len, &conv_len);
            if (conv) {
                if (s->num_rpu_q >= HOLD_MAX)
                    drop_rpu(s, 0);
                s->rpu_q[s->num_rpu_q++] = (struct held_rpu){
                    .pts = dp->pts, .nalu = conv, .len = conv_len,
                };
            }
            break;
        }
        pos += nl + len;
    }
    talloc_free(dp);
    pair(s, false);
}

struct demux_packet *mp_dovi_convert_pop(struct mp_dovi_convert *s)
{
    if (!s || !s->num_out)
        return NULL;
    struct demux_packet *dp = s->out[0];
    MP_TARRAY_REMOVE_AT(s->out, s->num_out, 0);
    return dp;
}

void mp_dovi_convert_flush(struct mp_dovi_convert *s)
{
    if (!s)
        return;
    pair(s, true);
    while (s->num_rpu_q)
        drop_rpu(s, 0);
}

void mp_dovi_convert_reset(struct mp_dovi_convert *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->num_bl_q; i++)
        talloc_free(s->bl_q[i]);
    s->num_bl_q = 0;
    while (s->num_rpu_q)
        drop_rpu(s, 0);
    for (int i = 0; i < s->num_out; i++)
        talloc_free(s->out[i]);
    s->num_out = 0;
}
