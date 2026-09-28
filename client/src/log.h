#pragma once

// Logging: every line goes to stdout, to a UDP broadcast on the LAN
// (received on the PC by tools/log_receiver.py), and to an in-memory ring
// buffer that the on-screen console draws.

#define LOG_UDP_PORT 9950 // 9943/9944 are used by ALVR
#define LOG_RING_LINES 40
#define LOG_LINE_MAX 256

void log_init();
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copies the ring buffer, oldest line first. Returns the number of lines.
int log_snapshot(char out[LOG_RING_LINES][LOG_LINE_MAX]);

#define LOG(...) log_printf(__VA_ARGS__)
