#ifdef __WIIU__

#include <stdint.h>

#include <padscore/kpad.h>
#include <vpad/input.h>
#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"

extern "C" {
int32_t __real_VPADRead(VPADChan chan, VPADStatus* buffers, uint32_t count, VPADReadError* outError);
uint32_t __real_KPADReadEx(KPADChan chan, KPADStatus* data, uint32_t count, KPADError* error);
}

namespace {

constexpr int VpadChannelCount = 2;
constexpr int KpadChannelCount = 7;
constexpr int ExtensionCount = 3;
constexpr int CallerCount = 16;
constexpr uint32_t DrainBufferSize = 16;

struct VpadCache {
    bool valid = false;
    VPADStatus newest{};
};

struct KpadCache {
    bool valid = false;
    KPADStatus newest{};
};

struct CallerState {
    const void* address = nullptr;
    bool used = false;

    uint32_t vpadPending[VpadChannelCount] = {};
    uint32_t vpadLast[VpadChannelCount] = {};

    uint32_t kpadCorePending[KpadChannelCount] = {};
    uint32_t kpadCoreLast[KpadChannelCount] = {};
    uint32_t kpadExtensionPending[KpadChannelCount][ExtensionCount] = {};
    uint32_t kpadExtensionLast[KpadChannelCount][ExtensionCount] = {};
};

struct RuntimeState {
    VpadCache vpad[VpadChannelCount];
    KpadCache kpad[KpadChannelCount];
    CallerState callers[CallerCount];
    uint32_t vpadCallSerial[VpadChannelCount] = {};
    uint32_t vpadRecentSerial[VpadChannelCount] = {};
    uint32_t vpadRecentHold[VpadChannelCount] = {};
    bool vpadRecentValid[VpadChannelCount] = {};
    uint32_t kpadCallSerial[KpadChannelCount] = {};
    uint32_t kpadRecentSerial[KpadChannelCount] = {};
    uint32_t kpadRecentCoreHold[KpadChannelCount] = {};
    uint32_t kpadRecentExtensionHold[KpadChannelCount][ExtensionCount] = {};
    bool kpadRecentValid[KpadChannelCount] = {};
    uint32_t cvarCalls = 0;
    bool enabled = true;
    bool logged = false;
};

RuntimeState runtime;

void ResetInputState() {
    for (VpadCache& cache : runtime.vpad) {
        cache.valid = false;
    }
    for (KpadCache& cache : runtime.kpad) {
        cache.valid = false;
    }
    for (CallerState& caller : runtime.callers) {
        caller = CallerState{};
    }
    for (int channel = 0; channel < VpadChannelCount; ++channel) {
        runtime.vpadCallSerial[channel] = 0;
        runtime.vpadRecentSerial[channel] = 0;
        runtime.vpadRecentHold[channel] = 0;
        runtime.vpadRecentValid[channel] = false;
    }
    for (int channel = 0; channel < KpadChannelCount; ++channel) {
        runtime.kpadCallSerial[channel] = 0;
        runtime.kpadRecentSerial[channel] = 0;
        runtime.kpadRecentCoreHold[channel] = 0;
        runtime.kpadRecentValid[channel] = false;
        for (int extension = 0; extension < ExtensionCount; ++extension) {
            runtime.kpadRecentExtensionHold[channel][extension] = 0;
        }
    }
}

bool IsInputEdgeFixEnabled() {
    if ((runtime.cvarCalls++ & 0xFFu) == 0) {
        const auto context = Ship::Context::GetInstance();
        if (context != nullptr) {
            const auto consoleVariables = context->GetConsoleVariables();
            if (consoleVariables != nullptr) {
                const bool enabled = consoleVariables->GetInteger("gWiiU.InputEdgeFix", 1) != 0;
                if (enabled != runtime.enabled) {
                    ResetInputState();
                    runtime.enabled = enabled;
                }
            }
        }
    }
    return runtime.enabled;
}

CallerState* GetCallerState(const void* address) {
    CallerState* freeSlot = nullptr;
    for (CallerState& caller : runtime.callers) {
        if (caller.used && caller.address == address) {
            return &caller;
        }
        if (!caller.used && freeSlot == nullptr) {
            freeSlot = &caller;
        }
    }

    if (freeSlot != nullptr) {
        freeSlot->used = true;
        freeSlot->address = address;
        for (int channel = 0; channel < VpadChannelCount; ++channel) {
            if (runtime.vpadRecentValid[channel] &&
                runtime.vpadCallSerial[channel] - runtime.vpadRecentSerial[channel] <= 1) {
                freeSlot->vpadPending[channel] = runtime.vpadRecentHold[channel];
            }
        }
        for (int channel = 0; channel < KpadChannelCount; ++channel) {
            if (runtime.kpadRecentValid[channel] &&
                runtime.kpadCallSerial[channel] - runtime.kpadRecentSerial[channel] <= 1) {
                freeSlot->kpadCorePending[channel] = runtime.kpadRecentCoreHold[channel];
                for (int extension = 0; extension < ExtensionCount; ++extension) {
                    freeSlot->kpadExtensionPending[channel][extension] =
                        runtime.kpadRecentExtensionHold[channel][extension];
                }
            }
        }
        return freeSlot;
    }

    // There are only a handful of Wii U input call sites. Preserve a stable
    // fallback if a third-party caller exhausts the bounded table.
    return &runtime.callers[0];
}

void ObserveVpadHold(int channel, uint32_t hold) {
    for (CallerState& caller : runtime.callers) {
        if (caller.used) {
            caller.vpadPending[channel] |= hold;
        }
    }
}

int ExtensionIndex(uint8_t extensionType) {
    switch (extensionType) {
        case WPAD_EXT_NUNCHUK:
            return 0;
        case WPAD_EXT_CLASSIC:
            return 1;
        case WPAD_EXT_PRO_CONTROLLER:
            return 2;
        default:
            return -1;
    }
}

uint32_t ExtensionHold(const KPADStatus& status, int extension) {
    switch (extension) {
        case 0:
            return status.nunchuk.hold;
        case 1:
            return status.classic.hold;
        case 2:
            return status.pro.hold;
        default:
            return 0;
    }
}

void SetExtensionEdges(KPADStatus& status, int extension, uint32_t hold, uint32_t last) {
    switch (extension) {
        case 0:
            status.nunchuk.hold = hold;
            status.nunchuk.trigger = hold & ~last;
            status.nunchuk.release = last & ~hold;
            break;
        case 1:
            status.classic.hold = hold;
            status.classic.trigger = hold & ~last;
            status.classic.release = last & ~hold;
            break;
        case 2:
            status.pro.hold = hold;
            status.pro.trigger = hold & ~last;
            status.pro.release = last & ~hold;
            break;
        default:
            break;
    }
}

void ObserveKpadStatus(int channel, const KPADStatus& status) {
    const int extension = ExtensionIndex(status.extensionType);
    for (CallerState& caller : runtime.callers) {
        if (caller.used) {
            caller.kpadCorePending[channel] |= status.hold;
            if (extension >= 0) {
                caller.kpadExtensionPending[channel][extension] |= ExtensionHold(status, extension);
            }
        }
    }
}

void LogActiveOnce() {
    if (!runtime.logged) {
        SPDLOG_INFO("INPUTWRAP: active");
        runtime.logged = true;
    }
}

} // namespace

extern "C" int32_t __wrap_VPADRead(VPADChan chan, VPADStatus* buffers, uint32_t count,
                                    VPADReadError* outError) {
    if (count != 1 || buffers == nullptr || outError == nullptr || !IsInputEdgeFixEnabled()) {
        return __real_VPADRead(chan, buffers, count, outError);
    }

    LogActiveOnce();

    const int channel = static_cast<int>(chan);
    if (channel < 0 || channel >= VpadChannelCount) {
        return __real_VPADRead(chan, buffers, count, outError);
    }

    ++runtime.vpadCallSerial[channel];
    CallerState* caller = GetCallerState(__builtin_return_address(0));
    VPADStatus samples[DrainBufferSize];
    VPADReadError realError = VPAD_READ_NO_SAMPLES;
    bool receivedSample = false;
    uint32_t recentHold = 0;

    for (;;) {
        const int32_t sampleCount = __real_VPADRead(chan, samples, DrainBufferSize, &realError);
        const int32_t boundedCount = sampleCount > static_cast<int32_t>(DrainBufferSize)
                                         ? static_cast<int32_t>(DrainBufferSize)
                                         : sampleCount;

        if (realError != VPAD_READ_SUCCESS && realError != VPAD_READ_NO_SAMPLES) {
            *outError = realError;
            return sampleCount;
        }

        if (boundedCount > 0) {
            if (!receivedSample) {
                runtime.vpad[channel].newest = samples[0];
                runtime.vpad[channel].valid = true;
                receivedSample = true;
            }
            for (int32_t i = 0; i < boundedCount; ++i) {
                ObserveVpadHold(channel, samples[i].hold);
                recentHold |= samples[i].hold;
            }
        }

        if (boundedCount < static_cast<int32_t>(DrainBufferSize)) {
            break;
        }
    }

    if (receivedSample) {
        runtime.vpadRecentHold[channel] = recentHold;
        runtime.vpadRecentSerial[channel] = runtime.vpadCallSerial[channel];
        runtime.vpadRecentValid[channel] = true;
    }

    if (!runtime.vpad[channel].valid) {
        *outError = realError;
        return 0;
    }

    VPADStatus result = runtime.vpad[channel].newest;
    const uint32_t last = caller->vpadLast[channel];
    const uint32_t hold = result.hold | (caller->vpadPending[channel] & ~last);
    result.hold = hold;
    result.trigger = hold & ~last;
    result.release = last & ~hold;
    caller->vpadLast[channel] = hold;
    caller->vpadPending[channel] = 0;

    *buffers = result;
    *outError = VPAD_READ_SUCCESS;
    return 1;
}

extern "C" uint32_t __wrap_KPADReadEx(KPADChan chan, KPADStatus* data, uint32_t count, KPADError* error) {
    if (count != 1 || data == nullptr || error == nullptr || !IsInputEdgeFixEnabled()) {
        return __real_KPADReadEx(chan, data, count, error);
    }

    LogActiveOnce();

    const int channel = static_cast<int>(chan);
    if (channel < 0 || channel >= KpadChannelCount) {
        return __real_KPADReadEx(chan, data, count, error);
    }

    ++runtime.kpadCallSerial[channel];
    CallerState* caller = GetCallerState(__builtin_return_address(0));
    KPADStatus samples[DrainBufferSize];
    KPADError realError = KPAD_ERROR_NO_SAMPLES;
    bool receivedSample = false;
    uint32_t recentCoreHold = 0;
    uint32_t recentExtensionHold[ExtensionCount] = {};

    for (;;) {
        const uint32_t sampleCount = __real_KPADReadEx(chan, samples, DrainBufferSize, &realError);
        const uint32_t boundedCount = sampleCount > DrainBufferSize ? DrainBufferSize : sampleCount;

        if (realError != KPAD_ERROR_OK && realError != KPAD_ERROR_NO_SAMPLES) {
            *error = realError;
            return sampleCount;
        }

        if (boundedCount > 0) {
            if (!receivedSample) {
                runtime.kpad[channel].newest = samples[0];
                runtime.kpad[channel].valid = true;
                receivedSample = true;
            }
            for (uint32_t i = 0; i < boundedCount; ++i) {
                ObserveKpadStatus(channel, samples[i]);
                recentCoreHold |= samples[i].hold;
                const int extension = ExtensionIndex(samples[i].extensionType);
                if (extension >= 0) {
                    recentExtensionHold[extension] |= ExtensionHold(samples[i], extension);
                }
            }
        }

        if (boundedCount < DrainBufferSize) {
            break;
        }
    }

    if (receivedSample) {
        runtime.kpadRecentCoreHold[channel] = recentCoreHold;
        for (int extension = 0; extension < ExtensionCount; ++extension) {
            runtime.kpadRecentExtensionHold[channel][extension] = recentExtensionHold[extension];
        }
        runtime.kpadRecentSerial[channel] = runtime.kpadCallSerial[channel];
        runtime.kpadRecentValid[channel] = true;
    }

    if (!runtime.kpad[channel].valid) {
        *error = realError;
        return 0;
    }

    KPADStatus result = runtime.kpad[channel].newest;
    const uint32_t coreLast = caller->kpadCoreLast[channel];
    const uint32_t coreHold = result.hold | (caller->kpadCorePending[channel] & ~coreLast);
    result.hold = coreHold;
    result.trigger = coreHold & ~coreLast;
    result.release = coreLast & ~coreHold;
    caller->kpadCoreLast[channel] = coreHold;
    caller->kpadCorePending[channel] = 0;

    const int extension = ExtensionIndex(result.extensionType);
    if (extension >= 0) {
        const uint32_t extensionLast = caller->kpadExtensionLast[channel][extension];
        const uint32_t extensionHold = ExtensionHold(result, extension) |
                                        (caller->kpadExtensionPending[channel][extension] & ~extensionLast);
        SetExtensionEdges(result, extension, extensionHold, extensionLast);
        caller->kpadExtensionLast[channel][extension] = extensionHold;
        caller->kpadExtensionPending[channel][extension] = 0;
    }

    *data = result;
    *error = KPAD_ERROR_OK;
    return 1;
}

#endif
