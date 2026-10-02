# PS VR on the PS4: technical reference

Everything this project learned about driving the PlayStation VR headset, the PS Camera
tracking, the PS Move and DualShock 4 controllers and the related PS4 system services from a
homebrew app. Sony never published this outside its SDK, and community documentation of the
VR side of the PS4 is close to nonexistent, so this page tries to be the reference we wish
we had: call sequences, structure layouts, parameter values, error codes, timings, and the
traps that cost us hours.

How the facts were established:

- the PS4 system libraries' own parameter checks (sizes, ranges, reserved fields that must
  be zero), read from the libraries the console loads;
- public structure definitions where they exist (the
  [shadPS4](https://github.com/shadps4-emu/shadPS4) emulator headers and the
  [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain);
- hardware tests, logged over the network: a PS4 FAT and a PS4 Pro on firmware 11.00 with
  GoldHEN, a CUH-ZVR2 headset, a PS Camera, two PS Moves and a DualShock 4.

Each statement says how sure it is: **verified** (seen working, or measured, on hardware),
**observed** (seen in the logs, meaning not fully understood) or **inferred** (deduced from
the libraries, not exercised). The code that uses all of it is in `client/src/`; file names
are given in each section.

Contents:

1. [The hardware](#1-the-hardware)
2. [App setup: package, sandbox, modules, memory](#2-app-setup-package-sandbox-modules-memory)
3. [The headset: libSceHmd](#3-the-headset-libscehmd)
4. [Getting the headset connected: the system dialogs](#4-getting-the-headset-connected-the-system-dialogs)
5. [VR display mode: video output and refresh rate](#5-vr-display-mode-video-output-and-refresh-rate)
6. [The VR compositor: reprojection](#6-the-vr-compositor-reprojection)
7. [Tracking: PS Camera and libSceVrTracker](#7-tracking-ps-camera-and-libscevrtracker)
8. [PS Move: libSceMove](#8-ps-move-libscemove)
9. [DualShock 4: libScePad](#9-dualshock-4-libscepad)
10. [Raw HID access: /dev/hid](#10-raw-hid-access-devhid)
11. [System service: background, PS menu, closing](#11-system-service-background-ps-menu-closing)
12. [Audio: headset output and microphone](#12-audio-headset-output-and-microphone)
13. [Video decoding: libSceVideodec2](#13-video-decoding-libscevideodec2)
14. [PS4 Pro specifics](#14-ps4-pro-specifics)
15. [Coordinate systems and conversions](#15-coordinate-systems-and-conversions)
16. [Error codes](#16-error-codes)
17. [Open questions](#17-open-questions)

---

## 1. The hardware

| Part | Facts |
| --- | --- |
| Headset (CUH-ZVR1 / ZVR2) | One 1920×1080 OLED panel (reported by `sceHmdGetDeviceInformation`, **verified**), split in two halves of 960×1080, one per eye. Runs at 90 Hz or 120 Hz (the PS4 output is 89.91 Hz or 119.88 Hz, see [section 5](#5-vr-display-mode-video-output-and-refresh-rate)). LEDs on the front and back that the camera tracks, an IMU, a microphone and a headphone jack. |
| Processor unit | Sits between the PS4 and the TV, and gives the TV its "social screen" while the headset is in use. The PS4 talks to it over HDMI and USB; with its USB cable unplugged the headset status is `NOT_READY_HMU_DISCONNECT`. |
| PS Camera | Stereo camera; the VR tracker uses it at 60 frames per second (**verified**: a new frame every ~16.7 ms). It is the origin of the tracking space ([section 15](#15-coordinate-systems-and-conversions)). |
| PS Move | Glowing sphere tracked by the camera, IMU, trigger, 9 buttons, rumble motor. Sphere 46 mm across (Sony's specification). |
| DualShock 4 | Light bar tracked by the camera, IMU, touchpad, rumble. |

Field of view reported by `sceHmdGetFieldOfView`, the same on every launch and both tested
consoles (**verified**), as tangents of the half angles:

| | Tangent | Angle |
| --- | --- | --- |
| Outer (towards the ear) | 1.2074 | 50.4° |
| Inner (towards the nose) | 1.1813 | 49.8° |
| Top | 1.2629 | 51.6° |
| Bottom | 1.2629 | 51.6° |

So each eye sees about 100° horizontally and 103° vertically, slightly asymmetric
(more towards the ear). The right eye is the mirror image of the left one.

Display latency reported by the device information (**observed**):
`flip_to_display_latency_90hz` = 5555 µs and `flip_to_display_latency_120hz` = 4160 µs,
that is half a refresh period from the flip to the panel.

## 2. App setup: package, sandbox, modules, memory

### Package flags (param.sfo)

The `ATTRIBUTE` value of param.sfo tells the system what the app does with VR
(**verified** for the two bits this project uses, `0x00004000` and `0x04000000`, and for
the Pro bit):

| Bit | Meaning |
| --- | --- |
| `0x00004000` | Supports PlayStation VR. |
| `0x04000000` | PlayStation VR required: the system asks for the headset when the app starts. |
| `0x00800000` | PS4 Pro enhanced: on a Pro, the app runs in Pro ("Neo") mode ([section 14](#14-ps4-pro-specifics)). Harmless on a base PS4. |

This project uses `0x04004000`. The SDK version written in the executable is `0x05508001`
(SDK 5.50); the camera configuration facts in
[section 7](#7-tracking-ps-camera-and-libscevrtracker) were found with it.

### Loading the system libraries

Apps run in a sandbox: `/system/common/lib` is not visible as such. It is exposed as
`/<random word>/common/lib`, where the word comes from `sceKernelGetFsSandboxRandomWord()`
(**verified**). Each library is loaded with:

```c
snprintf(path, sizeof(path), "/%s/common/lib/%s.sprx", sceKernelGetFsSandboxRandomWord(), name);
int handle = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
sceKernelDlsym(handle, "sceHmdOpen", (void **)&fn);
```

Loading a library that is already loaded just returns its handle. This project first loads
each one through the system module manager, `sceSysmoduleLoadModule(id)` (ids below
`0x80000000`) or `sceSysmoduleLoadModuleInternal(id)` (internal ids, `0x8000xxxx`), as apps
normally do, then by path to get a handle for `sceKernelDlsym`. The ids used
(**verified**: each loads and its functions work):

| Library | Sysmodule id | Used for |
| --- | --- | --- |
| libSceHmd | `0x00D4` | Headset: device information, open, field of view, the reprojection compositor |
| libSceVrTracker | `0x00ED` | Camera tracking of the headset and the controllers |
| libSceMove | `0x008F` | PS Move buttons, trigger, rumble |
| libSceMoveTracker | `0x00B1` | Loaded, not called (libSceVrTracker does the tracking) |
| libSceCamera | `0x8000001A` (internal) | PS Camera, feeds the tracker |
| libSceVideodec2 | `0x00CF` | H.264 / HEVC decoding |
| libSceAudioOut | `0x80000001` (internal) | Sound output |
| libSceAudioIn | `0x80000002` (internal) | Microphone |
| libSceHmdDistortion, libSceHmdReprojectionMultilayer | none | Loaded by path only; their exports were not needed |
| libSceCommonDialog | `0x80000018` (internal) | Base of the system dialogs |
| libSceHmdSetupDialog | `0x00EB` | "Connect your PlayStation VR" dialog |
| libSceVrServiceDialog | `0x00FD` | "Confirm your position" dialog |
| libSceVideoOut | none (always loaded) | Display; `sceVideoOutConfigureOutputMode_` is only reachable through `sceKernelDlsym` |

The user: `sceUserServiceInitialize(NULL)` (`0x80960003` = already initialized), then
`sceUserServiceGetInitialUser(&user)`. The headset, the controllers and the microphone are
opened for that user id. Code: `client/src/main.cpp` (`probe_modules`, `init_user`).

### Direct memory

The VR libraries take memory from the app instead of allocating it: they report a size and
an alignment, and the app maps direct memory of the right type:

```c
off_t phys;
sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, type, &phys);
sceKernelMapDirectMemory(&ptr, size, 0x33 /* CPU + GPU read/write */, 0, phys, align);
```

| Type | Name | Use |
| --- | --- | --- |
| 0 | Onion, write-back (cached) | Anything the CPU reads: the libraries' "onion" buffers, CPU-rendered images, decoder CPU memory |
| 3 | Garlic, write-combined | GPU-only data: the libraries' "garlic" buffers, display buffers |

Trap (**verified**): the CPU reading garlic memory is extremely slow. A software renderer
that blends into an image (reads back pixels) must draw into onion memory; the compositor
reads onion textures fine.

### The one call every VR app needs: sceGnmSubmitDone

Once an app uses the GPU (the compositor and the tracker do, through their own command
buffers), the system uses `sceGnmSubmitDone()` as its safe point to suspend or close the
app. An app that never calls it cannot be closed: the PS menu's "Close application" hangs,
then the console shows error **CE-34878-0** (**verified**). Call it once per frame, also in
any waiting loop (for example while the headset is not connected).

## 3. The headset: libSceHmd

Code: `client/src/hmd.cpp`, `hmd.h`.

### Initialization and device information

```c
struct HmdInitializeParam { void *reserved0; uint8_t reserved[8]; };   // all zero
int sceHmdInitialize(const HmdInitializeParam *param);                // 0x81110001 = already initialized

enum HmdDeviceStatus : uint32_t {
    READY = 0,                     // connected, powered, usable
    NOT_READY = 1,                 // also returned for a few hundred ms while the headset powers on
    NOT_DETECTED = 2,              // processor unit not connected
    NOT_READY_HMU_DISCONNECT = 3,  // processor unit's USB cable unplugged
};

struct HmdDeviceInformation {      // 0x20 bytes
    HmdDeviceStatus status;
    int32_t  user_id;
    uint8_t  reserve0[4];
    uint32_t panel_width;          // 1920
    uint32_t panel_height;         // 1080
    uint16_t flip_to_display_latency_90hz;   // 5555 (µs)
    uint16_t flip_to_display_latency_120hz;  // 4160 (µs)
    uint8_t  hmu_mount;            // 0 or 1 (see below)
    uint8_t  reserve1[7];
};
int sceHmdGetDeviceInformation(HmdDeviceInformation *info);                 // no handle needed
int sceHmdGetDeviceInformationByHandle(int32_t handle, HmdDeviceInformation *info);
```

`sceHmdGetDeviceInformation` works before the headset is opened: it is how an app waits for
the headset. `hmu_mount` is the headset's proximity sensor: it went to 0 each time the
headset was taken off and back to 1 when it was put on again, within a second, the rest of
the structure unchanged (**measured** on 2026-10-02 with the Dev build's logs). libSceHmd
reads it from the device on every call (no cache), so it can be polled; this project polls
it a few times per second, logs every change of the whole structure, and treats it as the
worn sensor once it has been 1 during the launch.

### Opening and the field of view

```c
int sceHmdOpen(int32_t user_id, int32_t type /*0*/, int32_t index /*0*/, void *param /*NULL*/);  // returns the handle
struct HmdFieldOfView { float tan_out, tan_in, tan_top, tan_bottom; };
int sceHmdGetFieldOfView(int32_t handle, HmdFieldOfView *fov);
int sceHmdClose(int32_t handle);
int sceHmdTerminate(void);
```

Keeping the headset open is part of what makes the system treat the app as a VR app.
The handle is needed by the tracker (it registers the headset by handle) and the
compositor.

### Power cycles

When the headset is switched off and on, or replugged, while the app runs (**verified**):

1. `sceHmdGetDeviceInformation` reports `NOT_READY` / `NOT_DETECTED`, then `READY` again.
2. The old handle is dead: `sceHmdGetDeviceInformationByHandle` returns `0x81110003`
   (handle invalid).
3. The app must close it, open the headset again, and register the new handle with the
   tracker (unregister the old one first).
4. Opening right after `READY` can still fail for a moment; retrying every second works.
5. The tracking restarts from scratch: the headset is not tracked until the camera sees it
   again.

## 4. Getting the headset connected: the system dialogs

Code: `client/src/hmd_setup.cpp`, `hmd_setup.h`; `wait_for_headset` and `monitor_headset`
in `main.cpp`.

The system has a ready-made "connect your PlayStation VR and turn it on" screen, with the
right pictures for each case (processor unit, USB, power). An app shows it instead of its
own message, and it finishes by itself the moment the headset becomes ready.

### Common dialog base

All system dialogs share a parameter header and must have `sceCommonDialogInitialize()`
called once (libSceCommonDialog):

```c
struct CommonDialogBaseParam {   // 0x30 bytes
    uint64_t size;               // 0x30
    uint8_t  reserved[36];
    uint32_t magic;              // 0xC0D1A109 + low 32 bits of the address of the parameter itself
};
```

The magic depends on where the parameter lives, so fill it in place (a copy moved
elsewhere is rejected). Only one common dialog can run at a time.

Dialog status values (`...UpdateStatus()`): 0 none, 1 initialized, 2 running,
3 finished. `0x80B80004` from `...Initialize()` means already initialized and is harmless.

### "Connect your PlayStation VR": libSceHmdSetupDialog

```c
struct HmdSetupDialogParam {     // 0x68 bytes
    CommonDialogBaseParam base;
    uint64_t size;               // 0x68
    int32_t  user_id;            // a logged-in user
    uint8_t  disable_handover_screen;
    uint8_t  reserved[40];       // checked: all zero
    uint8_t  pad[3];
};
struct HmdSetupDialogResult {    // 0x24 bytes
    int32_t result;              // 0 = headset ready, 1 = canceled by the user
    uint8_t reserved[32];
};
int sceHmdSetupDialogInitialize(void);
int sceHmdSetupDialogOpen(const HmdSetupDialogParam *param);
int sceHmdSetupDialogUpdateStatus(void);                    // call every frame
int sceHmdSetupDialogGetResult(HmdSetupDialogResult *r);    // once finished
int sceHmdSetupDialogTerminate(void);
```

Behaviour (**verified**):

- Opened while the headset is already ready, it finishes at once without showing anything.
- When the user cancels it (Circle), it finishes with result 1. Opening it again with the
  same parameter (no new `Initialize`) puts it back on screen: an app that needs the
  headset simply asks again until it is ready.
- `NOT_READY` is also the state of a headset that is powering on; waiting about one second
  before opening the dialog avoids flashing it at every power-on.
- If `UpdateStatus` returns a negative value, the dialog is gone: terminate it.
- Do not open it while the app is in the background (PS menu): wait until it is back in
  front ([section 11](#11-system-service-background-ps-menu-closing)).

### "Confirm your position": libSceVrServiceDialog

The PS VR screen that shows the camera view and asks the user to look at the camera, which
gets the tracker to find the headset. Useful when the tracking does not start (for example
the headset started while lying where the camera cannot see it).

```c
struct VrServiceDialogParam {    // 0x68 bytes
    CommonDialogBaseParam base;
    uint64_t size;               // 0x68
    uint32_t mode;               // < 3; 0 = confirm position
    uint8_t  reserved[44];       // checked: bytes 0x3d-0x67 zero
};
int sceVrServiceDialogInitialize(void);
int sceVrServiceDialogOpen(const VrServiceDialogParam *param);
int sceVrServiceDialogUpdateStatus(void);
int sceVrServiceDialogGetResult(void *result);   // same 0x24-byte shape as above
int sceVrServiceDialogTerminate(void);
```

Mode 0 was **verified** (the position screen appears; result 0 after it). Modes 1 and 2
were not tried. This project opens it once per launch, when the headset is moving (worn)
but still not tracked after 10 seconds.

## 5. VR display mode: video output and refresh rate

Code: `client/src/screen.cpp` (`screen_register_vr_buffers`, `screen_set_vr_output_mode`).

An app does not draw to the headset directly. It opens the TV output as usual, registers
two special buffers for the compositor, switches the output to the VR refresh rate, then
starts the compositor ([section 6](#6-the-vr-compositor-reprojection)).

Trap (**verified**): drawing side-by-side frames to the normal TV buffers does show
something in the headset, but when the app is killed the PSVR stays stuck in VR display
mode. Going through the compositor is what lets the system restore everything.

### VR display buffers

```c
OrbisVideoOutBufferAttribute attr = {0};
attr.format     = 0x80000000;   // A8R8G8B8 sRGB
attr.tmode      = 0;            // tiled
attr.width      = 1920;
attr.height     = 1080;
attr.pixelPitch = 1920;
attr.reserved[0] = 7;           // buffer option "VR"
sceVideoOutRegisterBuffers(handle, 2 /* first index */, bufs, 2, &attr);  // returns the group index (>= 0)
```

Two buffers of 1920×1080, tiled, with option 7, in garlic memory (16 MB each is generous for
the tiling padding). Only the GPU writes them. The app's own 2D buffers (indices 0 and 1)
stay registered for its TV status screen. This is the combination
`sceHmdReprojectionSetDisplayBuffers` accepts (**verified**).

### Output mode and refresh rate

```c
struct VideoOutMode {            // 0x20 bytes
    uint32_t size;               // 0x20
    uint8_t  signal_encoding, signal_range, colorimetry, depth;
    uint64_t refresh_rate;       // SceVideoOutRefreshRate
    uint64_t resolution;
    uint8_t  reserved[8];
};
int sceVideoOutConfigureOutputMode_(int32_t handle, uint32_t reserved /*0*/, const VideoOutMode *mode,
                                    const void *options, uint32_t mode_size, uint32_t options_size);
```

Fill the whole mode with `0xFF` ("any"), then set `size`, the refresh rate and
`resolution = 0xFFFFFFFFC1FFFFFF`. Call with `options = NULL, options_size = 0x10`; if that
fails, retry with 16 zero bytes of options.

| Refresh rate value | Output | Headset |
| --- | --- | --- |
| 3 | 59.94 Hz | the compositor's 60 Hz branch (not used) |
| 13 (`0xD`) | 119.88 Hz | 120 Hz |
| 35 (`0x23`) | 89.91 Hz | native 90 Hz |

Facts (**verified**):

- The compositor reads the output's refresh rate **once, when it starts**, and picks its
  timing from it. Switch the output first, then poll `sceVideoOutGetResolutionStatus`
  until `refreshRate` reports the new value (it did at once in every test; this project
  waits up to 2 s and falls back to 119.88 Hz), and only then start the compositor.
- At 120 Hz, apps usually render 60 frames per second and let the compositor show each
  frame twice, re-projected to the latest head pose; 90 Hz has no such ratio and is
  rendered at the full rate.
- The output mode switch makes HDMI capture cards drop the signal and re-sync.
- The 89.91 Hz output and a PC rendering at exactly 90.00 Hz drift by one frame every
  ~11 s; a streaming client must expect one dropped frame per ~11 s.

## 6. The VR compositor: reprojection

Code: `client/src/reproj.cpp`, `reproj.h`.

libSceHmd contains the system's VR compositor ("reprojection"). It runs its own thread,
wakes once per display refresh, takes the last frame the app submitted, warps it to the
latest head orientation, corrects the lens distortion and chromatic aberration, and flips
the VR buffers. Displaying through it is what puts the PS4 in VR mode properly: the PS
button quick menu shows in the headset, the TV gets the social screen, and the system can
take the headset back when the app closes.

### Setup

```c
uint64_t sceHmdReprojectionQueryOnionBuffSize(void);    // 0x810 (observed)
uint64_t sceHmdReprojectionQueryOnionBuffAlign(void);   // 0x100
uint64_t sceHmdReprojectionQueryGarlicBuffSize(void);   // 0x100000
uint64_t sceHmdReprojectionQueryGarlicBuffAlign(void);  // 0x100

struct ReprojInitParam {         // 0x38 bytes
    void    *onion;              // onion memory of the queried size (type 0)
    void    *garlic;             // garlic memory of the queried size (type 3)
    uint64_t thread_priority;    // 0x100 used
    uint64_t cpu_mask;           // <= 0x3f; 0x38 (cores 3-5) used
    uint32_t mode_a;             // <= 6; 5 used
    uint32_t mode_b;             // <= 7; 5 used
    uint32_t reserved0, reserved1;
    uint64_t reserved2;          // zero
};
int sceHmdReprojectionInitialize(const ReprojInitParam *p, uint32_t type, void *reserved);
int sceHmdReprojectionSetDisplayBuffers(int32_t videoout_handle, int32_t index_a, int32_t index_b, void *reserved);
```

The library checks that both buffers are set and different, the CPU mask fits cores 0-5, and
the modes are within range. The meaning of `mode_a` / `mode_b` and of the `type` argument
(0 to 2) is not known; `type` 1 works (**verified**). `SetDisplayBuffers` takes the indices
of the two VR buffers registered in [section 5](#5-vr-display-mode-video-output-and-refresh-rate).

Shutdown: `sceHmdReprojectionStop()`, `sceHmdReprojectionUnsetDisplayBuffers()`,
`sceHmdReprojectionFinalize()`.

### Textures

Images are handed over as AMD GCN texture descriptors (T#, 8 dwords), built by the app.
For a linear 32-bit image in memory order B, G, R, A (the usual video output format):

```c
uint64_t addr = (uint64_t)base >> 8;          // 256-byte aligned
dw[0] = (uint32_t)addr;
dw[1] = (uint32_t)(addr >> 32) & 0x3f
      | 10 << 20                              // data format 8_8_8_8
      | 9 << 26;                              // number format sRGB
dw[2] = (width - 1) | (height - 1) << 14;
dw[3] = 6 | 5 << 3 | 4 << 6 | 1 << 9         // swizzle: R <- z, G <- y, B <- x, A <- 1
      | 8 << 20                               // tiling index 8: display linear aligned
      | 9 << 28;                              // type: 2D color
dw[4] = (pitch_pixels - 1) << 13;
dw[5] = dw[6] = dw[7] = 0;
```

and a sampler descriptor (S#, 4 dwords): clamp to last texel on all three axes
(`2 | 2 << 3 | 2 << 6`), no mip levels, bilinear magnification and minification
(`1 << 20 | 1 << 22` in dword 2).

Rules learned on hardware (**verified**):

- **One texture per eye.** With both eyes side by side in one 1920-wide image and the
  per-eye transform selecting each half, the compositor ignored the pitch and mixed rows
  of the two eyes.
- The pitch must be properly aligned (the project aligns eye buffers generously); the
  textures may live in onion memory, which matters when the CPU writes them.
- The values written are sRGB; alpha is ignored (forced to 1 by the swizzle above).

### Submitting a 2D screen

`sceHmdReprojectionStart2dVr` shows one image as a floating screen in front of the user,
like the system's own cinematic mode. Handy before any 3D content is ready.

```c
struct Reproj2dParam {           // 0x50 bytes
    const GnmTexture *texture;   // T#
    const void *sampler;         // S#, 16 bytes
    float uv_transform[4];       // {1, 1, 0, 0} for the whole image
    void *label;                 // a writable buffer (1 KB, 64-byte aligned, works)
    uint32_t time_us;            // 2000..6999; 3000 used
    uint32_t pad;
    uint64_t zero[4];
};
int sceHmdReprojectionStart2dVr(const Reproj2dParam *p, uint64_t frame_number, void *reserved);
```

The library requires `texture`, `sampler` and `label` to be set and the last four qwords to
be zero. `frame_number` is the app's own counter, incremented at each submission.

### Submitting a stereo frame

```c
struct ReprojStereoParam {       // 0x80 bytes
    const GnmTexture *left;      // T# of the left eye image
    const GnmTexture *right;     // T# of the right eye image
    const void *sampler;
    float fov_left[4];           // where the left image sits in tangent space (below)
    float fov_right[4];
    void *label;                 // 8-byte aligned, writable
    uint32_t time_us;            // 2000..6999
    uint32_t pad;
    uint64_t unk9;
    uint32_t mode;               // < 2
    uint32_t pad2;
    uint64_t flags;              // only bits 0-3 and 28-31 may be set
    uint64_t zero[4];
};
struct ReprojPose {              // 0x38 bytes: the head pose the frame was rendered with
    float position[3];           // tracker space, metres
    float orientation[4];        // x, y, z, w, tracker space
    uint32_t pad0;
    uint64_t timestamp;          // tracker timestamp of that pose (µs)
    uint64_t unk28;
    uint32_t unk30, pad1;
};
int sceHmdReprojectionStart(const ReprojStereoParam *p, const ReprojPose *pose,
                            uint64_t frame_number, void *reserved);
```

**The per-eye block** (`fov_left`, `fov_right`) maps a direction to a texture coordinate:

```
u = tan_x * scale_x + offset_x        (x to the right, y downwards)
v = tan_y * scale_y + offset_y
block = { scale_x, scale_y, offset_x, offset_y }
```

For an image that covers the tangents `left`, `right`, `up`, `down` (all positive
magnitudes):

```c
block[0] = 1 / (left + right);
block[1] = 1 / (up + down);
block[2] = left / (left + right);
block[3] = up / (up + down);
```

So an app can render any field of view (larger to leave room for the warp, or the headset's
own one from `sceHmdGetFieldOfView`) and tell the compositor exactly where it is. This
layout was deduced from captures of the headset output (the image size varied inversely
with the scale, and the edges landed where predicted) (**verified**).

**The pose** is the head pose the frame was rendered for. The compositor compares it with
the latest tracked pose and corrects the difference in **orientation** only: it does not
move the image for position changes (**verified**: a renderer that lags the head position
shows "jelly" translation that the reprojection does not hide). The timestamp must be the
tracker timestamp of that pose (`sceVrTrackerGetResult`, [section 7](#7-tracking-ps-camera-and-libscevrtracker)).
The library checks it against the current tracking time; submit frames in pose order.

A streaming client renders nothing itself: it hands over the decoded frame with the pose
the remote renderer used, and the compositor re-projects it to where the head is now.
Head rotation then stays smooth even when a frame is shown twice.

### Pacing on the compositor's pass

```c
int sceHmdReprojectionSetUserEventStart(OrbisKernelEqueue eq, int id);
```

After `sceKernelCreateEqueue` and `sceKernelAddUserEventEdge(eq, id)`, the compositor
triggers the user event at the start of every pass, that is once per refresh
(**verified**). Waiting on that queue paces an app to the headset exactly.

Timing that matters for latency (**verified**, from the compositor's behaviour): the
compositor triggers the event, then immediately takes the last frame submitted. A frame
submitted right after waking on the event therefore misses that pass and waits a whole
refresh (11.1 ms at 90 Hz). The low-latency pattern is to wait for the event, then sleep
until shortly before the *next* pass (the expected period minus the time the app needs)
and submit then. The library also has `sceHmdReprojectionSetUserEventEnd`, triggered at a
later point of the compositor's pass (not used here).

## 7. Tracking: PS Camera and libSceVrTracker

Code: `client/src/camera.cpp`, `tracker.cpp`, `tracker.h`.

The VR tracker fuses the camera images and the IMUs of the headset, the PS Moves and the
DualShock 4. It is GPU-assisted: the app feeds it every camera frame, it runs a GPU pass and
a CPU pass, and the app reads poses whenever it wants.

### Camera

```c
int sceCameraIsAttached(int index /*0*/);                 // 1 when a camera is plugged in
int sceCameraOpen(int32_t user /*0xff*/, int32_t type /*0*/, int32_t index /*0*/, void *param /*NULL*/);
int sceCameraSetConfig(int32_t handle, void *config);     // 0x68 bytes, below
int sceCameraSetVideoSync(int32_t handle, void *param);   // {u32 size 0x10, u32 mode 1, void *option NULL}
int sceCameraStart(int32_t handle, void *param);          // {u32 size 0x18, u32 format_level[2] {0xf, 0xf}, void *option NULL}
int sceCameraGetFrameData(int32_t handle, void *frame);   // 0x248-byte descriptor, below
int sceCameraStop(int32_t handle);
int sceCameraClose(int32_t handle);
```

- Open the camera as user `0xff` (the system), not as the player.
- **Configuration:** 0x68 bytes, zero except `u32 size = 0x68` at +0 and `u32 type` at +4.
  Type 5 goes with tracker profile 100 and type 4 with profile 0. With SDK 5.50, type 5 is
  refused on some camera / console combinations (`0x802e000d`) and accepted on others, but
  with it the tracker then never leaves `NOT_STARTED` (**verified** on both consoles).
  **Type 4 with profile 0 tracks fine everywhere.**
- **Frames:** before each `sceCameraGetFrameData`, zero the 0x248-byte descriptor and set
  `u32 size = 0x248` at +0 and `u32 read mode = 0x11` at +4. The descriptor is then copied
  as is into the tracker's submit parameter.
- Start the camera before initializing the tracker.

### Tracker memory and initialization

```c
struct CalibrationSettings {     // 0x20 bytes
    uint32_t hmd_position;       // 0: manual (the only value accepted for the headset)
    uint32_t pad_position;       // 1: automatic
    uint32_t move_position;      // 1: automatic
    uint32_t gun_position;       // 0 (Aim controller)
    uint32_t reserved[4];
};
struct QueryMemoryParam  { uint32_t size, profile; uint32_t reserved[6]; CalibrationSettings calib; };
struct QueryMemoryResult { uint32_t size, onion_size, onion_alignment, garlic_size, garlic_alignment,
                           work_size, work_alignment; uint32_t reserved[9]; };
int sceVrTrackerQueryMemory(const QueryMemoryParam *p, QueryMemoryResult *r);
```

Sizes reported with profile 0 (**observed**): onion 8 MB, garlic 48 MB, work 16 MB, each
aligned to 64 KB (onion was 4 MB in two runs). Onion and work memory are type 0, garlic is
type 3.

```c
struct InitParam {               // 0x80 bytes
    uint32_t size;               // 0x80
    uint32_t profile;            // 0 (camera config type 4) or 100 (type 5)
    uint32_t execution_mode;     // 1 = parallel (0 = serial)
    int32_t  hmd_thread_priority, pad_thread_priority, move_thread_priority, gun_thread_priority;  // 256
    int32_t  reserved;
    uint64_t cpu_mask;           // 0x38 = cores 3-5
    CalibrationSettings calib;
    void *onion;   uint32_t onion_size, onion_alignment;
    void *garlic;  uint32_t garlic_size, garlic_alignment;
    void *work;    uint32_t work_size, work_alignment;
    int32_t  gpu_pipe_id;        // 4
    int32_t  gpu_queue_id;       // 4
};
int sceVrTrackerInit(const InitParam *p);
int sceVrTrackerTerm(void);
```

The tracker uses GPU compute pipe 4, queue 4 here. Other GPU users in the same app (the
video decoder's compute queue) must pick a different pipe and queue
([section 13](#13-video-decoding-libscevideodec2)).

### Devices

```c
enum { TRACKER_DEVICE_HMD = 0, TRACKER_DEVICE_DUALSHOCK4 = 1, TRACKER_DEVICE_MOVE = 2 };
int sceVrTrackerRegisterDevice(uint32_t device_type, int32_t handle);   // sceHmdOpen / scePadOpen / sceMoveOpen handle
int sceVrTrackerUnregisterDevice(int32_t handle);
```

Facts (**verified**):

- **Only two controllers in all.** Whatever the users and the types, the tracker runs two
  controllers at most besides the headset. A third one is accepted by
  `RegisterDevice` and even gets an LED colour, but stays `NOT_STARTED` forever. Logging a
  second user in does not add room. An app must choose (this project ranks: the player's
  PS Moves, then their DualShock 4, then other users' controllers) and unregister one to
  make room for another.
- **Register a controller once it is connected.** A PS Move registered while asleep kept a
  dark sphere until the tracker was restarted. Poll the controller and register it when it
  starts answering; unregister it when it disconnects.
- **The tracker drives the LEDs.** It picks each controller's colour so the camera can tell
  them apart, and lights the sphere or the light bar itself. Setting the sphere colour from
  the app fights with it and can switch the sphere off. A DualShock 4 that is unregistered
  should get `scePadResetLightBar` so it returns to the system colour.

LED colour indices in the results: 0 blue, 1 red, 2 cyan, 3 magenta, 4 yellow.

### The per-frame loop

Run in a dedicated thread:

```c
for (;;) {
    if (sceCameraGetFrameData(camera, submit.camera_frame_data) == 0) {
        submit.size = sizeof(submit);                      // 0x288
        rc = sceVrTrackerGpuSubmit(&submit);
        if (rc == 0x81260811) { usleep(1000); continue; }  // already processing this camera frame
        if (rc == 0) {
            SmallParam p = { .size = 0x20 };               // everything else zero
            if (sceVrTrackerGpuWait(&p) == 0)
                sceVrTrackerCpuProcess(&p);
            continue;
        }
    }
    // No new camera frame: feed the IMUs of every device type.
    for (type = 0; type <= TRACKER_DEVICE_MOVE; type++) {
        UpdateMotionSensorDataParam m = { .size = 0x20, .device_type = type };  // operation_mode 0, handle 0
        sceVrTrackerUpdateMotionSensorData(&m);
    }
    usleep(2000);
}
```

```c
struct GpuSubmitParam {          // 0x288 bytes
    uint32_t size;
    uint32_t pad_tracking_preference, camera_meta_check_mode, tracking_device_permit_type, robustness_level;  // 0
    uint32_t reserved0[10], reserved1;
    uint8_t  camera_frame_data[0x248];   // the camera descriptor, as filled by sceCameraGetFrameData
};
struct UpdateMotionSensorDataParam {     // 0x20 bytes
    uint32_t size, device_type;
    uint32_t operation_mode;     // 0 = every device of the type (handle 0), 1 = one handle
    int32_t  handle;
    int32_t  reserved[4];
};
```

`sceCameraGetFrameData` returns the same frame again until the camera has a new one (60 Hz);
submitting it twice gives `0x81260811` (ALREADY_PROCESSING_CAMERA_FRAME), which simply means
"wait a millisecond".

### Reading poses

```c
struct GetResultParam {          // 0x38 bytes
    uint32_t size;               // 0x38
    int32_t  handle;             // the device's handle
    uint32_t result_type;        // 0 = predicted, 1 = raw
    uint32_t reserved0;
    uint64_t prediction_time;    // tracker time (µs) the pose is wanted for
    uint32_t orientation_type;   // 0 = absolute
    uint32_t reserved1;
    uint32_t usage_type, user_frame_number, debug_marker_type;
    uint32_t reserved2[2];
};
int sceVrTrackerGetTime(uint64_t *now_us);
int sceVrTrackerGetResult(const GetResultParam *p, void *result);   // result: 0x5f0 bytes (give more room)
```

Result layout (the beginning, which is all this project reads):

```c
struct TrackerPose {             // 0x40 bytes
    float px, py, pz;            // metres, tracker space
    uint32_t reserved0;
    float qx, qy, qz, qw;        // orientation, tracker space
    uint32_t reserved1[8];
};
struct ResultData {
    int32_t  handle;
    uint32_t connected;
    uint32_t reserved0[2];
    uint64_t timestamp;          // tracker time of the pose (µs): the value to pass to the compositor
    uint64_t device_timestamp;
    uint32_t recalibrate_necessity;
    uint32_t playarea_brightness_risk;
    uint32_t reserved1[2];
    uint32_t led_color;          // 0 blue, 1 red, 2 cyan, 3 magenta, 4 yellow
    uint32_t status;             // 0 NOT_STARTED, 1 TRACKING, 2 NOT_TRACKING, 3 CALIBRATING
    uint32_t position_quality;   // 0 NONE, 3 NOT_VISIBLE, 6 PARTIAL, 9 FULL
    uint32_t orientation_quality;
    float velocity[3];           // m/s
    float acceleration[3];
    float angular_velocity[3];   // rad/s
    float angular_acceleration[3];
    float camera_orientation[4];
    // +0x80: device-specific part. For the headset:
    TrackerPose device_pose;     // the headset body
    TrackerPose left_eye_pose;   // left eye, includes the IPD set in the system settings
    TrackerPose right_eye_pose;
    TrackerPose head_pose;
    // ... rest not mapped
};
```

Facts (**verified** unless noted):

- **Qualities.** `FULL` and `PARTIAL` mean the camera sees the device. `NOT_VISIBLE` means
  the position is dead-reckoned from the IMU (it drifts within a second or two); `NONE`
  means no value at all: keep the last one. The orientation stays good out of view (IMU).
- **Eye poses.** For rendering, use the two eye poses, not the device pose: the device pose
  is not at eye level and does not rotate around the neck like the eyes do. The eye poses
  already include the user's IPD from the system settings. The centre between them is a
  good "head" position for a remote renderer.
- **Prediction.** `prediction_time` asks for the pose at a later time. Asking 40-70 ms
  ahead (a streaming latency) gave a pose that jumped at every camera frame (its velocity
  and acceleration estimates jump at each new image), which SteamVR showed as strong
  jitter. Predicting the position yourself from a smoothed velocity (here a 50 ms time
  constant, at most 15 cm ahead) is far steadier. Small predictions (a few ms) are fine.
- **Angular velocity frame.** Not documented. Correlating it with the rotation between
  successive orientations gave the **world frame** (tracker space) for the PS Moves on
  nearly every run (one run was inconclusive). Tracker-reported linear velocities are in
  tracker space too.
- **Start-up sequence.** A freshly started tracker reports the headset as `CALIBRATING`,
  then `NOT_STARTED`, then `TRACKING` once the camera has seen it. Until the first
  `TRACKING` with position quality `FULL`/`PARTIAL`, the pose is not meaningful.
- **The origin is the camera**, but the tracker places it itself; it does not give the
  floor. See [section 15](#15-coordinate-systems-and-conversions) and
  [Open questions](#17-open-questions).
- `recalibrate_necessity` and `playarea_brightness_risk` were not studied.

### Recalibration

```c
struct RecalibrateParam {        // 0x20 bytes
    uint32_t size;               // 0x20
    uint32_t device_type;
    uint32_t calibration_type;   // 2 = everything, 0 = position only
    uint32_t reserved[5];
};
int sceVrTrackerRecalibrate(const RecalibrateParam *p);
```

A full recalibration (type 2) of the headset and of each registered controller type fixes
accumulated gyro drift. The usual moment is when the app comes back from the PS menu
([section 11](#11-system-service-background-ps-menu-closing)). After it, results from just
before can still come out for a short while: ignore sightings for about 300 ms, and treat
the headset as not tracked until the camera sees it again.

### Out of the play area

The system status ([section 11](#11-system-service-background-ps-menu-closing)) has an
`is_out_of_vr_play_area` flag. It toggles as the user leaves and re-enters the camera's
tracking area (**observed**); the system shows its own warning in the headset.

## 8. PS Move: libSceMove

Code: `client/src/move.cpp`, `move.h`, `wand.cpp`.

```c
int sceMoveInit(void);
int sceMoveOpen(int32_t user_id, int32_t type /*0*/, int32_t index /*0 or 1*/);   // handle
int sceMoveGetDeviceInfo(int32_t handle, MoveDeviceInfo *info);   // {float sphere_radius; float accelerometer_offset[3];}
int sceMoveReadStateLatest(int32_t handle, MoveData *data);       // 0 while connected
int sceMoveSetVibration(int32_t handle, uint8_t intensity);
int sceMoveSetLightSphere(int32_t handle, uint8_t r, uint8_t g, uint8_t b);   // do not use with the tracker

struct MoveData {                // 0x40 bytes
    float    accelerometer[3];
    float    gyro[3];
    uint16_t buttons;
    uint16_t trigger;            // 0..255
    uint16_t ext_status, ext_digital0, ext_digital1, ext_analog[4];
    uint8_t  ext_custom[5];
    int64_t  timestamp;
    int32_t  count;
    float    temperature;
};
```

- Each user has two PS Move slots (index 0 and 1); open both at start and poll them: the
  handle stays valid while the controller sleeps, and `ReadStateLatest` starts returning 0
  when it connects.
- `GetDeviceInfo` returned 1 and a zero sphere radius in every test (**observed**), even
  with the controller on. Use the specification: the sphere is 46 mm across.

Button bits, mapped from logged presses (**verified**):

| Bit | Button | Notes |
| --- | --- | --- |
| `0x0001` | SELECT | Taken by the system for screenshots: never bind it. |
| `0x0002` | T | Trigger touched; set from about 16 % of travel. Use the analog value for a click. |
| `0x0004` | MOVE | The large central button. |
| `0x0008` | START | |
| `0x0010` | Triangle | |
| `0x0020` | Circle | |
| `0x0040` | Cross | |
| `0x0080` | Square | |
| `0x8000` | PS | Intercepted by the system (quick menu). |

Physical layout (as seen holding it, sphere up): MOVE button in the centre of the face,
Square top-left, Triangle top-right, Cross bottom-left, Circle bottom-right, PS button below,
START on the right side, SELECT on the left side, trigger at the back.

Tracking facts from height measurements (**verified**, against a tape measure): the point
the tracker reports is not the sphere centre but about 8 mm from it towards the handle; with
the 23 mm sphere radius, a Move resting on its sphere has its tracked point about 3.1 cm
above the surface. The orientation's forward axis runs from the handle to the sphere
(local -Z), the handle along local +Z.

**Rumble** (**verified**): `sceMoveSetVibration(handle, 0..255)`. The controller stops the
motor by itself a few seconds after the last command: a long vibration must be sent again
periodically (every second works). There is no duration parameter: the app switches the
motor off itself at the end of a pulse (this project makes pulses at least 10 ms long).

**Battery:** libSceMove reads it from the controller but throws it away. It can be read
from the raw HID report ([section 10](#10-raw-hid-access-devhid)).

## 9. DualShock 4: libScePad

Code: `client/src/pad.cpp`, `pad.h`.

The standard libScePad calls are enough, from the OpenOrbis headers: `scePadInit()`,
`scePadOpen(user, 0, 0, NULL)`, `scePadReadState`, `scePadGetControllerInformation`,
`scePadSetVibration(handle, {large, small})`, `scePadResetLightBar(handle)`.

- To track it, register its pad handle with the tracker as device type 1. It is tracked by
  its light bar, with a coarser position than a PS Move (fine for showing it, not precise
  enough to measure the floor with, in our tests).
- `scePadGetControllerInformation` reports the touchpad resolution: 1920×942 on our
  controller (**observed**); its result is all zeros while the controller is off.
- The battery level is not in libScePad's data. The pad handle is a HID handle
  ([section 10](#10-raw-hid-access-devhid)), but the PS Move's raw report read does not
  work on it; the battery byte of its decoded state has not been identified.

## 10. Raw HID access: /dev/hid

Code: `client/src/hid.cpp`, `hid.h`, the battery reads in `move.cpp` and `pad.cpp`.

The controller libraries read their devices through `/dev/hid` ioctls, and the handles they
return are HID handles. An app can open `/dev/hid` itself (read-only) and read the same
reports.

**PS Move raw input reports** (**verified**):

```c
struct HidReport {               // 0x40 bytes
    uint64_t timestamp_us;
    uint8_t  data[0x38];         // a kernel flag byte, then the device's input report
};
struct HidReadReports {          // 0x20 bytes
    uint32_t handle;             // the sceMoveOpen handle
    uint32_t pad0;
    HidReport *reports;
    uint32_t max_reports;        // 1
    uint32_t pad1;
    int32_t *device_id;          // set to 0 when the device is not connected
};
int n = ioctl(open("/dev/hid", O_RDONLY), 0xc0204834, &req);   // reports read (0 or 1), or < 0
```

In `data`, the report id is at `data[1]` and the battery at `data[0x0d]`: 0 to 5 (5 = full,
so the hardware only has 20 % steps), `0xEE` charging, `0xEF` charged (on USB). The
temperature is a 12-bit value at `data[0x26] << 4 | data[0x27] >> 4`.

Trap (**verified**): reports are **consumed** when read. Every libSceMove state call drains
the pending reports, so a read right after `sceMoveReadStateLatest` almost always finds the
queue empty, and a read steals a sample from libSceMove. Read the battery rarely (every few
seconds) and **before** the frame's `ReadStateLatest`; a read that returns nothing took
nothing and can be retried on the next frame.

**DualShock 4:** libScePad reads it with another ioctl, `0x8030482e`, argument
(0x30 bytes): `{u32 handle, u32 0, void *entries, u32 max_entries, u32 pad,
s32 *device_id, s32 *status, u64 mode = 2}`. It returns kernel-decoded entries of 0xa8 bytes:
+0x00 timestamp, +0x0c buttons, +0x10 sticks, +0x14 L2/R2, then motion and touch data. The
PS Move ioctl fails on a pad handle (`0x803b0004`).

Under the hood, the libraries obtain those handles from the system's input bus
(libSceMbus: `sceMbusResolveByUserId`, `sceMbusAddHandle`, and the `/dev/hid` ioctl
`0x800c4802` with `{id, 3, 1}`), which an app does not need to touch.

## 11. System service: background, PS menu, closing

Code: `poll_system_events` in `client/src/main.cpp`.

```c
struct SystemServiceStatus {
    int32_t event_num;                   // events waiting for sceSystemServiceReceiveEvent
    bool is_system_ui_overlaid;          // a system overlay is shown over the app
    bool is_in_background_execution;     // the PS menu or another app is in front
    bool is_cpu_mode7_cpu_normal;
    bool is_game_live_streaming_on_air;
    bool is_out_of_vr_play_area;         // the user left the camera's tracking area
    uint8_t reserved[128];
};
int sceSystemServiceGetStatus(SystemServiceStatus *st);
int sceSystemServiceReceiveEvent(void *event);   // {s32 type; u8 param[8192];}
```

- **There is no "about to close" event.** When the user closes the app from the PS menu, the
  system kills the process. Anything that must be restored (the headset leaving VR mode)
  must already be in the system's hands: that is what displaying through the compositor
  achieves. And `sceGnmSubmitDone` must be called every frame
  ([section 2](#the-one-call-every-vr-app-needs-scegnmsubmitdone)).
- **PS button:** the quick menu is drawn in the headset by the system;
  `is_system_ui_overlaid` goes to 1. Opening the home screen puts the app in the background
  (`is_in_background_execution` = 1).
- **Coming back from the background** (1 → 0) is the moment to recalibrate the tracking:
  the user usually took the headset off or turned around meanwhile, and the gyros drifted.
- Event type `0x1000000a` was seen regularly (**observed**, meaning unknown).

## 12. Audio: headset output and microphone

Code: `client/src/audio.cpp`.

- **Output:** `sceAudioOutOpen(0xff /* system user */, port, 0, 256 /* grain */, 48000,
  S16 stereo)`, port MAIN (0), with BGM (1) as fallback. 48 kHz is the only rate. While the
  PSVR is in use, the system plays the app's sound in the headphones plugged into the
  headset; the app does nothing VR-specific.
- **Microphone:** the PSVR's microphone is the system input device while the headset is
  connected. `sceAudioInOpen(user, GENERAL, 0, 256, 48000, mono)` failed with `0x80260103`
  on our setup; `sceAudioInOpen(user, VOICE_CHAT, 0, 256, 16000, mono)` works
  (**verified**): 16 kHz mono, to be resampled if 48 kHz is needed.

## 13. Video decoding: libSceVideodec2

The video decoder has its own page: [The PS4 video decoder](ps4-video-decoder.md) (API,
internals, pipeline depth, measurements, the hardware decoder question, colour conversion
and paced display). The points that matter for a VR app:

- The decoder available to apps (libSceVideodec2, resource type 1) is **software**: CPU
  threads plus GPU compute shaders. About 20-25 ms per picture at 1920×1056, with several
  pictures in flight: pipeline depth 3 sustains 90 frames per second, depth 2 is enough at
  60.
- It needs a GPU compute queue: pick a pipe and queue the tracker (pipe 4, queue 4) does not
  use; pipe 2, queue 0 works (**verified**).
- Output is linear NV12; the compositor needs RGB textures, one per eye, so a conversion
  pass is needed (about 4 ms on four CPU threads at 1920×1056).
- Keep the decoded frame within about 1920×1088; foveated encoding helps render above the
  panel resolution without exceeding it.

## 14. PS4 Pro specifics

```c
int sceKernelHasNeoMode(void);      // 1 on a PS4 Pro
int sceKernelIsAuthenticNeo(void);  // 1 on a PS4 Pro
int sceKernelIsNeoMode(void);       // 1 only when the app runs in Pro mode
```

- A Pro runs an app in base PS4 mode unless its param.sfo has the "PS4 Pro enhanced" bit
  (`0x00800000`, [section 2](#package-flags-paramsfo)); `sceKernelIsNeoMode` tells which.
  `sceKernelHasNeoMode` / `sceKernelIsAuthenticNeo` detect the console in either mode
  (**verified**).
- In base mode on a Pro, `sceKernelGetCpuFrequency` reported 2.1 GHz (**observed**).
- Nothing in this page requires Pro mode; all of it works on a base PS4.

## 15. Coordinate systems and conversions

**Tracker space** (all poses from libSceVrTracker, and the pose given to the compositor):

- origin at the PS Camera, metres;
- +Y up;
- a user facing the camera looks towards -Z (the camera looks towards +Z);
- right-handed; quaternions in x, y, z, w order;
- the floor is **not** given: it is at `y = -(camera height)`, which the app must find
  (ask for the user's height and use the eye height, or touch the floor with a tracked
  controller).

**Finding the floor.** Three ways, all used by this project (**verified** against a
tape measure, one 1.73 m user, so treat the ratios as starting points):

- *From the user's height:* standing straight, the point between the eyes (the midpoint of
  the two eye poses) was at 0.908 × body height above the floor.
- *Touching the floor with a PS Move:* the floor is the tracked position minus 3.1 cm with
  the sphere down (23 mm radius + 8 mm from the tracked point to the sphere centre,
  [section 8](#8-ps-move-libscemove)). It needs the camera to see the floor there, which a
  camera placed high or tilted up may not.
- *Arm span:* arms straight out to the sides, PS Moves pointing straight up: the distance
  between the hands (8 cm behind each sphere along the controller) was 0.868 × body
  height. The controllers must point up: pointed sideways, each sphere is about 10 cm
  further out and the result is about 10 % too tall.

The floor found this way is a height in tracker space, so it survives a restart of the app
as long as the camera does not move; see [Open questions](#17-open-questions) about
recalibrations.

**To OpenXR / SteamVR stage space** (what a PC VR stream expects): same axes, so only the
origin moves: `stage = tracker - (centre_x, floor_y, centre_z)`, with the centre on the
floor where the play area should be. Orientations pass through unchanged.

**Field of view to OpenXR angles:** OpenXR uses signed half angles, left and down negative:

```c
left eye:  angle_left = -atan(tan_out), angle_right = atan(tan_in)
right eye: angle_left = -atan(tan_in),  angle_right = atan(tan_out)
both:      angle_up   =  atan(tan_top), angle_down  = -atan(tan_bottom)
```

**Render size:** the panel gives 960×1080 per eye, but the lens distortion correction
magnifies the centre: rendering at about 130 % (1248×1404) keeps the centre sharp. The
compositor accepts any texture size, as long as the per-eye block
([section 6](#submitting-a-stereo-frame)) describes its field of view.

## 16. Error codes

| Code | Where | Meaning |
| --- | --- | --- |
| `0x81110001` | sceHmdInitialize | Already initialized (harmless) |
| `0x81110003` | sceHmdGetDeviceInformationByHandle | Handle invalid: the headset was power cycled or replugged |
| `0x81260811` | sceVrTrackerGpuSubmit | This camera frame is already being processed: wait for the next one |
| `0x802e000d` | sceCameraSetConfig | Configuration type refused (type 5 on some setups) |
| `0x80B80004` | sce*DialogInitialize | Dialog already initialized (harmless) |
| `0x80960003` | sceUserServiceInitialize | Already initialized (harmless) |
| `0x80260103` | sceAudioInOpen | Port / rate combination refused (GENERAL at 48 kHz here) |
| `0x803b0004` | /dev/hid ioctl `0xc0204834` | Not a device this read applies to (a DualShock 4 handle) |
| `0x811d0100` | sceVideodec2CreateDecoder | API failure inside the decoder core (the decoder's codes: [decoder page](ps4-video-decoder.md#11-error-codes)) |
| `0x811d0200` | sceVideodec2QueryDecoderMemoryInfo | Configuration refused for this resource type |
| `0x80c10015` | sceVideodecDecode (v1) | Fatal state; seen when the stream's level is above the declared one (no free picture slot) |
| CE-34878-0 | System | The app could not be closed: it never called `sceGnmSubmitDone` |

## 17. Open questions

- **Floor height after a recalibration.** The camera is the tracking origin, so a floor
  height measured once should stay valid. In practice it seems to move by a few
  centimetres after each tracking reset (recalibration, coming back from the PS menu). It is
  not settled whether the tracker's origin shifts slightly at each recalibration (the
  camera's own orientation estimate changing) or whether something else is at play. On
  2026-10-02, standing still through three resets in a row, the neck height the tracker
  reported moved by -9.8, +3.4 and +9.4 cm (**measured**): the app now re-anchors the floor
  on it, which is exact where the user stood; a tilt estimate off by a degree still moves
  the floor by about 2 cm per metre elsewhere in the room.
- The meaning of the compositor's `mode_a`, `mode_b`, `type`, `mode` and `flags` values,
  and of the `unk28` / `unk30` fields of its pose.
- The VR service dialog's modes 1 and 2.
- The DualShock 4 battery byte in the HID state, and why `sceMoveGetDeviceInfo` never
  returned the sphere radius.
- Whether camera configuration type 5 (with tracker profile 100) works with another SDK
  version or camera model, and what it would bring.
