#pragma once

#include <stddef.h>
#include <stdint.h>

#include "reproj.h"

// Video stream: H.264 hardware decoding with libSceVideodec2, then NV12 -> BGRA
// conversion on the CPU into one linear texture per eye for the system compositor.
// Checks and structure layouts: reference/decomp/vdec2_api.c, vdec2_internal.c and
// reference/shadps4-src/src/core/libraries/videodec/videodec2.h.

bool video_init(int videodec2_module);

// Network thread side.
void video_set_stream(uint32_t view_width, uint32_t view_height, bool full_range);
void video_set_decoder_config(uint32_t codec /*0 H264, 1 HEVC*/, const uint8_t *config, size_t len);
// One complete access unit (Annex B, SPS/PPS stripped by the streamer). Copied.
void video_push_frame(uint64_t timestamp_ns, bool is_idr, const uint8_t *data, size_t len);
// The stream lost packets: frames are dropped until the next IDR.
void video_packet_loss();
// True when the streamer should be asked for an IDR (rate-limited internally).
bool video_want_idr();
// Session ended: forget queued frames and the decoder configuration.
void video_reset();

// Render thread side: the most recent decoded frame. The returned textures stay valid
// (and untouched by the decoder) until the call after the next one.
struct VideoFrame {
    const GnmTexture *eye[2];
    uint64_t timestamp_ns; // tracking timestamp the streamer rendered it for
    uint64_t decoded_us;   // process time when it was decoded
    unsigned seq;          // increments with every new frame
};
bool video_latest(VideoFrame *out);
// Waits until a frame newer than after_seq is published (true) or timeout_us passes.
bool video_wait_new(unsigned after_seq, uint32_t timeout_us);

struct VideoStats {
    unsigned received, decoded, shown_candidates, dropped, errors;
    unsigned queue_max; // most frames waiting for the decoder since the previous call
    uint64_t decode_us_avg, convert_us_avg;
};
void video_get_stats(VideoStats *out);
