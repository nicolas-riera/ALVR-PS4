#pragma once

// System "connect your PlayStation VR" dialog (libSceHmdSetupDialog), shown while the
// headset is not ready: not connected, powered off, processor unit USB unplugged.
// Structures and behaviour from the dumped module (reference/decomp/hmdsetupdialog_*.c);
// usage modelled on VR Worlds (reference/decomp/vrworlds_setupdialog*.c): the dialog is
// reopened when the user cancels it, until the headset is ready.

enum HmdSetupState {
    HMD_SETUP_IDLE,     // not running
    HMD_SETUP_RUNNING,  // on screen (or reopened after a cancel)
    HMD_SETUP_DONE,     // finished with OK: the headset is ready
    HMD_SETUP_FAILED,   // could not be opened
};

// Resolves the functions and calls sceCommonDialogInitialize.
bool hmd_setup_init(int common_dialog_module, int setup_dialog_module);
// Opens the dialog for the user if it is not running. When the headset is already
// ready the system finishes it at once without showing anything.
bool hmd_setup_start(int user_id);
// Call every frame while running. DONE and FAILED are returned once, then IDLE.
HmdSetupState hmd_setup_poll();
bool hmd_setup_running();

// System VR service dialog (libSceVrServiceDialog), mode 0 as VR Worlds opens it
// (reference/decomp/vrservicedialog.c; VR Worlds eboot at 0x14f8bd9): the PS VR "confirm
// your position" screen, which gets the camera to find the headset.
bool vr_service_dialog_init(int module);
bool vr_service_dialog_open();
bool vr_service_dialog_running();
// Call every frame; ends the dialog once the system has finished it.
void vr_service_dialog_poll();
