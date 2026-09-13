#ifdef __WIIU__
#include "port/wiiu/WiiUImpl.h"

#include <exception>
#include <stdio.h>
#include <stdarg.h>
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
#include <nsysnet/_socket.h>

extern "C" {
void __real___cxa_throw(void* exception, std::type_info* typeInfo, void (*destructor)(void*))
    __attribute__((noreturn));

void __wrap___cxa_throw(void* exception, std::type_info* typeInfo, void (*destructor)(void*)) {
    Ship::WiiU::Watchdog::Emit("CXX: throw type=%s\n", typeInfo->name());
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

    Watchdog::Start();
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
// On by default in this diagnosis build: see the header for why the 500 ms tick is the
// blind spot rather than the breadcrumbs.
volatile uint32_t gEventStream = 1;

static OSThread sThread;
// 32 KB, statically reserved. The watchdog must survive a stall inside the allocator,
// so nothing here may touch the heap after Start() returns.
static uint8_t sStack[32 * 1024] __attribute__((aligned(16)));
static int sSocket = -1;
static bool sStarted = false;

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
    // Emit allocation-free, which is the one rule this file exists to obey.
    char line[512];
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
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(sSocket, line, n, 0, (struct sockaddr*)&to, sizeof(to));
}

void TraceEvent(uint32_t phase, const char* event) {
    if (gTraceState != TRACE_ACTIVE && gEventStream == 0) {
        return;
    }
    if (gTraceState == TRACE_ACTIVE && phase == PH_TEX_UPLOAD && strcmp(event, "enter") == 0 &&
        gTraceStepState == TRACE_STEPS_WAITING && strstr(gDetail, "font_letter") != nullptr) {
        gTraceStepState = TRACE_STEPS_ACTIVE;
        gTraceStepsRemaining = WDOG_TRACE_STEP_COUNT;
    }
    Emit("WDOG: trace frame=%u phase=%s event=%s detail=%s\n", gFrame, PhaseName(phase), event, gDetail);
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

static BOOL EmitException(OSExceptionType type, const char* typeName, OSContext* context) {
    Emit("EXC: core=%u\n", (unsigned int)OSGetCoreId());
    Emit("EXC: type=%s (%u)\n", typeName, (unsigned int)type);
    Emit("EXC: srr0=0x%08X\n", context->srr0);
    Emit("EXC: srr1=0x%08X\n", context->srr1);
    Emit("EXC: dar=0x%08X\n", context->dar);
    Emit("EXC: dsisr=0x%08X\n", context->dsisr);
    Emit("EXC: lr=0x%08X\n", context->lr);
    // FALSE = "not handled": the kernel then takes exactly the path it took in all six
    // previous freezes, so this build only ADDS information and does not change the
    // failure. Returning TRUE would resume at srr0, re-run the faulting instruction and
    // fault again forever, flooding the channel and altering what we are measuring.
    return FALSE;
}

static BOOL DsiExceptionCallback(OSContext* context) {
    return EmitException(OS_EXCEPTION_TYPE_DSI, "DSI", context);
}

static BOOL IsiExceptionCallback(OSContext* context) {
    return EmitException(OS_EXCEPTION_TYPE_ISI, "ISI", context);
}

static BOOL ProgramExceptionCallback(OSContext* context) {
    return EmitException(OS_EXCEPTION_TYPE_PROGRAM, "PROGRAM", context);
}

static BOOL MachineCheckExceptionCallback(OSContext* context) {
    return EmitException(OS_EXCEPTION_TYPE_MACHINE_CHECK, "MACHINE_CHECK", context);
}

static BOOL AlignmentExceptionCallback(OSContext* context) {
    return EmitException(OS_EXCEPTION_TYPE_ALIGNMENT, "ALIGNMENT", context);
}

static BOOL FloatingPointExceptionCallback(OSContext* context) {
    return EmitException(OS_EXCEPTION_TYPE_FLOATING_POINT, "FLOATING_POINT", context);
}

static int RegisterExceptionCallbacks(int, const char**) {
    OSSetExceptionCallback(OS_EXCEPTION_TYPE_MACHINE_CHECK, MachineCheckExceptionCallback);
    OSSetExceptionCallback(OS_EXCEPTION_TYPE_DSI, DsiExceptionCallback);
    OSSetExceptionCallback(OS_EXCEPTION_TYPE_ISI, IsiExceptionCallback);
    OSSetExceptionCallback(OS_EXCEPTION_TYPE_ALIGNMENT, AlignmentExceptionCallback);
    OSSetExceptionCallback(OS_EXCEPTION_TYPE_PROGRAM, ProgramExceptionCallback);
    OSSetExceptionCallback(OS_EXCEPTION_TYPE_FLOATING_POINT, FloatingPointExceptionCallback);
    Emit("EXC: registered core=%u\n", (unsigned int)OSGetCoreId());
    return 0;
}

static void RegisterExceptionCallbacksOnAllCores() {
    static OSThread threads[3];
    static uint8_t stacks[3][4 * 1024] __attribute__((aligned(16)));
    static const OSThreadAttributes attributes[3] = {
        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU0,
        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU1,
        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU2,
    };
    bool started[3] = { false, false, false };

    for (int core = 0; core < 3; ++core) {
        if (!OSCreateThread(&threads[core], RegisterExceptionCallbacks, 0, nullptr,
                            stacks[core] + sizeof(stacks[core]), sizeof(stacks[core]), 5, attributes[core])) {
            Emit("EXC: registration thread failed core=%d\n", core);
            continue;
        }
        started[core] = true;
        OSResumeThread(&threads[core]);
    }

    for (int core = 0; core < 3; ++core) {
        if (started[core]) {
            OSJoinThread(&threads[core], nullptr);
        }
    }
}

static int Main(int, const char**) {
    Emit("WDOG: online core=%d - if you never see this line the socket is the problem, not the game\n",
         (int)OSGetCoreId());

    uint32_t lastSeq = 0xFFFFFFFFu;
    uint32_t stalledTicks = 0;
    uint32_t tick = 0;

    for (;;) {
        const uint32_t seq = gSeq;
        const uint32_t phase = gPhase;
        const uint32_t frame = gFrame;
        const uint32_t steps = gSteps;
        const uint32_t opcode = gOpcode;
        const uint32_t cmd = gCmd;

        if (seq == lastSeq) {
            ++stalledTicks;
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
        if ((tick++ % 200u) == 0u) {
            const MEMHeapHandle mem2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
            // Cross-check: heapFree is a base-heap query whose heap type is assumed, so it is
            // unverified on first use. texBytes/texCount/lastPtr are our own counters and
            // cannot silently lie. If the two disagree, believe the counters.
            // mem2HeapFree read 0 on every beat of round 3, so the "MEM2 base heap is an
            // ExpHeap" assumption is WRONG and the field is dropped rather than left lying.
            // texCount/texBytes/lastTexPtr are our own counters and are kept.
            (void)mem2;
            Emit("WDOG: mem texCount=%u texBytes=%u lastTexPtr=0x%08X audioSeq=%u audioPhase=%u flips=%u\n",
                 gTexCount, gTexBytes, gLastTexPtr, gAudioSeq, gAudioPhase, gFlipCount);
        }

        OSSleepTicks(OSMillisecondsToTicks(WDOG_TICK_INTERVAL_MS));
    }
    return 0;
}

static void TerminateHandler() {
    Emit("CXX: terminate\n");
}

void Start() {
    std::set_terminate(TerminateHandler);

    if (sStarted) {
        return;
    }
    sStarted = true;

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
    Emit("watchdog: socket=%d channel-test-sendto=%d (if socket is -1 the watchdog cannot "
         "report and its silence means nothing)\n",
         sSocket, testRc);

    RegisterExceptionCallbacksOnAllCores();

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

#endif
