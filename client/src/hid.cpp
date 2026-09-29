#include "hid.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include "log.h"

struct HidReadReports {
    uint32_t handle;
    uint32_t pad0;
    HidReport *reports;
    uint32_t max_reports;
    uint32_t pad1;
    int32_t *device_id;
};
static_assert(sizeof(HidReadReports) == 0x20, "ioctl 0xc0204834 argument size");
static_assert(sizeof(HidReport) == 0x40, "HidReport size");

int hid_read_report(int handle, HidReport *report, int32_t *device_id)
{
    static int fd = -2;
    if (fd == -2) {
        fd = open("/dev/hid", O_RDONLY);
        LOG("hid: open(/dev/hid) -> %d", fd);
    }
    *device_id = 0;
    if (fd < 0)
        return -1;
    HidReadReports req;
    memset(&req, 0, sizeof(req));
    memset(report, 0, sizeof(*report));
    req.handle = (uint32_t)handle;
    req.reports = report;
    req.max_reports = 1;
    req.device_id = device_id;
    return ioctl(fd, 0xc0204834, &req);
}
