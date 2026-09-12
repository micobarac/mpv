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

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include "osdep/threads.h"

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "common/common.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "sub/draw_bmp.h"
#include "sub/osd.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"

// Subtitle bridge: function pointers registered by Rust at process
// init via the exported `torro_register_subtitle_callbacks()` below.
// Indirection is required because Android `System.loadLibrary` uses
// RTLD_LOCAL — `libmpv.so` cannot resolve symbols from the Rust
// cdylib (`libtorro_player_lib.so`) at link or load time. Rust calls
// in once to install the pointers; the VO then dispatches frames
// through them.
//
// `present` hands a packed premultiplied BGRA atlas + per-part rect
// table over to the Kotlin `SubtitleView` (a regular Android `View`,
// NOT a second SurfaceView) via JNI on the calling thread. The View
// holds the atlas as an ARGB_8888 Bitmap and re-draws it inside its
// own `onDraw` with a scale matrix mapping canvas (= video frame)
// coords to view (= on-screen video rect) coords.
//
// Single-SurfaceView constraint: TCL Realtek RHWC2 advertises
// `max layers: 1`. A second SurfaceView for subs would exceed the
// HWC plane budget and force GLES composition of the video plane,
// which stalls MediaCodec output buffers on this SoC. A regular View
// renders into the activity window (one shared compositor surface),
// outside HWC's per-layer plane count for the video Surface — the
// video keeps its dedicated overlay plane.
//
// `parts` layout: 8 int32 per part, in this order:
//   src_x, src_y, w, h, dst_x, dst_y, dst_w, dst_h
// (src is into the atlas; dst is in canvas coords.)
typedef void (*torro_subs_present_fn)(
    int32_t canvas_w, int32_t canvas_h,
    int32_t atlas_w,  int32_t atlas_h, int32_t atlas_stride,
    const uint8_t *atlas_bgra,  // borrowed through this synchronous callback
    int32_t num_parts,
    const int32_t *parts);
typedef void (*torro_subs_clear_fn)(void);

// How far ahead of its display time a decoded frame is handed to the
// compositor with a presentation timestamp
// (`av_mediacodec_render_buffer_at_time`). The compositor then shows
// it at the right vsync no matter what this process is doing in the
// meantime. Media3 measured the same fix on MediaTek A53 TV SoCs
// (androidx/media #2990): frame drops 0.139% -> 0.008% once frames
// could go out up to ~200 ms early, ~150 ms observed in practice;
// Kodi releases 1.5 vsyncs early on top of a 4-frame queue.
#define TORRO_EARLY_RELEASE_NS MP_TIME_MS_TO_NS(150)

static torro_subs_present_fn g_subs_present;
static torro_subs_clear_fn   g_subs_clear;
static torro_subs_clear_fn   g_subs_invalidate;

// SubtitleView's on-screen pixel dimensions. Rust pushes via the
// exported `torro_set_subtitle_canvas` setter when Kotlin's
// `SubtitleView.onSizeChanged` fires. Used as the OSD canvas for
// `osd_draw` so mpv's `sub-font-size` (which scales as
// size * canvas_h / 720) and `sub-pos` (canvas-relative %) match the
// effective on-screen rendering size — same contract Kodi's
// CGUITextLayout uses against the on-screen window. Zero = not yet
// reported; fall back to video frame dims so the first few frames
// don't render with a zero-sized canvas.
static atomic_int g_canvas_w;
static atomic_int g_canvas_h;

// Forward prototype with the visibility attribute so the symbol is
// exported from libmpv.so even though `gnu_symbol_visibility=hidden`.
__attribute__((visibility("default")))
void torro_register_subtitle_callbacks(
    torro_subs_present_fn present,
    torro_subs_clear_fn   clear,
    torro_subs_clear_fn   invalidate);

void torro_register_subtitle_callbacks(
    torro_subs_present_fn present,
    torro_subs_clear_fn   clear,
    torro_subs_clear_fn   invalidate)
{
    g_subs_present = present;
    g_subs_clear   = clear;
    g_subs_invalidate = invalidate;
}

__attribute__((visibility("default")))
void torro_set_subtitle_canvas(int width, int height);

void torro_set_subtitle_canvas(int width, int height)
{
    atomic_store(&g_canvas_w, width  > 0 ? width  : 0);
    atomic_store(&g_canvas_h, height > 0 ? height : 0);
}

// BEGIN SUBTITLE WORKER TYPES
struct subtitle_request {
    double pts;
    struct mp_osd_res res;
    uint64_t epoch;
};

struct subtitle_worker {
    mp_thread thread;
    mp_mutex lock;          // metadata only; never held while rendering/copying
    mp_mutex publish_lock;  // serializes publication with reset, not video release
    mp_cond wakeup;
    bool started, stop, pending, have_request;
    uint64_t epoch;
    struct subtitle_request request;
};
// END SUBTITLE WORKER TYPES

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;

    struct subtitle_worker subs;
    // Display time (mp_time ns) of `next_image`, from `vo_frame.pts`.
    // 0 for redraws, which release immediately.
    int64_t next_display_ns;
    // Diagnostic timestamps only: no extra frame/pixel buffering. Kodi 21.2
    // RendererMediaCodecSurface.cpp:107-121 and RenderManager.cpp:700-723
    // keep Surface release and overlay rendering as distinct stages. Measure
    // these boundaries without changing their ordering or presentation time.
    int64_t next_draw_ns;
    int64_t next_duration_ns;
    int64_t last_slow_log_ns;
    // Frames handed to the compositor but not yet on screen: their
    // display time and media pts. Subtitles are rendered for the frame
    // that is on screen NOW, not the one just released 150 ms early —
    // see flip_page. Kodi RendererMediaCodecSurface-21.2.cpp:107-121 and
    // RenderManager-21.2.cpp:719-723 separate release from overlay time.
    // mpv adaptation: 150 ms at 60 Hz needs ten pending timestamps;
    // sixteen bounded metadata entries include the currently submitted frame.
    // No additional codec buffers, surfaces, or presentation lead.
    struct { int64_t display_ns; double pts; } in_flight[16];
    int in_flight_n;
    // Media pts of the frame currently on screen, -1 before the first.
    double shown_pts;
};

// Translate an mp_time instant to Android's CLOCK_MONOTONIC (what
// MediaCodec render timestamps and `SystemClock.uptimeMillis` use).
// mp_time may run on CLOCK_MONOTONIC_RAW, so convert via "now" on
// both clocks rather than assuming a fixed offset.
static int64_t mono_ns_for(int64_t mp_ns)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t mono_now = (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
    return mono_now + (mp_ns - mp_time_ns());
}

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    mp_assert(vo->opts->WinID != 0 && vo->opts->WinID != -1);
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

// BEGIN SUBTITLE WORKER FUNCTIONS
// Kodi 21.2 RendererMediaCodecSurface.cpp:107-121 and RenderManager.cpp:702-723
// separate video release from GUI-overlay rendering. mpv adaptation: one worker
// performs osd_render and the synchronous Rust mailbox copy, with one pending metadata
// request and one active bitmap list. No codec frames or future pixmaps are queued.
static mp_static_mutex g_subtitle_registry = MP_STATIC_MUTEX_INITIALIZER;
static struct priv *g_subtitle_active;

struct subtitle_frame {
    int w, h, stride, num_parts;
    const uint8_t *pixels;
    uint8_t *owned_pixels;
    int32_t *parts;
    size_t pixels_capacity, parts_capacity;
};

static void subtitle_frame_free(struct subtitle_frame *frame)
{
    free(frame->owned_pixels);
    free(frame->parts);
}

// Kodi OverlayRenderer-21.2.cpp:141-157 renders every overlay in the frame.
// Preserve primary + secondary subtitles as one presentation. The common single
// atlas case borrows mpv's pixels/stride through the callback; Rust copies before
// returning. Multiple atlases are stacked once, with all source rects adjusted.
static bool subtitle_frame_prepare(struct sub_bitmap_list *list,
                                   struct subtitle_frame *out)
{
    size_t count = 0;
    int width = 0, height = 0;
    for (int i = 0; i < list->num_items; i++) {
        struct sub_bitmaps *item = list->items[i];
        if (item->format != SUBBITMAP_BGRA || !item->packed ||
            item->num_parts < 0 || item->packed_w <= 0 || item->packed_h <= 0 ||
            item->packed_w > INT_MAX / 4 ||
            item->packed->stride[0] < item->packed_w * 4 ||
            height > INT_MAX - item->packed_h ||
            count > INT_MAX - item->num_parts)
            return false;
        width = MPMAX(width, item->packed_w);
        height += item->packed_h;
        count += item->num_parts;
    }
    if (!count || count > SIZE_MAX / (8 * sizeof(int32_t)))
        return false;
    // Kodi OverlayRenderer-21.2.cpp:510-535 reuses render resources. Keep one
    // worker-owned metadata allocation and (only for multiple atlases) scratch;
    // capacity grows to the largest required byte count, never per-cue history.
    if (count > out->parts_capacity) {
        int32_t *parts = realloc(out->parts, count * 8 * sizeof(int32_t));
        if (!parts)
            return false;
        out->parts = parts;
        out->parts_capacity = count;
    }
    out->w = width;
    out->h = height;
    out->num_parts = count;
    if (list->num_items == 1) {
        out->stride = list->items[0]->packed->stride[0];
        out->pixels = list->items[0]->packed->planes[0];
    } else {
        out->stride = width * 4;
        if ((size_t)height > SIZE_MAX / out->stride)
            return false;
        size_t bytes = (size_t)height * out->stride;
        if (bytes > out->pixels_capacity) {
            uint8_t *pixels = realloc(out->owned_pixels, bytes);
            if (!pixels)
                return false;
            out->owned_pixels = pixels;
            out->pixels_capacity = bytes;
        }
        // Clear this atlas footprint, including unused row tails between items.
        memset(out->owned_pixels, 0, bytes);
        out->pixels = out->owned_pixels;
    }
    int y = 0, n = 0;
    for (int i = 0; i < list->num_items; i++) {
        struct sub_bitmaps *item = list->items[i];
        if (list->num_items > 1) {
            for (int row = 0; row < item->packed_h; row++)
                memcpy(out->owned_pixels + (size_t)(y + row) * out->stride,
                       item->packed->planes[0] + (size_t)row * item->packed->stride[0],
                       (size_t)item->packed_w * 4);
        }
        for (int j = 0; j < item->num_parts; j++) {
            struct sub_bitmap *part = &item->parts[j];
            if (part->src_x < 0 || part->src_y < 0 || part->w < 0 || part->h < 0 ||
                part->src_x > item->packed_w - part->w ||
                part->src_y > item->packed_h - part->h)
                return false;
            int32_t *r = &out->parts[(size_t)n++ * 8];
            r[0] = part->src_x; r[1] = part->src_y + y;
            r[2] = part->w; r[3] = part->h;
            r[4] = part->x; r[5] = part->y;
            r[6] = part->dw; r[7] = part->dh;
        }
        y += item->packed_h;
    }
    return true;
}

static MP_THREAD_VOID subtitle_thread(void *arg)
{
    struct vo *vo = arg;
    struct priv *p = vo->priv;
    struct subtitle_worker *worker = &p->subs;
    mp_thread_set_name("torro/subs");
    uint64_t cached_epoch = 0;
    int64_t last_change_id = 0;
    bool last_had_subs = false;
    struct subtitle_frame frame = {0};
    while (true) {
        mp_mutex_lock(&worker->lock);
        while (!worker->pending && !worker->stop)
            mp_cond_wait(&worker->wakeup, &worker->lock);
        if (worker->stop) {
            mp_mutex_unlock(&worker->lock);
            break;
        }
        struct subtitle_request request = worker->request;
        worker->pending = false;
        mp_mutex_unlock(&worker->lock);

        // mpv sub/osd.c:360-427 serializes renderer/decoder access and returns
        // an owned list. Never borrow vo->params: vo.c:604-613 replaces it before
        // reconfig. The request carries the exact canvas used for this render.
        bool formats[SUBBITMAP_COUNT] = { [SUBBITMAP_BGRA] = true };
        struct sub_bitmap_list *list = osd_render(vo->osd, request.res,
                                                 request.pts, OSD_DRAW_SUB_ONLY,
                                                 formats);
        if (!list)
            continue;
        // Kodi OverlayRenderer-21.2.cpp:510-535: reuse unchanged images. mpv's
        // list-level id (sub/osd.h:99-111) also tracks disappearing secondaries.
        if (cached_epoch == request.epoch && last_change_id == list->change_id) {
            talloc_free(list);
            continue;
        }
        bool empty = list->num_items == 0;
        bool ready = empty || subtitle_frame_prepare(list, &frame);
        mp_mutex_lock(&worker->publish_lock);
        mp_mutex_lock(&worker->lock);
        bool current = !worker->stop && request.epoch == worker->epoch;
        mp_mutex_unlock(&worker->lock);
        if (ready && current) {
            bool submitted = false;
            if (empty) {
                if (g_subs_clear && (last_had_subs || cached_epoch != request.epoch))
                    g_subs_clear();
                submitted = true;
            } else if (g_subs_present) {
                g_subs_present(list->w, list->h, frame.w, frame.h, frame.stride,
                               frame.pixels, frame.num_parts, frame.parts);
                submitted = true;
            }
            if (submitted) {
                cached_epoch = request.epoch;
                last_change_id = list->change_id;
                last_had_subs = !empty;
            }
        }
        mp_mutex_unlock(&worker->publish_lock);
        talloc_free(list);
    }
    subtitle_frame_free(&frame);
    MP_THREAD_RETURN();
}

static bool subtitle_worker_start(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct subtitle_worker *worker = &p->subs;
    if (mp_mutex_init(&worker->lock))
        return false;
    if (mp_mutex_init(&worker->publish_lock)) {
        mp_mutex_destroy(&worker->lock);
        return false;
    }
    if (mp_cond_init(&worker->wakeup)) {
        mp_mutex_destroy(&worker->publish_lock);
        mp_mutex_destroy(&worker->lock);
        return false;
    }
    worker->epoch = 1;
    if (mp_thread_create(&worker->thread, subtitle_thread, vo)) {
        mp_cond_destroy(&worker->wakeup);
        mp_mutex_destroy(&worker->publish_lock);
        mp_mutex_destroy(&worker->lock);
        return false;
    }
    worker->started = true;
    mp_mutex_lock(&g_subtitle_registry);
    g_subtitle_active = p;
    mp_mutex_unlock(&g_subtitle_registry);
    return true;
}

static void subtitle_submit(struct vo *vo, double pts)
{
    struct priv *p = vo->priv;
    if (!vo->params)
        return;
    int cw = atomic_load(&g_canvas_w), ch = atomic_load(&g_canvas_h);
    if (cw <= 0 || ch <= 0) {
        cw = vo->params->w;
        ch = vo->params->h;
    }
    struct subtitle_worker *worker = &p->subs;
    mp_mutex_lock(&worker->lock);
    if (!worker->stop) {
        worker->request = (struct subtitle_request){
            .pts = pts, .res = {.w = cw, .h = ch, .display_par = 1.0},
            .epoch = worker->epoch,
        };
        worker->pending = worker->have_request = true;
        mp_cond_signal(&worker->wakeup);
    }
    mp_mutex_unlock(&worker->lock);
}

// Kodi OverlayRenderer-21.2.cpp:85-111 flushes overlays/cache together. Epoch
// fencing prevents an in-progress old rasterization from resurrecting a clear.
// Only reset/option changes wait on publication; ordinary video releases do not.
static void subtitle_invalidate_request(struct priv *p, bool redraw)
{
    struct subtitle_worker *worker = &p->subs;
    mp_mutex_lock(&worker->publish_lock);
    mp_mutex_lock(&worker->lock);
    worker->epoch++;
    worker->pending = redraw && worker->have_request && !worker->stop;
    worker->request.epoch = worker->epoch;
    if (!redraw)
        worker->have_request = false;
    mp_cond_signal(&worker->wakeup);
    mp_mutex_unlock(&worker->lock);
    // The bridge also fences already queued Android presentations by generation.
    if (g_subs_invalidate)
        g_subs_invalidate();
    else if (g_subs_clear)
        g_subs_clear();
    mp_mutex_unlock(&worker->publish_lock);
}

static void subtitle_invalidate(struct priv *p)
{
    subtitle_invalidate_request(p, false);
}

// Called after subtitle track/style/visibility commands. Registry ownership
// fences VO teardown; render never needs this mutex or reenters the player API.
__attribute__((visibility("default")))
void torro_invalidate_subtitles(void);
void torro_invalidate_subtitles(void)
{
    mp_mutex_lock(&g_subtitle_registry);
    if (g_subtitle_active)
        subtitle_invalidate_request(g_subtitle_active, true);
    mp_mutex_unlock(&g_subtitle_registry);
}

static void subtitle_worker_stop(struct priv *p)
{
    struct subtitle_worker *worker = &p->subs;
    if (!worker->started)
        return;
    mp_mutex_lock(&g_subtitle_registry);
    if (g_subtitle_active == p)
        g_subtitle_active = NULL;
    mp_mutex_unlock(&g_subtitle_registry);
    // Serialize final publication with stop, then join without holding locks.
    mp_mutex_lock(&worker->publish_lock);
    mp_mutex_lock(&worker->lock);
    worker->stop = true;
    worker->pending = false;
    worker->epoch++;
    mp_cond_signal(&worker->wakeup);
    mp_mutex_unlock(&worker->lock);
    mp_mutex_unlock(&worker->publish_lock);
    mp_thread_join(worker->thread);
    if (g_subs_clear)
        g_subs_clear();
    mp_cond_destroy(&worker->wakeup);
    mp_mutex_destroy(&worker->publish_lock);
    mp_mutex_destroy(&worker->lock);
    worker->started = false;
}
// END SUBTITLE WORKER FUNCTIONS

// The frame on screen now is the newest in-flight frame whose display
// time has passed. Drops everything older than it.
static bool advance_shown_frame(struct priv *p)
{
    int64_t now = mp_time_ns();
    int shown = -1;
    for (int i = 0; i < p->in_flight_n; i++) {
        if (p->in_flight[i].display_ns <= now)
            shown = i;
    }
    if (shown < 0)
        return false;
    p->shown_pts = p->in_flight[shown].pts;
    int keep = p->in_flight_n - (shown + 1);
    memmove(p->in_flight, p->in_flight + shown + 1, keep * sizeof(p->in_flight[0]));
    p->in_flight_n = keep;
    return true;
}

// ---- Standard VO entry points ---------------------------------------------

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->in_flight_n = 0;
    p->shown_pts = -1;

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    if (!subtitle_worker_start(vo)) {
        MP_ERR(vo, "Could not start subtitle renderer\n");
        av_buffer_unref(&p->hwctx.av_device_ref);
        return -1;
    }
    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    // vo.c calls draw_frame + flip_page this much before each frame's
    // display time (`flip_queue_offset`, video/out/vo.c); flip_page
    // then queues the buffer with its real presentation timestamp.
    vo_set_queue_params(vo, TORRO_EARLY_RELEASE_NS, 1, 2);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    int64_t flip_start_ns = mp_time_ns();
    int64_t release_done_ns = flip_start_ns;
    bool timed_frame = p->next_image && p->next_display_ns > 0 &&
                       p->next_duration_ns > 0;
    double media_pts = p->next_image ? p->next_image->pts : MP_NOPTS_VALUE;
    if (p->next_image) {
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
        if (p->next_display_ns > 0) {
            av_mediacodec_render_buffer_at_time(buffer, mono_ns_for(p->next_display_ns));
            // Consume due timestamps before retaining the new future frame.
            advance_shown_frame(p);
            if (p->in_flight_n && p->next_display_ns <=
                p->in_flight[p->in_flight_n - 1].display_ns)
                p->in_flight_n = 0; // seek/discontinuous presentation clock
            if (p->in_flight_n == MP_ARRAY_SIZE(p->in_flight)) {
                // Saturation outside supported cadence: retain the earliest
                // future frames so the subtitle clock cannot starve; replace
                // only the furthest future timestamp. Memory remains bounded.
                p->in_flight_n--;
            }
            p->in_flight[p->in_flight_n].display_ns = p->next_display_ns;
            p->in_flight[p->in_flight_n].pts = p->next_image->pts;
            p->in_flight_n++;
        } else {
            // No timing: shown immediately, so it is the current frame.
            av_mediacodec_release_buffer(buffer, 1);
            p->shown_pts = p->next_image->pts;
            p->in_flight_n = 0;
        }
        release_done_ns = mp_time_ns();
        mp_image_unrefp(&p->next_image);
    }
    int64_t subs_start_ns = mp_time_ns();
    // Subtitles follow the frame on screen, not the one just queued
    // TORRO_EARLY_RELEASE_NS ahead: rendered for its pts and pushed
    // asynchronously; only bounded metadata crosses to the subtitle worker.
    // Rasterization and Rust mailbox copying cannot hold this release iteration.
    advance_shown_frame(p);
    if (p->shown_pts >= 0)
        subtitle_submit(vo, p->shown_pts);

    // Kodi 21.2 RenderManager.cpp:700-723: diagnose the video and overlay
    // stages separately. This probe does not claim that a late arrival was
    // caused by subtitles: ready_late measures arrival at draw_frame, while
    // flip_late includes the VO wait/scheduling interval. Only warn if a
    // frame interval was consumed; cap logging at once per second so a burst
    // of late frames does not itself flood the playback thread with logs.
    int64_t done_ns = mp_time_ns();
    int64_t queue_ns = p->next_display_ns - TORRO_EARLY_RELEASE_NS;
    if (timed_frame &&
        (done_ns - flip_start_ns > p->next_duration_ns ||
         flip_start_ns - queue_ns > p->next_duration_ns) &&
        (!p->last_slow_log_ns ||
         done_ns - p->last_slow_log_ns >= MP_TIME_S_TO_NS(1)))
    {
        p->last_slow_log_ns = done_ns;
        MP_WARN(vo, "torro-vo-late: pts=%.3f frame_ms=%.3f "
                "ready_late_ms=%.3f flip_late_ms=%.3f release_ms=%.3f "
                "unref_ms=%.3f subs_ms=%.3f\n",
                media_pts, MP_TIME_NS_TO_MS(p->next_duration_ns),
                MP_TIME_NS_TO_MS(p->next_draw_ns - queue_ns),
                MP_TIME_NS_TO_MS(flip_start_ns - queue_ns),
                MP_TIME_NS_TO_MS(release_done_ns - flip_start_ns),
                MP_TIME_NS_TO_MS(subs_start_ns - release_done_ns),
                MP_TIME_NS_TO_MS(done_ns - subs_start_ns));
    }
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    // Kodi 21.2 RendererMediaCodecSurface.cpp:95-104: record frame arrival
    // separately from the later Surface release; timing only, no queue change.
    p->next_draw_ns = mp_time_ns();
    p->next_duration_ns = frame->duration;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;
    // `vo_frame.pts` is the display time in mp_time ns (0 when the
    // core has no timing, e.g. display-synced or redraw).
    p->next_display_ns = (mpi && !frame->display_synced) ? frame->pts : 0;
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    if (request == VOCTRL_RESET) {
        // Seek / flush: the queued frames will never be shown.
        p->in_flight_n = 0;
        p->shown_pts = -1;
        subtitle_invalidate(p);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    // Force a fresh push on the next frame so the SubtitleView gets
    // the canvas dimensions for the new video params.
    struct priv *p = vo->priv;
    subtitle_invalidate(p);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);
    subtitle_worker_stop(p);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
}

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
