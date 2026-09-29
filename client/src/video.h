#pragma once

#include <stddef.h>
#include <stdint.h>

#include "foveation.h"
#include "reproj.h"

// Video stream: H.264 hardware decoding with libSceVideodec2, then NV12 -> BGRA
// conversion on the CPU into one linear texture per eye for the system compositor.
// Checks and structure layouts: reference/decomp/vdec2_api.c, vdec2_internal.c and
// reference/shadps4-src/src/core/libraries/videodec/videodec2.h.

bool video_init(int videodec2_module);

// Network thread side.
// view_*: one eye as displayed. ffe: foveated encoding settings, or null when it is off.
void video_set_stream(uint32_t view_width, uint32_t view_height, bool full_range, const FoveationSettings *ffe);
void video_set_decoder_config(uint32_t codec /*0 H264, 1 HEVC*/, const uint8_t *config, size_t len);
// One complete access unit (Annex B, SPS/PPS stripped by the streamer). Copied.
void video_push_frame(uint64_t timestamp_ns, bool is_idr, const uint8_t *data, size_t len);
// The stream lost packets: frames are dropped until the next IDR.
void video_packet_loss();
// True when the streamer should be asked for an IDR (rate-limited internally).
bool video_want_idr();
// Session ended: forget queued frames and the decoder configuration.
void video_reset();

// Render thread side, once per display frame. Converted frames wait in a FIFO; with `take`
// the oldest becomes the displayed frame (after skipping all but `keep` others, when more
// wait), otherwise the displayed frame stays. Returns false while there is no stream. The
// returned textures stay valid (and untouched by the decoder) until the call after the
// next one that takes a frame.
struct VideoFrame {
    const GnmTexture *eye[2];
    uint64_t timestamp_ns; // tracking timestamp the streamer rendered it for
    uint64_t decoded_us;   // process time when it was converted
    unsigned seq;          // increments with every frame taken for display
    int waiting;           // frames still waiting in the FIFO
};
bool video_next(VideoFrame *out, bool take, int keep);
// Frames converted but never displayed: FIFO full, skipped by video_next.
void video_pacing_stats(unsigned *overflow, unsigned *trimmed);
// Waits until a frame newer than after_seq is converted (true) or timeout_us passes
// (after_seq: video_published_seq()).
bool video_wait_new(unsigned after_seq, uint32_t timeout_us);
unsigned video_published_seq();

struct VideoStats {
    unsigned received, decoded, shown_candidates, dropped, errors;
    unsigned lost;      // frames with missing network packets
    unsigned queue_max; // most frames waiting for the decoder since the previous call
    uint64_t decode_us_avg, convert_us_avg;
    uint64_t decode_cpu_us_avg; // CPU time the decode thread spends inside Decode
    uint64_t bytes_avg;         // average access unit size
};
void video_get_stats(VideoStats *out);

// ---- Video bench (Dev build, tools/video_bench.py) -------------------------------------
// Decoder pipeline depth and conversion jobs (defaults 2 and 6); the decoder is recreated
// for another depth. null restores the defaults.
struct VideoBenchOptions {
    uint32_t decode_depth; // 1..8
    int convert_jobs;      // 2, 4, 6 or 8
};
void video_bench_set_options(const VideoBenchOptions *o);
struct VideoTimes {
    unsigned n;
    double avg, p50, p90, p99, max; // milliseconds
};
struct VideoBenchResult {
    unsigned received, decoded, published, dropped, replaced, errors;
    VideoTimes decode, convert, latency; // latency: frame pushed -> converted and published
    uint64_t bytes;
};
// Between begin and end every frame is timed. While a bench runs, a frame dropped because
// the queue is full does not make the decoder wait for an IDR (a bench clip has only one).
void video_bench_begin();
void video_bench_end(VideoBenchResult *out);
// Frames waiting for the decoder; pictures published since video_bench_begin.
int video_queued();
unsigned video_bench_published();
