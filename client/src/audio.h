#pragma once

#include <stddef.h>
#include <stdint.h>

// Game audio (streamer -> PS4, s16 stereo) played on the main audio port, which the
// system routes to the PSVR headphones in VR mode, and microphone capture (PS4 -> streamer,
// s16 mono 48 kHz). Parameters follow Beat Saber (reference/decomp/beatsaber_audio.c).

typedef void (*AudioMicSink)(const uint8_t *pcm, size_t len);

bool audio_init(int audioout_module, int audioin_module, int user_id, AudioMicSink mic_sink);

// Network thread side.
void audio_start_stream(uint32_t game_sample_rate /*0: no game audio*/, bool microphone);
void audio_push_game(const uint8_t *pcm, size_t len);
void audio_stop_stream();
