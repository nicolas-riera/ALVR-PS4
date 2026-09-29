#include "hmd_setup.h"

#include <stdint.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "log.h"

// Common dialog base: magic = 0xC0D1A109 + low 32 bits of the parameter's own address.
struct CommonDialogBaseParam {
    uint64_t size; // 0x30
    uint8_t reserved[36];
    uint32_t magic;
};

struct HmdSetupDialogParam {
    CommonDialogBaseParam base;
    uint64_t size;   // 0x68
    int32_t user_id; // must be logged in
    uint8_t disable_handover_screen;
    uint8_t reserved[40]; // checked: all zero
    uint8_t pad[3];
};

struct HmdSetupDialogResult {
    int32_t result; // 0 OK (headset ready), 1 canceled by the user
    uint8_t reserved[32];
};

static_assert(sizeof(CommonDialogBaseParam) == 0x30, "CommonDialogBaseParam size");
static_assert(sizeof(HmdSetupDialogParam) == 0x68, "HmdSetupDialogParam size");
static_assert(sizeof(HmdSetupDialogResult) == 0x24, "HmdSetupDialogResult size");

enum : int { STATUS_NONE = 0, STATUS_INITIALIZED = 1, STATUS_RUNNING = 2, STATUS_FINISHED = 3 };
static const uint32_t COMMON_DIALOG_MAGIC = 0xC0D1A109u;
static const int ERR_ALREADY_INITIALIZED = (int)0x80B80004;

static int (*p_initialize)();
static int (*p_open)(const HmdSetupDialogParam *);
static int (*p_update_status)();
static int (*p_get_result)(HmdSetupDialogResult *);
static int (*p_terminate)();

static bool g_ready_to_use, g_running;
static HmdSetupDialogParam g_param;

static bool resolve(int module, const char *name, void *fn)
{
    int rc = sceKernelDlsym(module, name, (void **)fn);
    if (rc != 0 || !*(void **)fn) {
        LOG("hmd setup dialog: symbol %s not found (0x%08x)", name, (unsigned)rc);
        return false;
    }
    return true;
}

bool hmd_setup_init(int common_module, int setup_module)
{
    if (common_module < 0 || setup_module < 0)
        return false;
    int (*common_init)() = nullptr;
    if (!resolve(common_module, "sceCommonDialogInitialize", &common_init) ||
        !resolve(setup_module, "sceHmdSetupDialogInitialize", &p_initialize) ||
        !resolve(setup_module, "sceHmdSetupDialogOpen", &p_open) ||
        !resolve(setup_module, "sceHmdSetupDialogUpdateStatus", &p_update_status) ||
        !resolve(setup_module, "sceHmdSetupDialogGetResult", &p_get_result) ||
        !resolve(setup_module, "sceHmdSetupDialogTerminate", &p_terminate))
        return false;
    LOG("sceCommonDialogInitialize -> 0x%08x", (unsigned)common_init());
    g_ready_to_use = true;
    return true;
}

static int g_user_id;

static int open_dialog()
{
    // The magic depends on the address: fill the static parameter in place.
    memset(&g_param, 0, sizeof(g_param));
    g_param.base.size = sizeof(CommonDialogBaseParam);
    g_param.base.magic = COMMON_DIALOG_MAGIC + (uint32_t)(uintptr_t)&g_param;
    g_param.size = sizeof(HmdSetupDialogParam);
    g_param.user_id = g_user_id;
    return p_open(&g_param);
}

bool hmd_setup_start(int user_id)
{
    if (!g_ready_to_use)
        return false;
    if (g_running)
        return true;
    int rc = p_initialize();
    if (rc < 0 && rc != ERR_ALREADY_INITIALIZED) {
        LOG("sceHmdSetupDialogInitialize -> 0x%08x", (unsigned)rc);
        return false;
    }
    g_user_id = user_id;
    rc = open_dialog();
    LOG("sceHmdSetupDialogOpen(user 0x%x) -> 0x%08x", user_id, (unsigned)rc);
    if (rc < 0) {
        p_terminate();
        return false;
    }
    g_running = true;
    return true;
}

bool hmd_setup_running()
{
    return g_running;
}

HmdSetupState hmd_setup_poll()
{
    if (!g_running)
        return HMD_SETUP_IDLE;
    int status = p_update_status();
    if (status != STATUS_FINISHED)
        return HMD_SETUP_RUNNING;
    HmdSetupDialogResult r;
    memset(&r, 0, sizeof(r));
    int rc = p_get_result(&r);
    LOG("hmd setup dialog finished: GetResult -> 0x%08x, result %d", (unsigned)rc, r.result);
    if (rc == 0 && r.result != 0) {
        // Canceled: ask again, as VR Worlds does (same parameter, no re-initialization).
        rc = open_dialog();
        LOG("sceHmdSetupDialogOpen (again) -> 0x%08x", (unsigned)rc);
        if (rc >= 0)
            return HMD_SETUP_RUNNING;
    }
    p_terminate();
    g_running = false;
    return rc == 0 && r.result == 0 ? HMD_SETUP_DONE : HMD_SETUP_FAILED;
}
