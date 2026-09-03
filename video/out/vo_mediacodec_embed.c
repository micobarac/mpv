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
// `present_at_uptime_ms`: when the subtitle picture must become
// visible, on Android's uptime clock (CLOCK_MONOTONIC ms, the clock
// `View.postAtTime` takes). Frames are handed to the compositor
// TORRO_EARLY_RELEASE_NS ahead of their display time (see flip_page),
// so the subtitle for a frame is pushed that much early too and the
// Kotlin side holds it until this instant. 0 = now.
typedef void (*torro_subs_present_fn)(
    int32_t canvas_w, int32_t canvas_h,
    int32_t atlas_w,  int32_t atlas_h,
    const uint8_t *atlas_bgra,  // tight rows, stride = atlas_w * 4
    int32_t num_parts,
    const int32_t *parts,
    int64_t present_at_uptime_ms);
typedef void (*torro_subs_clear_fn)(int64_t present_at_uptime_ms);

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
    torro_subs_clear_fn   clear);

void torro_register_subtitle_callbacks(
    torro_subs_present_fn present,
    torro_subs_clear_fn   clear)
{
    g_subs_present = present;
    g_subs_clear   = clear;
}

__attribute__((visibility("default")))
void torro_set_subtitle_canvas(int width, int height);

void torro_set_subtitle_canvas(int width, int height)
{
    atomic_store(&g_canvas_w, width  > 0 ? width  : 0);
    atomic_store(&g_canvas_h, height > 0 ? height : 0);
}

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;

    // `sub_bitmaps.change_id` from mpv (`sub/osd.h:83`) — mpv keeps
    // this constant frame-over-frame when the rendered subtitle
    // image is unchanged. That's the "libass changed=0 fast path":
    // when it matches `last_change_id`, we skip the JNI push entirely
    // — the SubtitleView already shows the right frame.
    int last_change_id;
    // True iff the SubtitleView currently has a non-empty frame.
    // Used to debounce subtitle-clear calls: we only emit
    // a clear when subs *were* showing and now aren't.
    bool last_had_subs;
    // Per-flip flag set inside the bitmap callback. If osd_draw
    // returns without invoking the callback (mpv emits no bitmaps
    // because there's nothing to render this frame), and we *did*
    // have subs on the previous flip, push a clear.
    bool got_bitmaps_this_flip;
    // Display time (mp_time ns) of `next_image`, from `vo_frame.pts`.
    // 0 for redraws, which release immediately.
    int64_t next_display_ns;
    // Uptime-clock instant the current flip's subtitles must appear.
    int64_t subs_at_uptime_ms;
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

// Reusable scratch for tight-packed atlas pixel data. Sized to the
// largest atlas seen so far; grows monotonically (libass output
// rarely exceeds ~512x256 even on 4K content).
static uint8_t *g_atlas_scratch;
static size_t   g_atlas_scratch_cap;

// Reusable per-part metadata table. Capped at 256 parts — libass-via-
// mpv typically emits 1-20 parts (one per text run) even on the most
// complex ASS frames. 256 is comfortable headroom.
#define MAX_PARTS 256
static int32_t g_parts[8 * MAX_PARTS];

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

// Repack rows of `src` (with `src_stride` bytes per row) into a tight
// `bytes_per_row * h` buffer so Kotlin's `Bitmap.copyPixelsFromBuffer`
// gets row-aligned pixels.
static void repack_tight(uint8_t *dst, const uint8_t *src,
                         int src_stride, int bytes_per_row, int h)
{
    if (src_stride == bytes_per_row) {
        memcpy(dst, src, (size_t)bytes_per_row * (size_t)h);
        return;
    }
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * bytes_per_row,
               src + (size_t)y * src_stride,
               (size_t)bytes_per_row);
    }
}

static void render_sub_bitmaps_cb(void *ctx, struct sub_bitmaps *imgs)
{
    struct vo *vo = ctx;
    struct priv *p = vo->priv;
    p->got_bitmaps_this_flip = true;

    // libass / mpv `changed=0` fast path. `change_id` is incremented
    // by mpv only when the rendered subtitle picture actually
    // differs from the previous frame (`sub/osd.h:83`). Matching
    // change_id means the SubtitleView's existing atlas + parts are
    // still valid — push nothing.
    if (imgs->change_id == p->last_change_id)
        return;
    p->last_change_id = imgs->change_id;

    if (imgs->num_parts == 0 || imgs->format != SUBBITMAP_BGRA ||
        !imgs->packed)
    {
        if (p->last_had_subs) {
            if (g_subs_clear) g_subs_clear(p->subs_at_uptime_ms);
            p->last_had_subs = false;
        }
        return;
    }

    int aw = imgs->packed_w;
    int ah = imgs->packed_h;
    int row_bytes = aw * 4;  // SUBBITMAP_BGRA = 4 bpp premultiplied
    size_t need = (size_t)row_bytes * (size_t)ah;
    if (need > g_atlas_scratch_cap) {
        free(g_atlas_scratch);
        g_atlas_scratch = malloc(need);
        g_atlas_scratch_cap = g_atlas_scratch ? need : 0;
        if (!g_atlas_scratch) {
            MP_ERR(vo, "torro-subs: OOM allocating %zu-byte atlas scratch\n", need);
            return;
        }
    }
    repack_tight(g_atlas_scratch, imgs->packed->planes[0],
                 imgs->packed->stride[0], row_bytes, ah);

    int n = imgs->num_parts;
    if (n > MAX_PARTS) {
        MP_WARN(vo, "torro-subs: %d parts exceeds MAX_PARTS=%d, truncating\n",
                n, MAX_PARTS);
        n = MAX_PARTS;
    }
    for (int i = 0; i < n; i++) {
        struct sub_bitmap *b = &imgs->parts[i];
        int32_t *r = &g_parts[i * 8];
        r[0] = b->src_x;
        r[1] = b->src_y;
        r[2] = b->w;
        r[3] = b->h;
        r[4] = b->x;
        r[5] = b->y;
        r[6] = b->dw;
        r[7] = b->dh;
    }

    // Canvas dimensions: SubtitleView's on-screen pixel size (pushed
    // from Kotlin via `torro_set_subtitle_canvas` in `onSizeChanged`).
    // This is the same coordinate space `emit_subs` handed to mpv as
    // the OSD canvas. Fall back to source frame dims if Kotlin hasn't
    // reported yet — keeps the first frame from rendering on a zero
    // canvas while remaining harmlessly under-sized.
    int cw = atomic_load(&g_canvas_w);
    int ch = atomic_load(&g_canvas_h);
    if (cw <= 0 || ch <= 0) {
        cw = vo->params ? vo->params->w : aw;
        ch = vo->params ? vo->params->h : ah;
    }

    if (g_subs_present)
        g_subs_present(cw, ch, aw, ah, g_atlas_scratch, n, g_parts,
                       p->subs_at_uptime_ms);
    p->last_had_subs = true;
}

static void emit_subs(struct vo *vo, double pts, int64_t at_uptime_ms)
{
    struct priv *p = vo->priv;
    if (!vo->params)
        return;
    p->subs_at_uptime_ms = at_uptime_ms;

    // OSD canvas = SubtitleView's on-screen pixel size (Kotlin
    // pushes via `torro_set_subtitle_canvas`). Matters because
    // mpv's `sub-font-size` scales as `size * canvas_h / 720` and
    // `sub-pos` is canvas-relative — using the source frame size
    // would render fonts at source-pixel scale (much smaller on
    // 4K displays where source is 1080p) and leave sub-pos at
    // unintended on-screen positions. Fall back to source frame
    // dims if Kotlin hasn't reported yet (rare race on first frame).
    int cw = atomic_load(&g_canvas_w);
    int ch = atomic_load(&g_canvas_h);
    if (cw <= 0 || ch <= 0) {
        cw = vo->params->w;
        ch = vo->params->h;
    }
    struct mp_osd_res res = {
        .w = cw,
        .h = ch,
        .display_par = 1.0,
    };
    bool formats[SUBBITMAP_COUNT] = {0};
    formats[SUBBITMAP_BGRA] = true;  // mpv converts libass → premul BGRA

    p->got_bitmaps_this_flip = false;
    osd_draw(vo->osd, res, pts, OSD_DRAW_SUB_ONLY,
             formats, render_sub_bitmaps_cb, vo);

    if (!p->got_bitmaps_this_flip && p->last_had_subs) {
        // osd_draw chose not to invoke the callback (no subs to
        // render). Clear the SubtitleView so the previous frame's
        // text doesn't stay on screen.
        if (g_subs_clear) g_subs_clear(at_uptime_ms);
        p->last_had_subs = false;
        p->last_change_id = 0;
    }
}

// ---- Standard VO entry points ---------------------------------------------

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->last_change_id = 0;
    p->last_had_subs = false;

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
    double pts = 0;
    int64_t at_uptime_ms = 0;
    if (p->next_image) {
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
        if (p->next_display_ns > 0) {
            int64_t mono_ns = mono_ns_for(p->next_display_ns);
            av_mediacodec_render_buffer_at_time(buffer, mono_ns);
            at_uptime_ms = mono_ns / INT64_C(1000000);
        } else {
            av_mediacodec_release_buffer(buffer, 1);
        }
        pts = p->next_image->pts;
        mp_image_unrefp(&p->next_image);
    }
    emit_subs(vo, pts, at_uptime_ms);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

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
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    // Force a fresh push on the next frame so the SubtitleView gets
    // the canvas dimensions for the new video params.
    struct priv *p = vo->priv;
    p->last_change_id = 0;
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);
    if (g_subs_clear) g_subs_clear(0);

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
