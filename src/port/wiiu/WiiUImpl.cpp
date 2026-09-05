#ifdef __WIIU__
#include <port/wiiu/WiiUImpl.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/iosupport.h>

#include <whb/log.h>
#include <whb/log_udp.h>
#include <coreinit/debug.h>

#include <ship/window/Window.h>
#include <ship/Context.h>

namespace Ship {
namespace WiiU {

static bool hasVpad = false;
static VPADReadError vpadError;
static VPADStatus vpadStatus;

static bool hasKpad[4] = { false };
static KPADError kpadError[4] = { KPAD_ERROR_OK };
static KPADStatus kpadStatus[4];

#if 1 /* force UDP logging: no other way to diagnose on-device */
extern "C" {
void __wrap_abort() {
    printf("Abort called.\n");
    // force a stack trace
    *(uint32_t*)0xdeadc0de = 0xcafebabe;
    while (1)
        ;
}

static ssize_t wiiu_log_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%*.*s", len, len, ptr);
    OSReport(buf);
    WHBLogWritef("%*.*s", len, len, ptr);
    return len;
}

static const devoptab_t dotab_stdout = {
    .name = "stdout_whb",
    .write_r = wiiu_log_write,
};
};
#endif

void Init(const std::string& shortName) {
#if 1 /* force UDP logging: no other way to diagnose on-device */
    WHBLogUdpInit();
    WHBLogPrint("Hello World!");

    devoptab_list[STD_OUT] = &dotab_stdout;
    devoptab_list[STD_ERR] = &dotab_stdout;
#endif

    // make sure the required folders exist
    mkdir("/vol/external01/wiiu/", 0755);
    mkdir("/vol/external01/wiiu/apps/", 0755);
    mkdir(("/vol/external01/wiiu/apps/" + shortName).c_str(), 0755);

    chdir(("/vol/external01/wiiu/apps/" + shortName).c_str());

    KPADInit();
    WPADEnableURCC(true);
}

void Exit() {
    KPADShutdown();

    WHBLogUdpDeinit();
}

void ThrowMissingOTR(const char* otrPath) {
    // TODO handle this better in the future
    OSFatal("Main OTR file not found!");
}

void ThrowInvalidOTR() {
    OSFatal("Invalid OTR files! Try regenerating them!");
}

void Update() {
    bool rescan = false;

    VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &vpadError);
    if (vpadError == VPAD_READ_SUCCESS) {
        if (!hasVpad) {
            rescan = true;
        }

        hasVpad = true;
    } else if (vpadError != VPAD_READ_NO_SAMPLES) {
        if (hasVpad) {
            rescan = true;
        }

        hasVpad = false;
    }

    for (int i = 0; i < 4; i++) {
        KPADReadEx((KPADChan)i, &kpadStatus[i], 1, &kpadError[i]);
        if (kpadError[i] == KPAD_ERROR_OK && kpadStatus[i].extensionType != 255) {
            if (!hasKpad[i]) {
                rescan = true;
            }

            hasKpad[i] = true;
        } else if (kpadError[i] != KPAD_ERROR_NO_SAMPLES) {
            if (hasKpad[i]) {
                rescan = true;
            }

            hasKpad[i] = false;
        }
    }

    // Old libultraship exposed ControlDeck::ScanDevices() to re-enumerate on hotplug.
    // Modern libultraship has no such method - there is nothing named Scan/Refresh/Reload
    // Devices anywhere in the tree. The Wii U's VPAD/KPAD are polled directly above every
    // frame, so the connection-state tracking is all the rescan we need; `rescan` is kept
    // because the branches above are the hotplug bookkeeping.
    (void)rescan;
}

VPADStatus* GetVPADStatus(VPADReadError* error) {
    *error = vpadError;
    return hasVpad ? &vpadStatus : nullptr;
}

KPADStatus* GetKPADStatus(WPADChan chan, KPADError* error) {
    *error = kpadError[chan];
    return hasKpad[chan] ? &kpadStatus[chan] : nullptr;
}

}; // namespace WiiU
}; // namespace Ship

#endif
