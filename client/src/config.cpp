#include "config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <orbis/libkernel.h>

#include "log.h"

static const char *CONFIG_DIR = "/data/alvr-ps4";
static const char *CONFIG_PATH = "/data/alvr-ps4/config.txt";

void config_load(ClientConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "hostname=%63s", cfg->hostname) == 1)
                continue;
        }
        fclose(f);
    }
    if (cfg->hostname[0]) {
        LOG("config: hostname %s (from %s)", cfg->hostname, CONFIG_PATH);
        return;
    }
    // Same format as ALVR 20.14.1 (alvr/client_core/src/storage.rs): 4 random digits.
    uint64_t seed = sceKernelReadTsc() ^ (sceKernelGetProcessTime() << 17);
    unsigned n = (unsigned)((seed * 6364136223846793005ull + 1442695040888963407ull) >> 33) % 10000;
    snprintf(cfg->hostname, sizeof(cfg->hostname), "%04u.client", n);
    LOG("config: new hostname %s", cfg->hostname);
    config_store(cfg);
}

void config_store(const ClientConfig *cfg)
{
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        LOG("config: cannot write %s", CONFIG_PATH);
        return;
    }
    fprintf(f, "hostname=%s\n", cfg->hostname);
    fclose(f);
}
