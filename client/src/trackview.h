#pragma once

#include <stdint.h>

#include "lobby.h"

// Server side of ALVR PS4 Tracking Viewer (PC companion, wire format in trackview_proto.h):
// viewers say hello on TRACKVIEW_PORT and get the tracked devices back while they keep doing
// so. Runs on the main loop, never blocks.

void trackview_init();
// Reads the viewers' hellos and drops the silent ones; true when a state is due (a viewer
// listens and the last state is old enough): fill a view and call trackview_send.
bool trackview_poll(uint64_t now);
void trackview_send(const LobbyView *view, uint64_t now);
