#pragma once

// Persistent client settings, stored in /data/alvr-ps4/config.txt.

#define ALVR_STREAMER_VERSION "20.14.1"

struct ClientConfig {
    char hostname[64]; // ALVR client hostname, "NNNN.client" like the official client
};

void config_load(ClientConfig *cfg);
void config_store(const ClientConfig *cfg);
