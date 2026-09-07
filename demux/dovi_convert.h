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

#pragma once

#include <stdbool.h>

struct demuxer;
struct sh_stream;
struct demux_packet;
struct mp_codec_params;

// Dolby Vision profile 7 -> 8.1 conversion in the demuxer, for hardware
// decoders that accept single-layer input only (Android MediaCodec).
//
// Per base-layer packet: the enhancement-layer NAL units (HEVC UNSPEC63)
// are dropped and the RPU (UNSPEC62) is rewritten with libdovi mode 2, so
// the decoder receives a valid single-layer 8.1 stream and switches the
// panel to Dolby Vision. For files carrying the EL as a separate track the
// RPU lives in the EL packets; those are read, their RPU converted and
// attached to the base-layer packet with the same timestamp.
//
// Same operation as Kodi's "Dolby Vision compatibility mode"
// (xbmc PR #22546, CBitstreamConverter::processDoviRpu), extended to the
// two-track layout. Only the enhancement picture data is lost; on MEL discs
// it is empty, on FEL discs it is unrecoverable on this class of hardware.
struct mp_dovi_convert;

// Create a converter for base-layer stream `bl`. `two_track` selects the
// separate-EL-track mode (RPU arrives via mp_dovi_convert_push_el).
// Returns NULL if the stream is not HEVC.
struct mp_dovi_convert *mp_dovi_convert_create(struct demuxer *demuxer,
                                               struct sh_stream *bl,
                                               bool two_track);

// Rewrite the stream's Dolby Vision configuration record to profile 8.1
// (single layer, HDR10-compatible base) so the decoder is opened as DV.
void mp_dovi_convert_fix_codec(struct mp_codec_params *codec);

// Feed a base-layer packet. Takes ownership.
void mp_dovi_convert_push_bl(struct mp_dovi_convert *s, struct demux_packet *dp);

// Feed an enhancement-layer packet (two-track mode). Takes ownership.
void mp_dovi_convert_push_el(struct mp_dovi_convert *s, struct demux_packet *dp);

// Next converted base-layer packet, or NULL when nothing is ready.
struct demux_packet *mp_dovi_convert_pop(struct mp_dovi_convert *s);

// End of stream: release every held packet for output.
void mp_dovi_convert_flush(struct mp_dovi_convert *s);

// Seek: drop every held packet.
void mp_dovi_convert_reset(struct mp_dovi_convert *s);
