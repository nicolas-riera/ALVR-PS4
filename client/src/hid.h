#pragma once

#include <stdint.h>

// Raw HID input reports from /dev/hid, as libSceMove reads them
// (reference/decomp/move_battery.c). Each entry is a kernel flag byte followed by the
// device's input report. Reports are consumed when read, so a poll steals one sample from
// the library that owns the device.
struct HidReport {
    uint64_t timestamp_us;
    uint8_t data[0x38];
};

// Latest pending report of a HID handle. Returns the number of reports read (0 or 1), or
// a negative error; *device_id is 0 when the device is not connected.
int hid_read_report(int handle, HidReport *report, int32_t *device_id);
