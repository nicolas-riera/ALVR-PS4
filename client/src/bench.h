#pragma once

// Video bench (Dev build only): tools/video_bench.py on the PC sends H.264 test clips over
// TCP port 9955; each clip is stored in memory, then played through the real video
// pipeline (queue, hardware decoder, conversion, display in the headset) at a given frame
// rate or as fast as the pipeline takes it, and the timings are sent back.

#define BENCH_PORT 9955

// Starts the bench server thread (does nothing in the stable build).
void bench_start();
// A clip is playing: the render loop shows the video even without a PC connected.
bool bench_active();
