#ifdef __WIIU__
#include <malloc.h>
#include "port/wiiu/WiiUImpl.h"

#include <exception>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <typeinfo>
#include <unistd.h>
#include <sys/iosupport.h>

#include <whb/log.h>
#include <whb/log_udp.h>
#include <coreinit/debug.h>

#include <ship/window/Window.h>
#include <ship/Context.h>

#include "port/wiiu/WiiUWatchdog.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/exception.h>
#include <sys/socket.h>
#include <coreinit/memheap.h>
#include <coreinit/memexpheap.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <coreinit/core.h>
#include <sysapp/launch.h>
#include <nsysnet/_socket.h>
#include <cstdlib>

static int wiiu_log_open(struct _reent*, void*, const char*, int, int) {
    return -1;
}

static int wiiu_log_close(struct _reent*, void*) {
    return 0;
}

static int wiiu_log_fstat(struct _reent*, void*, struct stat* st) {
    memset(st, 0, sizeof(*st));
    return 0;
}

static ssize_t wiiu_log_discard(struct _reent*, void*, const char*, size_t len) {
    return len;
}

static const devoptab_t dotab_devnull = {
    .name = "devnull_wiiu",
    .open_r = wiiu_log_open,
    .close_r = wiiu_log_close,
    .write_r = wiiu_log_discard,
    .fstat_r = wiiu_log_fstat,
};

extern "C" {
void __real___cxa_throw(void* exception, std::type_info* typeInfo, void (*destructor)(void*))
    __attribute__((noreturn));
extern void __init(void);

static void RestoreStdioDevoptab() {
    // In the linked ELF, devoptab_list[STD_IN/OUT/ERR] default to &dotab_stdnull.
    // Its measured open_r, close_r, read_r, and fstat_r slots are all NULL, so
    // restoring the original entries would hand those NULL slots back during teardown.
    devoptab_list[STD_OUT] = &dotab_devnull;
    devoptab_list[STD_ERR] = &dotab_devnull;
}

// Every SohGui::RegisterPopup OK handler calls exit(). On Cafe OS that unwinds atexit
// handlers and static destructors while the GX2 context and ProcUI are still live, and
// something in that teardown never returns - the console hangs and needs a power cycle.
// Same class of problem as abort() in the terminate handler, and the same remedy: do the
// small amount of teardown that is safe, then _Exit so the system reclaims the title
// normally and the Aroma plugins survive.
void __real_exit(int status) __attribute__((noreturn));
void __wrap_exit(int status) {
    Ship::WiiU::Watchdog::Emit("EXIT: exit(%d) - bypassing destructor teardown\n", status);
    KPADShutdown();
    RestoreStdioDevoptab();
    WHBLogUdpDeinit();
    _Exit(status);
}

void __wrap___cxa_throw(void* exception, std::type_info* typeInfo, void (*destructor)(void*)) {
    // One frame is not enough: __cxa_throw is called BY std::__throw_out_of_range, so
    // __builtin_return_address(0) only ever names that helper. Walk the PowerPC
    // back-chain instead - r1 points at a frame whose first word is the caller's frame
    // and whose second word is that frame's saved LR - to get the real call path.
    //
    // A constant tied to this translation unit went stale on every rebuild:
    // 0x049057c4 -> 0x0490581c -> 0x04904fc8. __init only moves if the crt layout in
    // the linker script moves, so derive the bias from its fixed head-of-.text address.
    const uint32_t bias = (uint32_t)(uintptr_t)&__init - 0x02000080u;

    uint32_t* sp;
    __asm__ volatile("mr %0, 1" : "=r"(sp));

    char line[512];
    int n = snprintf(line, sizeof(line), "CXX: throw type=%s bias=0x%08X frames:", typeInfo->name(), bias);
    for (int depth = 0; depth < 8 && sp != nullptr && n < (int)sizeof(line) - 16; ++depth) {
        uint32_t* next = (uint32_t*)sp[0];
        if (next <= sp || (uintptr_t)next & 3u) {
            break;
        }
        const uint32_t lr = next[1];
        if (lr > bias) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%08X", lr - bias);
        }
        sp = next;
    }
    Ship::WiiU::Watchdog::Emit("%s\n", line);
    __real___cxa_throw(exception, typeInfo, destructor);
}
}

namespace Ship {
namespace WiiU {

static bool hasVpad = false;
static VPADReadError vpadError;
static VPADStatus vpadStatus;

static bool hasKpad[4] = { false };
static KPADError kpadError[4] = { KPAD_ERROR_OK };
static KPADStatus kpadStatus[4];

// This is the only storage used by the exception path.  It must remain independent of
// the heap, locks, sockets, and the game's renderer: OSFatal owns the last step.
static char sExceptionMessage[768];
static const char sExceptionHexDigits[] = "0123456789ABCDEF";
static const char* const sExceptionGprNames[] = {
    "GPR0",  "GPR1",  "GPR2",  "GPR3",  "GPR4",  "GPR5",  "GPR6",
    "GPR7",  "GPR8",  "GPR9",  "GPR10", "GPR11", "GPR12",
};

static char* AppendExceptionText(char* destination, const char* source) {
    while (*source != '\0') {
        *destination++ = *source++;
    }
    return destination;
}

static char* AppendExceptionHex(char* destination, uint32_t value) {
    *destination++ = '0';
    *destination++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        *destination++ = sExceptionHexDigits[(value >> shift) & 0xF];
    }
    return destination;
}

static char* AppendExceptionRegister(char* destination, const char* name, uint32_t value) {
    destination = AppendExceptionText(destination, name);
    *destination++ = '=';
    destination = AppendExceptionHex(destination, value);
    *destination++ = ' ';
    return destination;
}

static void EmitExceptionMessage(const char* typeName, const OSContext* context) {
    // Emit formats into a 384-byte stack-local buffer, so sequential calls keep their own
    // text. Emit the register evidence before OSGetSymbolName: symbol lookup may stall or
    // fault in the exception context. The GPRs still reach the screen via OSFatal.
    if (context == nullptr) {
        Watchdog::Emit("EXC: type=%s context=NULL\n", typeName);
        return;
    }

    uint32_t* sp = (uint32_t*)(uintptr_t)context->gpr[1];
    uint32_t caller = 0;
    do {
        if (sp == nullptr || (uintptr_t)sp & 3u) {
            break;
        }
        uint32_t* next = (uint32_t*)sp[0];
        if (next <= sp || (uintptr_t)next & 3u) {
            break;
        }
        caller = next[1];
    } while (false);

    Watchdog::Emit("EXC: %s srr0=0x%08X dar=0x%08X dsisr=0x%08X srr1=0x%08X lr=0x%08X r1=0x%08X core=%u\n",
                   typeName, context->srr0, context->dar, context->dsisr, context->srr1, context->lr,
                   context->gpr[1], (unsigned int)OSGetCoreId());

    char lrSymbol[64] = {};
    char callerSymbol[64] = {};
    OSGetSymbolName(context->lr, lrSymbol, sizeof(lrSymbol));
    lrSymbol[sizeof(lrSymbol) - 1] = '\0';
    if (caller != 0) {
        OSGetSymbolName(caller, callerSymbol, sizeof(callerSymbol));
        callerSymbol[sizeof(callerSymbol) - 1] = '\0';
    }
    Watchdog::Emit("EXC-SYM: lr=0x%08X %s caller=0x%08X %s\n", context->lr, lrSymbol, caller, callerSymbol);
}

static void FormatExceptionMessage(const char* typeName, const OSContext* context) {
    char* destination = sExceptionMessage;
    destination = AppendExceptionText(destination, "Wii U ");
    destination = AppendExceptionText(destination, typeName);
    destination = AppendExceptionText(destination, " exception\n");

    if (context == nullptr) {
        destination = AppendExceptionText(destination, "exception context unavailable\n");
        *destination = '\0';
        return;
    }

    destination = AppendExceptionRegister(destination, "SRR0", context->srr0);
    destination = AppendExceptionRegister(destination, "SRR1", context->srr1);
    *destination++ = '\n';
    destination = AppendExceptionRegister(destination, "DAR", context->dar);
    destination = AppendExceptionRegister(destination, "DSISR", context->dsisr);
    *destination++ = '\n';
    destination = AppendExceptionRegister(destination, "LR", context->lr);
    *destination++ = '\n';

    // r0-r12 cover the volatile call state, stack pointer, TOC, and argument registers.
    for (int gpr = 0; gpr <= 12; ++gpr) {
        destination = AppendExceptionRegister(destination, sExceptionGprNames[gpr], context->gpr[gpr]);
        if (gpr == 3 || gpr == 6 || gpr == 9 || gpr == 12) {
            *destination++ = '\n';
        }
    }
    *destination = '\0';
}

static BOOL FatalException(const char* typeName, OSContext* context) {
    FormatExceptionMessage(typeName, context);
    EmitExceptionMessage(typeName, context);
    OSFatal(sExceptionMessage);
    return FALSE;
}

static BOOL DsiExceptionCallback(OSContext* context) {
    return FatalException("DSI", context);
}

static BOOL IsiExceptionCallback(OSContext* context) {
    return FatalException("ISI", context);
}

static BOOL ProgramExceptionCallback(OSContext* context) {
    return FatalException("PROGRAM", context);
}

static BOOL MachineCheckExceptionCallback(OSContext* context) {
    return FatalException("MACHINE_CHECK", context);
}

static BOOL AlignmentExceptionCallback(OSContext* context) {
    return FatalException("ALIGNMENT", context);
}

static BOOL FloatingPointExceptionCallback(OSContext* context) {
    return FatalException("FLOATING_POINT", context);
}

// All six types the previous socket-based version covered. ALIGNMENT in particular is
// worth keeping on PowerPC: this port reads little-endian archive data, and a misaligned
// or swapped access is a live failure mode here, not a theoretical one.
//
// OSSetExceptionCallbackEx(GLOBAL_ALL_CORES) replaces the old per-core OSSetExceptionCallback,
// which needed a thread spawned on each of the three cores to install itself.
static void InstallExceptionCallbacks() {
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, DsiExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, IsiExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, ProgramExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_MACHINE_CHECK,
                             MachineCheckExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT,
                             AlignmentExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_FLOATING_POINT,
                             FloatingPointExceptionCallback);
}

static OSThread sCrashTestThread;
static uint8_t sCrashTestStack[4 * 1024] __attribute__((aligned(16)));

static int CrashTestMain(int, const char**) {
    OSSleepTicks(OSMillisecondsToTicks(1000));
    Watchdog::Emit("EXC: CRASHTEST triggering DSI\n");
    *(volatile uint32_t*)0xdeadc0d0 = 0xcafebabe;
    return 0;
}

static void ArmCrashTestIfRequested() {
    // Init has already chdir'd into /vol/external01/wiiu/apps/<shortName>, and "sd:" is not a
    // mount wut provides - use the real device path so a missing file means "not requested"
    // rather than "wrong path".
    FILE* marker = fopen("CRASHTEST", "rb");
    if (marker == nullptr) {
        return;
    }
    fclose(marker);

    Watchdog::Emit("EXC: CRASHTEST found; DSI scheduled in 1000ms\n");
    if (!OSCreateThread(&sCrashTestThread, CrashTestMain, 0, nullptr,
                        sCrashTestStack + sizeof(sCrashTestStack), sizeof(sCrashTestStack), 5,
                        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
        Watchdog::Emit("EXC: CRASHTEST thread creation failed\n");
        return;
    }
    OSSetThreadName(&sCrashTestThread, "Wii U crash test");
    OSResumeThread(&sCrashTestThread);
}

#if 1 /* force UDP logging: no other way to diagnose on-device */
extern "C" {
void __wrap_abort() {
    // An assert() or abort() fired. printf() here only reaches the devoptab/UDP path, which is
    // exactly the channel that keeps failing on this platform - so write the fact to the SD card
    // too, next to the spdlog file, where it survives the freeze and the reboot that follows.
    printf("Abort called.\n");
    FILE* f = fopen("abort.log", "a");
    if (f) {
        fprintf(f, "abort() called - an assert fired. See the tail of the game log for the last\n"
                   "successful operation; the assert is in whatever ran immediately after it.\n");
        fflush(f);
        fclose(f);
    }
    // Also try to flush whatever spdlog still holds, so the game log is complete.
    fflush(NULL);
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
    .open_r = wiiu_log_open,
    .close_r = wiiu_log_close,
    .write_r = wiiu_log_write,
    .fstat_r = wiiu_log_fstat,
};
};
#endif

void Init(const std::string& shortName) {
    // Called twice now: once at the very top of InitOTR (so logging and the chdir are
    // up before ANY filesystem access) and once where upstream has always called it,
    // at the tail of RunExtract. Second call is a no-op.
    static bool sInitialised = false;
    if (sInitialised) {
        return;
    }
    sInitialised = true;

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

    Watchdog::Start();
    InstallExceptionCallbacks();
    Watchdog::Emit("EXC: exception handlers installed successfully\n");
    ArmCrashTestIfRequested();
}

void Exit() {
    KPADShutdown();

    RestoreStdioDevoptab();
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
    VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &vpadError);
    if (vpadError == VPAD_READ_SUCCESS) {
        hasVpad = true;
    } else if (vpadError != VPAD_READ_NO_SAMPLES) {
        hasVpad = false;
    }

    for (int i = 0; i < 4; i++) {
        KPADReadEx((KPADChan)i, &kpadStatus[i], 1, &kpadError[i]);
        if (kpadError[i] == KPAD_ERROR_OK && kpadStatus[i].extensionType != 255) {
            hasKpad[i] = true;
        } else if (kpadError[i] != KPAD_ERROR_NO_SAMPLES) {
            hasKpad[i] = false;
        }
    }
}

VPADStatus* GetVPADStatus(VPADReadError* error) {
    *error = vpadError;
    return hasVpad ? &vpadStatus : nullptr;
}

KPADStatus* GetKPADStatus(WPADChan chan, KPADError* error) {
    *error = kpadError[chan];
    return hasKpad[chan] ? &kpadStatus[chan] : nullptr;
}

namespace Watchdog {

volatile uint32_t gSeq = 0;
volatile uint32_t gPhase = PH_NONE;
volatile uint32_t gSteps = 0;
volatile uint32_t gOpcode = 0;
volatile uint32_t gCmd = 0;
volatile uint32_t gFrame = 0;
char gDetail[128] = { 0 };
char gTexturePath[128] = { 0 };
volatile uint32_t gTraceState = TRACE_OFF;
volatile uint32_t gTraceFramesRemaining = 0;
volatile uint32_t gTraceStepState = TRACE_STEPS_WAITING;
volatile uint32_t gTraceStepsRemaining = 0;
volatile uint32_t gAudioSeq = 0;
volatile uint32_t gAudioPhase = 0;
volatile uint32_t gTexBytes = 0;
volatile uint32_t gTexCount = 0;
volatile uint32_t gLastTexPtr = 0;
volatile uint32_t gFlipCount = 0;
volatile uint32_t gOtrCacheHits = 0;
volatile uint32_t gOtrCacheMisses = 0;
volatile uint32_t gOtrResourceManagerLookups = 0;
volatile uint32_t gDrawBufferHighWaterBytes = 0;
volatile uint32_t gGx2SlotWaits = 0;
volatile uint32_t gFrameTimingCount = 0;
volatile uint32_t gFrameTimingGpuUs = 0;
volatile uint32_t gFrameTimingCpuUs = 0;
volatile uint32_t gFrameTimingWaitUs = 0;
// Keep the periodic watchdog and explicitly requested diagnostics, but do not stream every
// watchdog Enter/Leave pair from the GX2 hot path.
volatile uint32_t gEventStream = 0;
volatile uint32_t gEmitOk = 0;
volatile uint32_t gEmitFail = 0;

// The profiler is deliberately a small, allocation-free open-addressed table. The watchdog
// samples the render thread while it is suspended, then does all table work only after the
// thread has been resumed.
static const uint32_t kProfileTableSize = 1024;
static const uint32_t kProfileTopPcCount = 24;
static const uint32_t kProfileTopLrCount = 12;
static const uint32_t kProfileEntriesPerLine = 6;
static const uint32_t kProfileSampleIntervalMs = 10;
static const uint32_t kProfileSamplesPerReport = 1000;
static const uint32_t kProfileSamplesPerWatchdogTick = WDOG_TICK_INTERVAL_MS / kProfileSampleIntervalMs;

struct ProfileBucket {
    uint32_t address;
    uint32_t count;
};

struct ProfileTop {
    uint32_t address;
    uint32_t count;
};

static ProfileBucket sPcProfile[kProfileTableSize];
static ProfileBucket sLrProfile[kProfileTableSize];
static ProfileTop sTopPcs[kProfileTopPcCount];
static ProfileTop sTopLrs[kProfileTopLrCount];
static uint32_t sProfileSampleCount = 0;

// UNICAST, not broadcast. Broadcast is what died at 322 datagrams/second on 2026-09-13 and
// took every diagnostic line with it, which then read as a PowerPC-wide wedge for two
// sessions. This is the workstation running wiiu/udplog.py; udplog.py binds 0.0.0.0 so it
// receives unicast without any change. If the listener moves, this constant moves with it.
static const uint32_t kLogHostAddr = 0x0A0104ABu; // 10.1.4.171

void RecordGX2Wait(uint32_t microseconds) {
    __sync_fetch_and_add(&gFrameTimingWaitUs, microseconds);
}

void RecordGX2SlotWait(uint32_t microseconds) {
    __sync_fetch_and_add(&gGx2SlotWaits, 1);
    RecordGX2Wait(microseconds);
}

void RecordFrameTiming(uint32_t gpuMicroseconds, uint32_t cpuMicroseconds) {
    __sync_fetch_and_add(&gFrameTimingGpuUs, gpuMicroseconds);
    __sync_fetch_and_add(&gFrameTimingCpuUs, cpuMicroseconds);
    __sync_synchronize();
    __sync_fetch_and_add(&gFrameTimingCount, 1);
}

static OSThread sThread;
static OSThread* sSampleThread = nullptr;
// 32 KB, statically reserved. The watchdog must survive a stall inside the allocator,
// so nothing here may touch the heap after Start() returns.
static uint8_t sStack[32 * 1024] __attribute__((aligned(16)));
static int sSocket = -1;
static bool sStarted = false;
static bool sRescueFired = false;

static const char* PhaseName(uint32_t phase) {
    switch (phase & ~PH_DONE) {
        case PH_NONE:
            return "none";
        case PH_DL_STEP:
            return "dl-step";
        case PH_LOAD_RES:
            return "load-resource";
        case PH_ARCHIVE:
            return "archive-read";
        case PH_TEX_ALLOC:
            return "gx2-tex-alloc";
        case PH_TEX_UPLOAD:
            return "gx2-tex-upload";
        case PH_FLUSH:
            return "flush";
        case PH_ENDFRAME:
            return "end-frame";
        case PH_GX2_DRAW_DONE:
            return "gx2-draw-done";
        case PH_GX2_SLOT_WAIT:
            return "gx2-slot-wait";
        case PH_GX2_SET_PIXEL_TEXTURE:
            return "gx2-set-pixel-texture";
        case PH_GX2_DRAW_TRIANGLES:
            return "gx2-draw-triangles";
        case PH_GX2_GENERATE_SHADER:
            return "gx2-generate-shader";
        case PH_GX2_SET_FETCH_SHADER:
            return "gx2-set-fetch-shader";
        case PH_GX2_SET_VERTEX_SHADER:
            return "gx2-set-vertex-shader";
        case PH_GX2_SET_PIXEL_SHADER:
            return "gx2-set-pixel-shader";
        case PH_TEX_UPLOAD_FN:
            return "gx2-upload-texture-fn";
        case PH_GX2_NEW_TEXTURE:
            return "gx2-new-texture";
        case PH_GX2_SELECT_TEXTURE:
            return "gx2-select-texture";
        case PH_GX2_SET_SAMPLER:
            return "gx2-set-sampler";
        case PH_GUI_TEXTURE:
            return "gui-texture-load";
        case PH_IMGUI_FONT_TEX:
            return "imgui-fonts-texture";
        case PH_IMGUI_DEVICE_OBJECTS:
            return "imgui-device-objects";
        case PH_IMGUI_NEW_FRAME:
            return "imgui-new-frame";
        case PH_GAME_INIT:
            return "game-init";
        default:
            return "?";
    }
}

// snprintf into a fixed buffer, then one sendto. No std::string, no fmt, no spdlog,
// no allocation - see the header for why that matters.
void Emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Emit(const char* fmt, ...) {
    // MUST stay a stack local. With gEventStream on, the main thread (Enter/Leave) and the
    // watchdog thread (tick lines) call this concurrently; a shared static buffer would
    // interleave them and the channel would lie - the third time instrumentation would have
    // lied in this hunt. 512 bytes fits the watchdog's 32 KB stack, and stack use keeps
    // Emit allocation-free, which is the one rule this file exists to obey. 384 rather than
    // 512 because Emit is also called from the exception-registration threads and from the
    // exception callbacks, whose stacks are 4 KB, and losing an EXC: line to a blown stack
    // would cost the evidence most worth having. Not smaller than 384: the worst-case
    // `alive` line is 281 bytes (a 127-byte detail plus every numeric field at full width),
    // and truncating it would drop `flips=` off the end - the field the tick exists to
    // report. vsnprintf truncates safely either way, but silently.
    char line[384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || sSocket < 0) {
        return;
    }
    if (n > (int)sizeof(line) - 1) {
        n = (int)sizeof(line) - 1;
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(4405);
    to.sin_addr.s_addr = htonl(kLogHostAddr);
    // The return value was ignored, so a channel that had stopped delivering was
    // indistinguishable from a console that had stopped running.
    if (sendto(sSocket, line, n, 0, (struct sockaddr*)&to, sizeof(to)) < 0) {
        ++gEmitFail;
    } else {
        ++gEmitOk;
    }
}

static uint32_t ProfileHash(uint32_t address) {
    return ((address >> 4) ^ (address >> 12) ^ (address >> 20)) & (kProfileTableSize - 1);
}

static void ProfileAdd(ProfileBucket* table, uint32_t address) {
    if (address == 0) {
        return;
    }

    const uint32_t start = ProfileHash(address);
    for (uint32_t probe = 0; probe < kProfileTableSize; ++probe) {
        ProfileBucket* bucket = &table[(start + probe) & (kProfileTableSize - 1)];
        if (bucket->address == address) {
            ++bucket->count;
            return;
        }
        if (bucket->address == 0) {
            bucket->address = address;
            bucket->count = 1;
            return;
        }
    }
}

static void ProfileSelectTop(const ProfileBucket* table, ProfileTop* top, uint32_t topCount) {
    memset(top, 0, sizeof(ProfileTop) * topCount);

    for (uint32_t bucketIndex = 0; bucketIndex < kProfileTableSize; ++bucketIndex) {
        const ProfileBucket& bucket = table[bucketIndex];
        if (bucket.address == 0 || bucket.count == 0) {
            continue;
        }

        uint32_t insertAt = topCount;
        for (uint32_t topIndex = 0; topIndex < topCount; ++topIndex) {
            if (bucket.count > top[topIndex].count) {
                insertAt = topIndex;
                break;
            }
        }
        if (insertAt == topCount) {
            continue;
        }

        for (uint32_t topIndex = topCount - 1; topIndex > insertAt; --topIndex) {
            top[topIndex] = top[topIndex - 1];
        }
        top[insertAt].address = bucket.address;
        top[insertAt].count = bucket.count;
    }
}

static void EmitProfileTop(const char* kind, const ProfileTop* top, uint32_t topCount) {
    uint32_t first = 0;
    while (first < topCount && top[first].count != 0) {
        char line[384];
        int length = snprintf(line, sizeof(line), "PROF: %s=", kind);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return;
        }

        uint32_t emitted = 0;
        while (first < topCount && emitted < kProfileEntriesPerLine && top[first].count != 0) {
            const int written = snprintf(line + length, sizeof(line) - (size_t)length, "%s0x%08X:%u",
                                         emitted == 0 ? "" : " ", top[first].address, top[first].count);
            if (written < 0 || (size_t)written >= sizeof(line) - (size_t)length) {
                return;
            }
            length += written;
            ++first;
            ++emitted;
        }
        Emit("%s\n", line);
    }
}

static void EmitProfileReport() {
    ProfileSelectTop(sPcProfile, sTopPcs, kProfileTopPcCount);
    ProfileSelectTop(sLrProfile, sTopLrs, kProfileTopLrCount);
    EmitProfileTop("pc", sTopPcs, kProfileTopPcCount);
    EmitProfileTop("lr", sTopLrs, kProfileTopLrCount);
    Emit("PROF: samples=%u\n", sProfileSampleCount);

    memset(sPcProfile, 0, sizeof(sPcProfile));
    memset(sLrProfile, 0, sizeof(sLrProfile));
    sProfileSampleCount = 0;
}

static void ProfileSample() {
    if (sSampleThread == nullptr) {
        return;
    }

    OSSuspendThread(sSampleThread);
    const uint32_t pc = sSampleThread->context.srr0;
    const uint32_t lr = sSampleThread->context.lr;
    OSResumeThread(sSampleThread);

    ProfileAdd(sPcProfile, pc);
    ProfileAdd(sLrProfile, lr & ~0xFu);
    ++sProfileSampleCount;
    if (sProfileSampleCount >= kProfileSamplesPerReport) {
        EmitProfileReport();
    }
}

void TraceEvent(uint32_t phase, const char* event) {
    if (gTraceState != TRACE_ACTIVE && gEventStream == 0) {
        return;
    }
    // Resource loading is the flood: two events per asset, hundreds of assets, and it is what
    // pushed a capture to 397 lines/second - past anything this channel has been measured to
    // survive. These phases still update the breadcrumb, so the 500 ms tick reports which asset
    // the game is on; they just do not each get their own datagram. Everything that localises a
    // hang - game-init steps, GX2, ImGui - keeps streaming.
    if (gTraceState != TRACE_ACTIVE && (phase == PH_LOAD_RES || phase == PH_ARCHIVE)) {
        return;
    }
    if (gTraceState == TRACE_ACTIVE && phase == PH_TEX_UPLOAD && strcmp(event, "enter") == 0 &&
        gTraceStepState == TRACE_STEPS_WAITING && strstr(gDetail, "font_letter") != nullptr) {
        gTraceStepState = TRACE_STEPS_ACTIVE;
        gTraceStepsRemaining = WDOG_TRACE_STEP_COUNT;
    }
    // gSeq on every line: the receiver can now tell "the game stopped" from "the datagrams
    // stopped arriving" by whether the next line it sees skipped sequence numbers.
    Emit("WDOG: trace seq=%u frame=%u phase=%s event=%s detail=%s\n", gSeq, gFrame, PhaseName(phase), event,
         gDetail);
}

void TraceStep(uint32_t opcode, const void* cmd, uint32_t steps) {
    if (gTraceState != TRACE_ACTIVE || gTraceStepState != TRACE_STEPS_ACTIVE || gTraceStepsRemaining == 0) {
        return;
    }
    Emit("WDOG: trace frame=%u phase=dl-step event=step steps=%u opcode=0x%02X cmd=0x%08X\n", gFrame, steps,
         opcode, (uint32_t)(uintptr_t)cmd);
    --gTraceStepsRemaining;
    if (gTraceStepsRemaining == 0) {
        gTraceStepState = TRACE_STEPS_DONE;
    }
}

void ArmTraceAfterTrackLoad() {
    gTraceFramesRemaining = 0;
    gTraceStepsRemaining = 0;
    gTraceStepState = TRACE_STEPS_WAITING;
    gTraceState = TRACE_ARMED;
}

static int Main(int, const char**) {
    Emit("WDOG: online core=%d - if you never see this line the socket is the problem, not the game\n",
         (int)OSGetCoreId());

    uint32_t lastSeq = 0xFFFFFFFFu;
    uint32_t stalledTicks = 0;
    uint32_t tick = 0;
    uint32_t samplesSinceWatchdogTick = 0;

    for (;;) {
        ProfileSample();
        ++samplesSinceWatchdogTick;
        if (samplesSinceWatchdogTick < kProfileSamplesPerWatchdogTick) {
            OSSleepTicks(OSMillisecondsToTicks(kProfileSampleIntervalMs));
            continue;
        }
        samplesSinceWatchdogTick = 0;

        const uint32_t seq = gSeq;
        const uint32_t phase = gPhase;
        const uint32_t frame = gFrame;
        const uint32_t steps = gSteps;
        const uint32_t opcode = gOpcode;
        const uint32_t cmd = gCmd;

        if (seq == lastSeq) {
            ++stalledTicks;
            // Self-rescue attempt, once per run. Two things are being measured at once, and
            // both are worth a line of code:
            //   1. If this line ARRIVES, the watchdog thread is still running while the game
            //      is stuck - which retires "the wedge is PowerPC-wide" as a claim, since that
            //      only ever rested on this channel going quiet.
            //   2. If the console then actually returns to the menu, the loop becomes
            //      autonomous: ftpiiu and wiiload come back by themselves and no hand is
            //      needed. It may well not work - SYSLaunchMenu needs the MAIN thread to
            //      process the ProcUI foreground release, and that is the thread that is
            //      stuck - but it costs one call to find out.
            // Nothing here allocates before the Emit: a held heap lock must not be what
            // stops the report.
            if (!sRescueFired && (stalledTicks * WDOG_TICK_INTERVAL_MS) >= WDOG_STALL_RESCUE_MS) {
                sRescueFired = true;
                Emit("WDOG: stalled %ums - watchdog thread IS alive; attempting SYSLaunchMenu\n",
                     stalledTicks * WDOG_TICK_INTERVAL_MS);
                SYSLaunchMenu();
            }
            // The main thread has not moved. Whatever `phase` says is where it stopped;
            // a DONE flag means it stopped just after that call returned, not inside it.
            Emit("WDOG: STALLED %ums phase=%s%s frame=%u steps=%u opcode=0x%02X cmd=0x%08X detail=%s flips=%u\n",
                 stalledTicks * WDOG_TICK_INTERVAL_MS, PhaseName(phase), (phase & PH_DONE) ? "(returned)" : "(in progress)",
                 frame, steps, opcode, cmd, gDetail, gFlipCount);
        } else {
            if (stalledTicks != 0) {
                Emit("WDOG: recovered after %ums\n", stalledTicks * WDOG_TICK_INTERVAL_MS);
            }
            stalledTicks = 0;
            lastSeq = seq;
            Emit("WDOG: alive seq=%u phase=%s%s frame=%u steps=%u opcode=0x%02X cmd=0x%08X detail=%s flips=%u\n",
                 seq, PhaseName(phase), (phase & PH_DONE) ? "(returned)" : "(in progress)", frame, steps, opcode,
                 cmd, gDetail, gFlipCount);
        }

        // MEM2 pressure, every 200th tick (~100 s) and on its own line. Deliberately NOT folded
        // into the alive/STALLED lines: Emit's buffer is 512 bytes and a long detail= path
        // would push these fields off the end of exactly the line we most need intact.
        if ((tick++ % 20u) == 0u) {
            const MEMHeapHandle mem2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
            // Cross-check: heapFree is a base-heap query whose heap type is assumed, so it is
            // unverified on first use. texBytes/texCount/lastPtr are our own counters and
            // cannot silently lie. If the two disagree, believe the counters.
            // mem2HeapFree read 0 on every beat of round 3, so the "MEM2 base heap is an
            // ExpHeap" assumption is WRONG and the field is dropped rather than left lying.
            // texCount/texBytes/lastTexPtr are our own counters and are kept.
            (void)mem2;
            const uint32_t timingFrames = __sync_lock_test_and_set(&gFrameTimingCount, 0);
            const uint32_t timingGpuUs = __sync_lock_test_and_set(&gFrameTimingGpuUs, 0);
            const uint32_t timingCpuUs = __sync_lock_test_and_set(&gFrameTimingCpuUs, 0);
            const uint32_t timingWaitUs = __sync_lock_test_and_set(&gFrameTimingWaitUs, 0);
            const uint32_t slotWaits = __sync_lock_test_and_set(&gGx2SlotWaits, 0);
            const uint32_t gpuAvgUs = timingFrames ? timingGpuUs / timingFrames : 0;
            const uint32_t cpuAvgUs = timingFrames ? timingCpuUs / timingFrames : 0;
            const uint32_t waitAvgUs = timingFrames ? timingWaitUs / timingFrames : 0;
            // newlib heap (what malloc/new actually use): arena = bytes obtained via sbrk,
            // used = bytes in allocated chunks. Growth of "used" is the leak/pressure signal.
            const struct mallinfo heapInfo = mallinfo();
            Emit("WDOG: mem texCount=%u texBytes=%u lastTexPtr=0x%08X audioSeq=%u audioPhase=%u flips=%u "
                 "emitOk=%u emitFail=%u otrHit=%u otrMiss=%u rmLookup=%u drawHwm=%u frames=%u gpuUs=%u cpuUs=%u "
                 "waitUs=%u slotWaits=%u heapArena=%u heapUsed=%u\n",
                 gTexCount, gTexBytes, gLastTexPtr, gAudioSeq, gAudioPhase, gFlipCount, gEmitOk, gEmitFail,
                 gOtrCacheHits, gOtrCacheMisses, gOtrResourceManagerLookups, gDrawBufferHighWaterBytes, timingFrames,
                 gpuAvgUs, cpuAvgUs, waitAvgUs, slotWaits, (uint32_t)heapInfo.arena,
                 (uint32_t)heapInfo.uordblks);
        }

        OSSleepTicks(OSMillisecondsToTicks(kProfileSampleIntervalMs));
    }
    return 0;
}

static void TerminateHandler() {
    Emit("CXX: terminate\n");
    // Do NOT fall through to abort(). abort() hard-wedges the PowerPC side and takes
    // ftpiiu and the wiiload server with it, so every uncaught exception has been
    // costing a physical power cycle - the console still answers ping, but nothing
    // that needs CPU does, and it cannot be recovered remotely.
    //
    // _Exit() instead: no destructors (we are already in a terminate handler, the heap
    // and locks cannot be trusted), but the title ends the way the system expects and
    // the Aroma plugins survive. A crash then costs a wiiload, not a reboot.
    _Exit(1);
}

void Start() {
    std::set_terminate(TerminateHandler);

    if (sStarted) {
        return;
    }
    sStarted = true;
    sSampleThread = OSGetCurrentThread();

    // wut wants the socket library brought up before any BSD call. WHBLogUdpInit() has
    // already run by this point and does this for its own socket, but whether that init is
    // process-global is not something to assume - calling it again is harmless.
    socket_lib_init();

    sSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (sSocket >= 0) {
        int on = 1;
        setsockopt(sSocket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }

    // Prove the channel works NOW, while everything is healthy, and say so on the ordinary
    // log. Without this, a socket that was never created and a console that is wedged
    // produce identical evidence: no WDOG lines at all. One test datagram from this thread
    // settles which, before it can possibly matter.
    int testRc = -1;
    if (sSocket >= 0) {
        static const char probe[] = "WDOG: channel test from Start()\n";
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(4405);
        to.sin_addr.s_addr = INADDR_BROADCAST;
        testRc = (int)sendto(sSocket, probe, sizeof(probe) - 1, 0, (struct sockaddr*)&to, sizeof(to));
    }
    // On the RAW channel, not spdlog. The log level is now `warn` on Wii U (the trace/info
    // flood was ~290 UDP datagrams a second and is itself a suspect), so an SPDLOG_INFO here
    // would be swallowed -- and this is the one line MK_FREEZE_2026-09-09.md makes the
    // precondition for reading a capture at all.
    Emit("watchdog: socket=%d channel-test-sendto=%d dest=%u.%u.%u.%u:4405 (if socket is -1 the "
         "watchdog cannot report and its silence means nothing)\n",
         sSocket, testRc, (unsigned int)((kLogHostAddr >> 24) & 0xFF), (unsigned int)((kLogHostAddr >> 16) & 0xFF),
         (unsigned int)((kLogHostAddr >> 8) & 0xFF), (unsigned int)(kLogHostAddr & 0xFF));

    // Core 2. The game loop runs on core 1, so a stalled main thread cannot hold this off.
    // Priority 5 is above the main thread's 16 (lower number wins on Cafe OS).
    if (!OSCreateThread(&sThread, Main, 0, nullptr, sStack + sizeof(sStack), sizeof(sStack), 5,
                        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        return;
    }
    OSSetThreadName(&sThread, "MK freeze watchdog");
    OSResumeThread(&sThread);
}

} // namespace Watchdog

}; // namespace WiiU
}; // namespace Ship

extern "C" void modelRenderWatchdogReportInvalidDisplayList(uint32_t index, uint32_t size, const char* handler,
                                                            const void* node) {
    static uint32_t emitted = 0;
    static uint64_t lastEmitTick = 0;
    static const uint64_t kTicksPerSecond = 62156250ULL;
    const uint64_t now = OSGetSystemTick();
    // The first 20 go out unthrottled: a 1/sec cap samples roughly one report per
    // second out of hundreds per frame, which hid the real spread of bad indices.
    if (emitted >= 20 && lastEmitTick != 0 && now - lastEmitTick < kTicksPerSecond) {
        return;
    }
    ++emitted;
    lastEmitTick = now;

    // Dump the node as it actually sits in memory. If cmd0 reads 5 (big-endian,
    // i.e. already converted) while the s16 array at +8 reads byte-reversed, then
    // the header and the payload of one node disagree -- which the load-time swap
    // cannot produce, and would mean the renderer is reading a different buffer.
    const uint32_t* words = static_cast<const uint32_t*>(node);
    const int16_t* arr = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(node) + 8);
    Ship::WiiU::Watchdog::Emit(
        "MODEL: skipped invalid display-list index=%u size=%u handler=%s node=%p cmd0=0x%08X size4=0x%08X "
        "arr=[%d,%d,%d]\n",
        index, size, handler, node, words[0], words[1], (int)arr[0], (int)arr[1], (int)arr[2]);
}

#endif
