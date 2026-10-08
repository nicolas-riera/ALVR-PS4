#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <orbis/SaveData.h>
#include <orbis/libkernel.h>

#include "log.h"

// Settings live in the PS4's own save data (Settings > Application Saved Data Management),
// in one save "settings" of the user who started the app, as a small key=value file. Both
// the stable and the Dev app use the stable app's save (INSTALL_DIR_SAVEDATA in the Dev
// param.sfo), so they keep the same hostname and the PC keeps trusting them.

static const char *SAVE_DIR_NAME = "settings";
static const char *SAVE_FILE = "settings.txt";
static const char *SAVE_TMP_FILE = "settings.tmp"; // written first, then renamed over SAVE_FILE
static const uint64_t SAVE_BLOCKS = 96; // the minimum (96 x 32 KiB)

static int g_user = -1;
static bool g_save_ok; // libSceSaveData initialized

// Saving runs on its own thread: mounting and committing take long enough to stall a frame.
static pthread_t g_saver;
static pthread_mutex_t g_save_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_save_cond = PTHREAD_COND_INITIALIZER;
static ClientConfig g_pending;
static bool g_has_pending, g_saver_started;

static const unsigned SAVE_NOT_FOUND = 0x809f0008; // read-only mount of a save never written

// Mounts the save (created on first use). Returns 0, or the error (logged).
static int mount(bool write, OrbisSaveDataMountPoint *mp)
{
    OrbisSaveDataDirName dir;
    memset(&dir, 0, sizeof(dir));
    strncpy(dir.data, SAVE_DIR_NAME, sizeof(dir.data) - 1);
    OrbisSaveDataMount2 m;
    memset(&m, 0, sizeof(m));
    m.userId = g_user;
    m.dirName = &dir;
    m.blocks = SAVE_BLOCKS;
    m.mountMode = write ? ORBIS_SAVE_DATA_MOUNT_MODE_RDWR | ORBIS_SAVE_DATA_MOUNT_MODE_CREATE2 |
                              ORBIS_SAVE_DATA_MOUNT_MODE_COPY_ICON
                        : ORBIS_SAVE_DATA_MOUNT_MODE_RDONLY;
    alignas(8) uint8_t result[128]; // OrbisSaveDataMountResult, with room to spare
    memset(result, 0, sizeof(result));
    int rc = sceSaveDataMount2(&m, (OrbisSaveDataMountResult *)result);
    if (rc < 0) {
        if (write || (unsigned)rc != SAVE_NOT_FOUND)
            LOG("config: save data mount (%s, user 0x%x) -> 0x%08x", write ? "write" : "read", g_user, (unsigned)rc);
        return rc;
    }
    memset(mp, 0, sizeof(*mp));
    memcpy(mp->data, result, sizeof(mp->data) - 1); // mount point name, first in the result
    return 0;
}

static void unmount(OrbisSaveDataMountPoint *mp)
{
    int rc = sceSaveDataUmount(mp);
    if (rc < 0)
        LOG("config: save data unmount -> 0x%08x", (unsigned)rc);
}

static void write_save(const ClientConfig *cfg)
{
    OrbisSaveDataMountPoint mp;
    if (mount(true, &mp) != 0)
        return;
    // Written to a temporary file renamed over the settings once complete: the system may
    // close the app at any time, and a half-written file would lose every setting.
    const char *dir = mp.data[0] == '/' ? mp.data + 1 : mp.data;
    char path[64], tmp[64];
    snprintf(path, sizeof(path), "/%s/%s", dir, SAVE_FILE);
    snprintf(tmp, sizeof(tmp), "/%s/%s", dir, SAVE_TMP_FILE);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "hostname=%s\n", cfg->hostname);
        fprintf(f, "resolution_percent=%d\n", cfg->resolution_percent);
        fprintf(f, "controller_prediction_ms=%d\n", cfg->controller_prediction_ms);
        fprintf(f, "head_prediction=%d\n", cfg->head_prediction_percent);
        fprintf(f, "camera_height_cm=%d\n", cfg->camera_height_cm);
        fprintf(f, "user_height_cm=%d\n", cfg->user_height_cm);
        fprintf(f, "center_on_steamvr_start=%d\n", cfg->center_on_connect);
        fprintf(f, "refresh_rate_hz=%d\n", cfg->refresh_rate);
        fprintf(f, "vibration_percent=%d\n", cfg->vibration_percent);
        fprintf(f, "hud=%d\n", cfg->hud);
        fprintf(f, "lobby_settings=%d\n", cfg->lobby_settings);
        for (int h = 0; h < 2; h++) {
            const char *side = h ? "right" : "left";
            fprintf(f, "pad_%s_swap=%d\n", side, cfg->pad_swap[h]);
            fprintf(f, "pad_%s_alt_move=%d\n", side, cfg->pad_alt_move[h]);
            fprintf(f, "pad_%s_alt_other=%d\n", side, cfg->pad_alt_other[h]);
        }
        const bool written = !ferror(f);
        if (fclose(f) != 0 || !written)
            LOG("config: cannot write %s", tmp);
        else if (rename(tmp, path) != 0)
            LOG("config: cannot rename %s to %s", tmp, path);
    } else {
        LOG("config: cannot write %s", tmp);
    }
    // What the system's saved data list shows.
    char detail[128];
    snprintf(detail, sizeof(detail), "ALVR client name %s", cfg->hostname);
    sceSaveDataSetParam(&mp, ORBIS_SAVE_DATA_PARAM_TYPE_TITLE, (void *)"ALVR PS4", 9);
    sceSaveDataSetParam(&mp, ORBIS_SAVE_DATA_PARAM_TYPE_SUB_TITLE, (void *)"Settings", 9);
    sceSaveDataSetParam(&mp, ORBIS_SAVE_DATA_PARAM_TYPE_DETAIL, detail, strlen(detail) + 1);
    unmount(&mp);
}

static void *saver_thread(void *)
{
    for (;;) {
        pthread_mutex_lock(&g_save_lock);
        while (!g_has_pending)
            pthread_cond_wait(&g_save_cond, &g_save_lock);
        ClientConfig cfg = g_pending;
        g_has_pending = false;
        pthread_mutex_unlock(&g_save_lock);
        const uint64_t t0 = sceKernelGetProcessTime();
        write_save(&cfg);
        LOG("config: saved (%.0f ms)", (sceKernelGetProcessTime() - t0) / 1000.0);
    }
    return nullptr;
}

// Returns 0 (read, or no settings file in the save), SAVE_NOT_FOUND (no save yet) or the
// mount error.
static int read_save(ClientConfig *cfg)
{
    OrbisSaveDataMountPoint mp;
    const int rc = mount(false, &mp);
    if (rc != 0)
        return rc;
    char path[64];
    snprintf(path, sizeof(path), "/%s/%s", mp.data[0] == '/' ? mp.data + 1 : mp.data, SAVE_FILE);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "hostname=%63s", cfg->hostname) == 1)
                continue;
            if (sscanf(line, "resolution_percent=%d", &cfg->resolution_percent) == 1)
                continue;
            if (sscanf(line, "controller_prediction_ms=%d", &cfg->controller_prediction_ms) == 1)
                continue;
            if (sscanf(line, "head_prediction=%d", &cfg->head_prediction_percent) == 1)
                continue;
            if (sscanf(line, "camera_height_cm=%d", &cfg->camera_height_cm) == 1)
                continue;
            if (sscanf(line, "user_height_cm=%d", &cfg->user_height_cm) == 1)
                continue;
            if (sscanf(line, "center_on_steamvr_start=%d", &cfg->center_on_connect) == 1)
                continue;
            if (sscanf(line, "refresh_rate_hz=%d", &cfg->refresh_rate) == 1)
                continue;
            if (sscanf(line, "vibration_percent=%d", &cfg->vibration_percent) == 1)
                continue;
            if (sscanf(line, "hud=%d", &cfg->hud) == 1)
                continue;
            if (sscanf(line, "lobby_settings=%d", &cfg->lobby_settings) == 1)
                continue;
            for (int h = 0; h < 2; h++) {
                const char *side = h ? "right" : "left";
                char key[32];
                int v;
                snprintf(key, sizeof(key), "pad_%s_swap=%%d", side);
                if (sscanf(line, key, &v) == 1)
                    cfg->pad_swap[h] = v;
                snprintf(key, sizeof(key), "pad_%s_alt_move=%%d", side);
                if (sscanf(line, key, &v) == 1)
                    cfg->pad_alt_move[h] = v;
                snprintf(key, sizeof(key), "pad_%s_alt_other=%%d", side);
                if (sscanf(line, key, &v) == 1)
                    cfg->pad_alt_other[h] = v;
            }
        }
        fclose(f);
    }
    unmount(&mp);
    return 0;
}

void config_load(ClientConfig *cfg, int user_id)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->resolution_percent = 130; // 1248x1404 per eye; foveated encoding keeps the decoded frame at 1920x1056
    cfg->controller_prediction_ms = 0;
    cfg->head_prediction_percent = CONFIG_DEFAULT_HEAD_PREDICTION;
    cfg->center_on_connect = 1;
    cfg->refresh_rate = 90;
    cfg->vibration_percent = 100;
    cfg->hud = 0;
    cfg->lobby_settings = 1;

    g_user = user_id;
    int rc = sceSaveDataInitialize3(0);
    g_save_ok = rc >= 0;
    if (!g_save_ok) {
        LOG("config: sceSaveDataInitialize3 -> 0x%08x, settings will not be kept", (unsigned)rc);
    } else {
        // A save that exists but cannot be read now is not overwritten with defaults (a new
        // hostname would make the PC distrust the PS4): tried 3 times, then this run keeps
        // its settings in memory only.
        for (int attempt = 0; attempt < 3; attempt++) {
            rc = read_save(cfg);
            if (rc == 0 || (unsigned)rc == SAVE_NOT_FOUND)
                break;
            sceKernelUsleep(200000);
        }
        if (rc != 0 && (unsigned)rc != SAVE_NOT_FOUND) {
            LOG("config: the settings save cannot be read (0x%08x): defaults for this run, nothing saved",
                (unsigned)rc);
            g_save_ok = false;
        }
    }

    if (cfg->resolution_percent < 50 || cfg->resolution_percent > 160)
        cfg->resolution_percent = 130;
    if (cfg->controller_prediction_ms < 0 || cfg->controller_prediction_ms > 60)
        cfg->controller_prediction_ms = 0;
    if (cfg->head_prediction_percent < 0 || cfg->head_prediction_percent > 100)
        cfg->head_prediction_percent = CONFIG_DEFAULT_HEAD_PREDICTION;
    if (cfg->camera_height_cm < 0 || cfg->camera_height_cm > 300)
        cfg->camera_height_cm = 0;
    if (cfg->user_height_cm < 100 || cfg->user_height_cm > 230)
        cfg->user_height_cm = 0;
    cfg->center_on_connect = cfg->center_on_connect != 0;
    if (cfg->refresh_rate != 60 && cfg->refresh_rate != 90)
        cfg->refresh_rate = 90;
    if (cfg->vibration_percent < 0 || cfg->vibration_percent > 100)
        cfg->vibration_percent = 100;
    cfg->hud = cfg->hud != 0;
    cfg->lobby_settings = cfg->lobby_settings != 0;
    for (int h = 0; h < 2; h++) {
        cfg->pad_swap[h] = cfg->pad_swap[h] != 0;
        cfg->pad_alt_move[h] = cfg->pad_alt_move[h] != 0;
        cfg->pad_alt_other[h] = cfg->pad_alt_other[h] != 0;
    }
    LOG("config: trackpad left swap %d alternate move %d other %d, right swap %d alternate move %d other %d",
        cfg->pad_swap[0], cfg->pad_alt_move[0], cfg->pad_alt_other[0], cfg->pad_swap[1], cfg->pad_alt_move[1],
        cfg->pad_alt_other[1]);
    if (cfg->hostname[0]) {
        LOG("config: hostname %s, resolution %d%%, %d Hz, headset prediction %d%%, extra controller prediction %d ms, "
            "camera height %d cm, user height %d cm, vibration %d%%, overlay %d (save data of user 0x%x)",
            cfg->hostname, cfg->resolution_percent, cfg->refresh_rate, cfg->head_prediction_percent,
            cfg->controller_prediction_ms, cfg->camera_height_cm, cfg->user_height_cm, cfg->vibration_percent, cfg->hud,
            user_id);
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
    if (!g_save_ok)
        return;
    pthread_mutex_lock(&g_save_lock);
    if (!g_saver_started)
        g_saver_started = pthread_create(&g_saver, nullptr, saver_thread, nullptr) == 0;
    g_pending = *cfg;
    g_has_pending = true;
    pthread_cond_signal(&g_save_cond);
    pthread_mutex_unlock(&g_save_lock);
}
