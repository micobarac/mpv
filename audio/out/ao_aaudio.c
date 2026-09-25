/*
 * AAudio audio output driver
 *
 * Copyright (C) 2024 Jun Bo Bi <jambonmcyeah@gmail.com>
 *
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

#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

#include <aaudio/AAudio.h>
#include <android/api-level.h>

#include "ao.h"
#include "audio/format.h"
#include "common/common.h"
#include "common/msg.h"
#include "internal.h"
#include "options/m_option.h"
#include "osdep/timer.h"

struct priv {
    AAudioStream *stream;

    int32_t device_id;
    aaudio_session_id_t session_id;
    int32_t buffer_capacity;
    aaudio_performance_mode_t performance_mode;

    _Atomic int64_t presented;
    _Atomic int64_t discarded;

    // Control publishes a new monotonic epoch before starting audio; only the
    // callback owns the cached timestamp. No allocation or callback-side lock.
    _Atomic int64_t timestamp_epoch;
    _Atomic int64_t reset_epoch;
    _Atomic int64_t paused_duration;
    int64_t pause_started; // control thread only
    int64_t callback_epoch;
    int64_t presented_time;
    // Media3 AudioTrackPositionTracker port (callback-owned). `trk_pos` is
    // the audio-time position last handed to the clock, `trk_time` the
    // active-playback time it was handed at. `pair_*` remember the last
    // accepted HAL pair for the rate diagnostic. All reset with the epoch.
    bool trk_valid;
    int64_t trk_pos, trk_time;
    bool pair_valid;
    int64_t pair_pos, pair_time;
    int32_t xruns;
    int32_t xruns_logged;
    int64_t last_pair_warn, last_bound_warn;

    int device_api;
    void *lib_handle;

#define AAUDIO_FUNCTION(name, ret, ...) ret (*name)(__VA_ARGS__);
#include "aaudio_functions26.inc"
#include "aaudio_functions28.inc"
#include "aaudio_functions32.inc"
#undef AAUDIO_FUNCTION
};

struct function_map {
    const char *symbol;
    int offset;
};

#define AAUDIO_FUNCTION(name, ret, ...) {#name, offsetof(struct priv, name)},
static const struct function_map lib_functions26[] = {
#include "aaudio_functions26.inc"
};

static const struct function_map lib_functions28[] = {
#include "aaudio_functions28.inc"
};

static const struct function_map lib_functions32[] = {
#include "aaudio_functions32.inc"
};
#undef AAUDIO_FUNCTION

static const struct {
    int api_level;
    int length;
    const struct function_map *functions;
} lib_functions[] = {
    {26, MP_ARRAY_SIZE(lib_functions26), lib_functions26},
    {28, MP_ARRAY_SIZE(lib_functions28), lib_functions28},
    {32, MP_ARRAY_SIZE(lib_functions32), lib_functions32}
};

/*
 * There is no documentation in AAudio for the order of positions for AAudio.
 * It's assumed to work the same way as AudioTrack (even the order of the bits
 * for the position mask is the same for both)
 * See https://developer.android.com/reference/android/media/AudioFormat#channelPositionMask
 */
static const struct mp_chmap aaudio_default_chmaps[] = {
    {0},                                                                                /* empty */
    MP_CHMAP_INIT_MONO,                                                                 /* mono */
    MP_CHMAP_INIT_STEREO,                                                               /* stereo */
    MP_CHMAP3(FL, FR, FC),                                                              /* 3.0 */
    MP_CHMAP4(FL, FR, BL, BR),                                                          /* quad */
    MP_CHMAP5(FL, FR, FC, BL, BR),                                                      /* 5.0 */
    MP_CHMAP6(FL, FR, FC, LFE, BL, BR),                                                 /* 5.1 */
    MP_CHMAP7(FL, FR, FC, LFE, BL, BR, BC),                                             /* 6.1 */
    MP_CHMAP8(FL, FR, FC, LFE, BL, BR, SL, SR),                                         /* 7.1 */
    {0},
    MP_CHMAP10(FL, FR, FC, LFE, BL, BR, SL, SR, TSL, TSR),                              /* 7.1.2 */
    {0},
    MP_CHMAP12(FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR),                    /* 7.1.4 */
    {0},
    MP_CHMAP14(FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR, WL, WR),            /* 9.1.4 */
    {0},
    MP_CHMAP16(FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR, TSL, TSR, WL, WR)   /* 9.1.6 */
};

static const struct mp_chmap aaudio_chmaps[] = {
    {0},                                                                                /* empty */
    /*
     * This should be `{1, {MP_SP(FL)}}` according to spec
     * but `mp_chmap_sel` doesn't like it
     */
    MP_CHMAP_INIT_MONO,                                                                 /* mono */
    MP_CHMAP_INIT_STEREO,                                                               /* stereo */
    MP_CHMAP3(FL, FR, LFE),                                                             /* 2.1 */
    MP_CHMAP3(FL, FR, FC),                                                              /* 3.0 */
    MP_CHMAP3(FL, FR, BC),                                                              /* 3.0 (back) */
    MP_CHMAP4(FL, FR, FC, LFE),                                                         /* 3.1 */
    MP_CHMAP4(FL, FR, TSL, TSR),                                                        /* 2.0.2 */
    MP_CHMAP5(FL, FR, LFE, TSL, TSR),                                                   /* 2.1.2 */
    MP_CHMAP5(FL, FR, FC, TSL, TSR),                                                    /* 3.0.2 */
    MP_CHMAP6(FL, FR, FC, LFE, TSL, TSR),                                               /* 3.1.2 */
    MP_CHMAP4(FL, FR, BL, BR),                                                          /* quad */
    MP_CHMAP4(FL, FR, SL, SR),                                                          /* quad (side) */
    MP_CHMAP4(FL, FR, FC, BC),                                                          /* quad (center) */
    MP_CHMAP5(FL, FR, FC, BL, BR),                                                      /* 5.0 */
    MP_CHMAP6(FL, FR, FC, LFE, BL, BR),                                                 /* 5.1 */
    MP_CHMAP6(FL, FR, FC, LFE, SL, SR),                                                 /* 5.1 (side) */
    MP_CHMAP7(FL, FR, FC, LFE, BL, BR, BC),                                             /* 6.1 */
    MP_CHMAP8(FL, FR, FC, LFE, BL, BR, SL, SR),                                         /* 7.1 */
    MP_CHMAP8(FL, FR, FC, LFE, BL, BR, TSL, TSR),                                       /* 5.1.2 */
    MP_CHMAP10(FL, FR, FC, LFE, BL, BR, TFL, TFR, TBL, TBR),                            /* 5.1.4 */
    MP_CHMAP10(FL, FR, FC, LFE, BL, BR, SL, SR, TSL, TSR),                              /* 7.1.2 */
    MP_CHMAP12(FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR),                    /* 7.1.4 */
    MP_CHMAP14(FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR, WL, WR),            /* 9.1.4 */
    MP_CHMAP16(FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR, TSL, TSR, WL, WR)   /* 9.1.6 */
};

static const aaudio_channel_mask_t aaudio_masks[] = {
    AAUDIO_CHANNEL_INVALID,
    AAUDIO_CHANNEL_MONO,
    AAUDIO_CHANNEL_STEREO,
    AAUDIO_CHANNEL_2POINT1,
    AAUDIO_CHANNEL_TRI,
    AAUDIO_CHANNEL_TRI_BACK,
    AAUDIO_CHANNEL_3POINT1,
    AAUDIO_CHANNEL_2POINT0POINT2,
    AAUDIO_CHANNEL_2POINT1POINT2,
    AAUDIO_CHANNEL_3POINT0POINT2,
    AAUDIO_CHANNEL_3POINT1POINT2,
    AAUDIO_CHANNEL_QUAD,
    AAUDIO_CHANNEL_QUAD_SIDE,
    AAUDIO_CHANNEL_SURROUND,
    AAUDIO_CHANNEL_PENTA,
    AAUDIO_CHANNEL_5POINT1,
    AAUDIO_CHANNEL_5POINT1_SIDE,
    AAUDIO_CHANNEL_6POINT1,
    AAUDIO_CHANNEL_7POINT1,
    AAUDIO_CHANNEL_5POINT1POINT2,
    AAUDIO_CHANNEL_5POINT1POINT4,
    AAUDIO_CHANNEL_7POINT1POINT2,
    AAUDIO_CHANNEL_7POINT1POINT4,
    AAUDIO_CHANNEL_9POINT1POINT4,
    AAUDIO_CHANNEL_9POINT1POINT6
};

static_assert(MP_ARRAY_SIZE(aaudio_chmaps) == MP_ARRAY_SIZE(aaudio_masks),
    "`aaudio_masks` and `aaudio_chmaps` MUST have the same number of entries.");

/*
 * The values corresponds to indexes of `aaudio_chmaps`
 *
 * Different devices may support different number of max channels
 * depending on what they set `FCC_LIMIT` to
 * See https://cs.android.com/android/platform/superproject/+/android-latest-release:system/media/audio/include/system/audio.h;l=234
 */
static const uint8_t aaudio_max_chnums[] = {
    MP_ARRAY_SIZE(aaudio_chmaps),
    // Kodi 21.2 AESinkAUDIOTRACK.cpp:564-596 retries supported layouts before
    // failing. These are exclusive map counts: include the last valid layout
    // for each Android channel limit, including stereo and mono.
    23, /* FCC_12: through 7.1.4 */
    20, /* FCC_8: through 5.1.2 */
    3,  /* FCC_2: through stereo */
    2   /* FCC_1: through mono */
};

static bool load_lib_functions(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->device_api = android_get_device_api_level();
    p->lib_handle = dlopen("libaaudio.so", RTLD_NOW | RTLD_GLOBAL);
    if (!p->lib_handle)
        return false;

    for (int i = 0; i < MP_ARRAY_SIZE(lib_functions); i++) {
        if (p->device_api < lib_functions[i].api_level)
            break;

        for (int j = 0; j < lib_functions[i].length; j++) {
            const char *sym = lib_functions[i].functions[j].symbol;
            void *fun = dlsym(p->lib_handle, sym);
            if (!fun)
                fun = dlsym(RTLD_DEFAULT, sym);
            if (!fun) {
                MP_WARN(ao, "Could not resolve symbol %s\n", sym);
                return false;
            }
            *(void **)((uint8_t *)p + lib_functions[i].functions[j].offset) = fun;
        }
    }
    return true;
}

static void error_callback(AAudioStream *stream, void *context, aaudio_result_t error)
{
    struct ao *ao = context;
    struct priv *p = ao->priv;

    MP_ERR(ao, "%s, trying to reload...\n", p->AAudio_convertResultToText(error));
    ao_request_reload(ao);
}

static int64_t monotonic_time_ns(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return MP_TIME_S_TO_NS(ts.tv_sec) + ts.tv_nsec;
}

static aaudio_data_callback_result_t data_callback(AAudioStream *stream, void *context,
                                                   void *data, int32_t nframes)
{
    struct ao *ao = context;
    struct priv *p = ao->priv;

    aaudio_result_t result;
    int64_t presented, present_time;
    int64_t written = p->AAudioStream_getFramesWritten(stream);
    int64_t epoch = atomic_load(&p->timestamp_epoch);
    if (epoch != p->callback_epoch) {
        if (atomic_load(&p->reset_epoch) > p->callback_epoch)
            p->presented_time = 0;
        p->callback_epoch = epoch;
        // Media3 AudioTrackPositionTracker.java:253-257 and
        // AudioTimestampPoller.reset(): a start, resume or flush is a
        // discontinuity; the next report is taken as is.
        p->trk_valid = false;
        p->pair_valid = false;
    }

    result = p->AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC,
                                         &presented, &present_time);
    int64_t now = monotonic_time_ns();
    int64_t paused_duration = atomic_load(&p->paused_duration);
    bool warn_ok = now - p->last_pair_warn > MP_TIME_S_TO_NS(1);
    if (result < 0) {
        MP_TRACE(ao, "AAudioStream_getTimestamp() returned %s\n",
                 p->AAudio_convertResultToText(result));
    } else if (presented >= 0 && presented <= written &&
               present_time > 0 && present_time >= epoch && present_time <= now) {
        int64_t active_time = present_time - paused_duration;
        // Diagnostics only. VLC clock.c:37,436-454: the rate between two
        // clock points must stay within 0.8..1.2 of real time, or the
        // source is not trusted. Media3 AudioTimestampPoller.java:332-339:
        // a pair more than 5 s from the device's own read counter is
        // spurious. Neither changes the clock here; both name the pair.
        if (p->pair_valid && active_time > p->pair_time && warn_ok) {
            int64_t dpos_ns = MP_TIME_S_TO_NS(presented - p->pair_pos) / ao->samplerate;
            int64_t rate_permille = dpos_ns * 1000 / (active_time - p->pair_time);
            if (rate_permille < 800 || rate_permille > 1200) {
                p->last_pair_warn = now; warn_ok = false;
                MP_WARN(ao, "timestamp rate %" PRId64 "/1000 of real time: presented=%" PRId64
                        " prev=%" PRId64 " dt_ms=%" PRId64 "\n", rate_permille, presented,
                        p->pair_pos, (active_time - p->pair_time) / 1000000);
            }
        }
        int64_t device_read = p->AAudioStream_getFramesRead(stream);
        int64_t read_gap = presented > device_read ? presented - device_read
                                                   : device_read - presented;
        if (warn_ok && read_gap > 5 * (int64_t)ao->samplerate) {
            p->last_pair_warn = now; warn_ok = false;
            MP_WARN(ao, "timestamp %" PRId64 " disagrees with frames read %" PRId64 "\n",
                    presented, device_read);
        }
        p->pair_pos = presented;
        p->pair_time = active_time;
        p->pair_valid = true;
        p->presented = presented;
        p->presented_time = active_time;
    }
    int32_t xruns = p->AAudioStream_getXRunCount(stream);
    if (xruns != p->xruns) {
        // An underrun is a real discontinuity: the device played silence and
        // its position jumped by that much. Take the next report unbounded
        // instead of converging on it at 10 % of real time through the clamp
        // below (a 200 ms underrun otherwise stayed in the delay for ~2 s).
        if (xruns > p->xruns)
            p->trk_valid = false;
        p->xruns = xruns;
        // Warn at most once a second, like the pair diagnostics above: this
        // runs on the real-time callback and mp_msg takes the log locks.
        if (warn_ok) {
            p->last_pair_warn = now;
            warn_ok = false;
            MP_WARN(ao, "device underrun count %" PRId32 " -> %" PRId32 "\n",
                    p->xruns_logged, xruns);
            p->xruns_logged = xruns;
        }
    }

    // Kodi 21.2 AESinkAUDIOTRACK.cpp:703-741 advances the timestamp's
    // frame position by elapsed monotonic time; :675-681 bounds consumed
    // output. AAudio supplies the same position/time pair. A failed query
    // retains that pair, not a frozen position dated as if it were new.
    // Keep the cached timestamp in active-playback time. Subtract only paused
    // wall time on resume, preserving extrapolation already consumed before
    // pause even when timestamp queries fail across multiple pause cycles.
    // Before the first valid timestamp, anchor the existing position once.
    int64_t active_now = now - paused_duration;
    if (!p->presented_time)
        p->presented_time = active_now;
    int64_t pending = written - (atomic_load(&p->presented) +
                                 atomic_load(&p->discarded));
    int64_t delay = MP_TIME_S_TO_NS(MPMAX(pending, 0)) / ao->samplerate;
    delay = MPMAX(delay - MPMAX(active_now - p->presented_time, 0), 0);
    // Media3 AudioTrackPositionTracker.java:268-291 (getCurrentPositionUs):
    // the position handed to the clock may leave the previous report's
    // prediction by at most 10 % of the elapsed time
    // (MAX_POSITION_SMOOTHING_SPEED_CHANGE_PERCENT, :115), unless it has
    // not moved at all (a stalled device must be seen) or is more than 1 s
    // away (MAX_POSITION_DRIFT_FOR_SMOOTHING_US, :112: a discontinuity
    // must be seen). player/video.c:636 copies this delay straight into
    // the frame wait, so one spurious HAL pair otherwise becomes a burst
    // of dropped frames followed by a hold of the same length. The
    // position is audio time: frames written and not flushed, minus the
    // delay. A healthy stream never leaves the window and is untouched.
    // Tracking arms only once a HAL pair has been accepted in this epoch:
    // until then the clock runs on the anchor, and `discarded` after a
    // flush comes from a cached pair up to one callback old, so the first
    // real pair after start, resume or seek carries a legitimate correction
    // (Media3 AudioTimestampPoller: no smoothing before an accepted stamp).
    int64_t out_ns = MP_TIME_S_TO_NS(written - atomic_load(&p->discarded)) / ao->samplerate;
    int64_t pos = out_ns - delay;
    if (p->trk_valid && active_now > p->trk_time && pos != p->trk_pos) {
        int64_t elapsed = active_now - p->trk_time;
        int64_t expected = p->trk_pos + elapsed;
        int64_t drift = pos > expected ? pos - expected : expected - pos;
        if (drift < MP_TIME_S_TO_NS(1)) {
            int64_t max_drift = elapsed * 10 / 100;
            int64_t bounded = MPCLAMP(pos, expected - max_drift, expected + max_drift);
            if (bounded != pos) {
                // Log only corrections the line can show: the printout is
                // whole milliseconds, and the 10 % window trips on the HAL's
                // sub-millisecond timestamp jitter all through steady
                // playback (50-95 "+0 ms" lines per session, 2026-09-15).
                // The clock is bounded either way.
                if (llabs(bounded - pos) >= MP_TIME_MS_TO_NS(1) &&
                    now - p->last_bound_warn > MP_TIME_S_TO_NS(1)) {
                    p->last_bound_warn = now;
                    MP_WARN(ao, "position bounded: reported %+" PRId64 " ms from prediction, "
                            "passed %+" PRId64 " ms\n", (pos - expected) / 1000000,
                            (bounded - expected) / 1000000);
                }
                pos = bounded;
                delay = MPMAX(out_ns - pos, 0);
            }
        }
    } else if (p->trk_valid && pos == p->trk_pos &&
               now - p->last_bound_warn > MP_TIME_S_TO_NS(1) &&
               pending > 2 * (int64_t)ao->device_buffer) {
        // Kodi PR 24729 (superviseaudiodelay): a sink whose position does
        // not move while more than twice its buffer has been fed is stuck.
        // Reported only; the stalled position is passed through above.
        p->last_bound_warn = now;
        MP_WARN(ao, "device position stuck at %" PRId64 " with %" PRId64 " frames pending\n",
                pos / 1000000, pending);
    }
    p->trk_pos = pos;
    p->trk_time = active_now;
    p->trk_valid = p->pair_valid;
    // mpv may use MONOTONIC_RAW with a different epoch. Convert the remaining
    // duration, never an absolute CLOCK_MONOTONIC timestamp, to its clock.
    int64_t end_time = mp_time_ns() + delay +
                       MP_TIME_S_TO_NS(nframes) / ao->samplerate;

    ao_read_data(ao, &data, nframes, end_time, NULL, true, true);

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->stream) {
        p->AAudioStream_close(p->stream);
        p->stream = NULL;
    }

    if (p->lib_handle) {
        dlclose(p->lib_handle);
        p->lib_handle = NULL;
    }
}

static int init(struct ao *ao)
{
    if (!load_lib_functions(ao))
        return -1;

    struct priv *p = ao->priv;

    aaudio_result_t result;
    AAudioStreamBuilder *builder;

    if ((result = p->AAudio_createStreamBuilder(&builder)) < 0) {
        MP_ERR(ao, "AAudio_createStreamBuilder() returned %s\n",
               p->AAudio_convertResultToText(result));
        return -1;
    }

    aaudio_format_t format;

    if (p->device_api >= 34 && af_fmt_is_spdif(ao->format)) {
        format = AAUDIO_FORMAT_IEC61937;
    } else if (af_fmt_is_float(ao->format)) {
        ao->format = AF_FORMAT_FLOAT;
        format = AAUDIO_FORMAT_PCM_FLOAT;
    } else if (af_fmt_is_int(ao->format)) {
        if (af_fmt_to_bytes(ao->format) > 2 && p->device_api >= 31) {
            ao->format = AF_FORMAT_S32;
            format = AAUDIO_FORMAT_PCM_I32;
        } else {
            ao->format = AF_FORMAT_S16;
            format = AAUDIO_FORMAT_PCM_I16;
        }
    } else {
        ao->format = AF_FORMAT_S16;
        format = AAUDIO_FORMAT_PCM_I16;
    }

    p->AAudioStreamBuilder_setDeviceId(builder, p->device_id);
    p->AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    p->AAudioStreamBuilder_setSharingMode(builder,
                                          (ao->init_flags & AO_INIT_EXCLUSIVE)
                                              ? AAUDIO_SHARING_MODE_EXCLUSIVE
                                              : AAUDIO_SHARING_MODE_SHARED);
    p->AAudioStreamBuilder_setFormat(builder, format);
    p->AAudioStreamBuilder_setSampleRate(builder, ao->samplerate);
    p->AAudioStreamBuilder_setErrorCallback(builder, error_callback, ao);
    p->AAudioStreamBuilder_setBufferCapacityInFrames(builder,
                                                     p->buffer_capacity);
    p->AAudioStreamBuilder_setPerformanceMode(builder, p->performance_mode);
    p->AAudioStreamBuilder_setDataCallback(builder, data_callback, ao);

    if (p->device_api >= 28) {
        if (ao->set_media_role)
            p->AAudioStreamBuilder_setContentType(builder,
                                                  (ao->init_flags &
                                                   AO_INIT_MEDIA_ROLE_MUSIC)
                                                      ? AAUDIO_CONTENT_TYPE_MUSIC
                                                      : AAUDIO_CONTENT_TYPE_MOVIE);
        p->AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
        p->AAudioStreamBuilder_setSessionId(builder, p->session_id);
    }

    if (p->device_api >= 32) {
        uint8_t i = 0;
        struct mp_chmap channels = ao->channels;

        do {
            struct mp_chmap_sel sel = {0, .tmp = p};
            aaudio_channel_mask_t channel_mask = AAUDIO_CHANNEL_INVALID;

            for (int j = 1; j < aaudio_max_chnums[i]; j++)
                mp_chmap_sel_add_map(&sel, &aaudio_chmaps[j]);

            if (!ao_chmap_sel_adjust(ao, &sel, &channels)) {
                MP_ERR(ao, "Failed to find channel map\n");
                goto err;
            }

            for (uint8_t j = 0; j < aaudio_max_chnums[i]; j++) {
                if (mp_chmap_equals(&aaudio_chmaps[j], &channels)) {
                    channel_mask = aaudio_masks[j];
                    break;
                }
            }

            assert(channel_mask != AAUDIO_CHANNEL_INVALID);
            p->AAudioStreamBuilder_setChannelMask(builder, channel_mask);

            result = p->AAudioStreamBuilder_openStream(builder, &p->stream);

            if (result != AAUDIO_ERROR_OUT_OF_RANGE) {
                break;
            }
        // Kodi AESinkAUDIOTRACK.cpp:594-596 fails after supported fallbacks;
        // test the next index before accessing another candidate.
        } while (++i < MP_ARRAY_SIZE(aaudio_max_chnums));

        ao->channels = channels;
    } else {
        result = p->AAudioStreamBuilder_openStream(builder, &p->stream);
    }

    if (result < 0) {
        MP_ERR(ao, "AAudioStreamBuilder_openStream() returned %s\n",
               p->AAudio_convertResultToText(result));
        goto err;
    }

    if (p->device_api < 32) {
        int32_t channel_count = p->AAudioStream_getChannelCount(p->stream);

        if (channel_count >= MP_ARRAY_SIZE(aaudio_default_chmaps) ||
            (ao->channels = aaudio_default_chmaps[channel_count]).num == 0) {
            MP_ERR(ao, "Unknown layout for channel count: %" PRId32 "\n",
                   channel_count);
            goto err;
        }
    }

    ao->device_buffer = p->AAudioStream_getBufferCapacityInFrames(p->stream);

    p->AAudioStreamBuilder_delete(builder);
    return 1;
err:
    p->AAudioStreamBuilder_delete(builder);
    return -1;
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    aaudio_result_t result;
    // Frames still queued in a PAUSED stream are played after
    // requestStart(), so they must stay in the delay estimate; only a
    // flushed or stopped stream has dropped them. mpv reaches start()
    // on a paused stream after an underrun: buffer.c clears `playing`,
    // the cache-pause resume in ao_set_paused() therefore skips
    // set_pause(false), and player/audio.c restarts via ao_start()
    // instead. Counting the retained buffer (up to the stream's
    // capacity) as discarded made the reported delay that much too
    // small, the video looked late by the same amount and was dropped
    // frame after frame on a real-time-paced decoder.
    aaudio_stream_state_t state = p->AAudioStream_getState(p->stream);
    int64_t now = monotonic_time_ns();
    if (state != AAUDIO_STREAM_STATE_PAUSED && state != AAUDIO_STREAM_STATE_PAUSING) {
        p->discarded = p->AAudioStream_getFramesWritten(p->stream) - p->presented;
        // Kodi AESinkAUDIOTRACK.cpp:988-1008 resets timestamp state on drain.
        atomic_store(&p->reset_epoch, now);
    }
    if (p->pause_started) {
        atomic_fetch_add(&p->paused_duration, now - p->pause_started);
        p->pause_started = 0;
    }
    atomic_store(&p->timestamp_epoch, now);
    if ((result = p->AAudioStream_requestStart(p->stream)) < 0) {
        MP_ERR(ao, "AAudioStream_requestStart() returned %s\n",
               p->AAudio_convertResultToText(result));
        return ao_request_reload(ao);
    }
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    int64_t now = monotonic_time_ns();
    if (paused) {
        if (!p->pause_started)
            p->pause_started = now;
    } else {
        if (p->pause_started) {
            atomic_fetch_add(&p->paused_duration, now - p->pause_started);
            p->pause_started = 0;
        }
        atomic_store(&p->timestamp_epoch, now);
    }
    aaudio_result_t result = paused
                    ? p->AAudioStream_requestPause(p->stream)
                    : p->AAudioStream_requestStart(p->stream);

    if (result < 0) {
        MP_ERR(ao, "AAudioStream_request%s() returned %s\n",
               paused ? "Pause" : "Start",
               p->AAudio_convertResultToText(result));
        ao_request_reload(ao);
        return false;
    }

    return true;
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    aaudio_result_t result;
    aaudio_stream_state_t state = p->AAudioStream_getState(p->stream);

    switch (state) {
        case AAUDIO_STREAM_STATE_STARTING:
        case AAUDIO_STREAM_STATE_STOPPING:
        case AAUDIO_STREAM_STATE_PAUSING:
        case AAUDIO_STREAM_STATE_FLUSHING:
            if ((result = p->AAudioStream_waitForStateChange(p->stream,
                                state, &state, INT64_MAX)) < 0) {
                MP_ERR(ao, "AAudioStream_waitForStateChange() returned %s\n",
                       p->AAudio_convertResultToText(result));
                return ao_request_reload(ao);
            }
    }

    if (state != AAUDIO_STREAM_STATE_PAUSED) {
        if ((result = p->AAudioStream_requestPause(p->stream)) < 0) {
            MP_ERR(ao, "AAudioStream_requestPause() %s\n",
                   p->AAudio_convertResultToText(result));
            return ao_request_reload(ao);
        }

        if ((result = p->AAudioStream_waitForStateChange(p->stream,
                            AAUDIO_STREAM_STATE_PAUSING, &state, INT64_MAX)) < 0) {
            MP_ERR(ao, "AAudioStream_waitForStateChange() returned %s\n",
                   p->AAudio_convertResultToText(result));
            return ao_request_reload(ao);
        }
    }

    if ((result = p->AAudioStream_requestFlush(p->stream)) < 0) {
        MP_ERR(ao, "AAudioStream_requestFlush() returned %s\n",
               p->AAudio_convertResultToText(result));
        return ao_request_reload(ao);
    }
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_aaudio = {
    .description = "AAudio audio output",
    .name = "aaudio",
    .init = init,
    .uninit = uninit,
    .start = start,
    .reset = reset,
    .set_pause = set_pause,

    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .device_id = AAUDIO_UNSPECIFIED,
        .session_id = AAUDIO_SESSION_ID_NONE,
        .buffer_capacity = AAUDIO_UNSPECIFIED,
        .performance_mode = AAUDIO_PERFORMANCE_MODE_NONE,
        .presented = 0,
        .discarded = 0,
        .stream = NULL,
        .lib_handle = NULL
    },
    .options_prefix = "aaudio",
    .options =
        (const struct m_option[]){
            {"device-id", OPT_CHOICE(device_id,
                {"auto", AAUDIO_UNSPECIFIED}),
                M_RANGE(INT32_MIN, INT32_MAX)},
            {"session-id", OPT_CHOICE(session_id,
                {"none", AAUDIO_SESSION_ID_NONE}),
                M_RANGE(INT32_MIN, INT32_MAX)},
            {"buffer-capacity", OPT_CHOICE(buffer_capacity,
                {"auto", AAUDIO_UNSPECIFIED}),
                M_RANGE(1, INT32_MAX)},
            {"performance-mode", OPT_CHOICE(performance_mode,
                {"none", AAUDIO_PERFORMANCE_MODE_NONE},
                {"low-latency", AAUDIO_PERFORMANCE_MODE_LOW_LATENCY},
                {"power-saving", AAUDIO_PERFORMANCE_MODE_POWER_SAVING})},
            {0}},
};
