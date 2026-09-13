#pragma once

// Freeze diagnosis for the Mario Kart 64 race-start hang.
//
// The main thread stops inside one Interpreter::Run() and the whole log goes quiet.
// spdlog cannot tell us where, because whatever the main thread is holding it is also
// holding the sink mutex, so the logger dies with it. This is a second, independent
// channel: the main thread leaves a breadcrumb in plain globals, and a watchdog thread
// pinned to another core transmits it over its own raw UDP socket.
//
// Rules this file exists to obey:
//   * the watchdog never allocates - a stall inside the heap lock must not take it down
//   * the watchdog never touches spdlog
//   * leaving a breadcrumb must be cheap enough for the display-list inner loop

#ifdef __WIIU__

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

namespace Ship::WiiU::Watchdog {

enum Phase : uint32_t {
    PH_NONE = 0,
    PH_DL_STEP,      // walking the display list
    PH_LOAD_RES,     // ResourceManager::LoadResourceProcess
    PH_ARCHIVE,      // the actual archive/SD read underneath it
    PH_TEX_ALLOC,    // GX2 texture allocation out of the MEM1 heap
    PH_TEX_UPLOAD,   // GX2 texture upload / invalidate
    PH_FLUSH,        // Interpreter Flush()
    PH_ENDFRAME,     // Interpreter EndFrame()
    PH_GX2_DRAW_DONE, // GX2DrawDone() serialisation
    PH_GX2_SET_PIXEL_TEXTURE, // GX2SetPixelTexture()
    PH_GX2_DRAW_TRIANGLES,    // backend triangle draw submission
    PH_GX2_GENERATE_SHADER,   // gx2GenerateShaderGroup()
    PH_GX2_SET_FETCH_SHADER,  // GX2SetFetchShader()
    PH_GX2_SET_VERTEX_SHADER, // GX2SetVertexShader()
    PH_GX2_SET_PIXEL_SHADER,  // GX2SetPixelShader()
    // Added for the Banjo "wedge after gx2-tex-upload returned" hunt. The old vocabulary
    // could not tell the inner GX2Invalidate leave apart from the upload function's own
    // scope leave - both printed "gx2-tex-upload(returned)" with the same detail - and the
    // texture *setup* calls around an upload were not instrumented at all.
    PH_TEX_UPLOAD_FN,         // gfx_gx2_upload_texture() as a whole
    PH_GX2_NEW_TEXTURE,       // gfx_gx2_new_texture()
    PH_GX2_SELECT_TEXTURE,    // gfx_gx2_select_texture()
    PH_GX2_SET_SAMPLER,       // gfx_gx2_set_sampler_parameters()
    PH_GUI_TEXTURE,           // Fast3dGui GUI-texture load (named)
    PH_IMGUI_FONT_TEX,        // ImGui_ImplGX2_CreateFontsTexture()
    PH_IMGUI_DEVICE_OBJECTS,  // ImGui_ImplGX2_CreateDeviceObjects()
    PH_IMGUI_NEW_FRAME,       // ImGui_ImplGX2_NewFrame()
    PH_GAME_INIT,             // game-side startup step (detail = the step name)
    PH_COUNT
};

// Set on a phase that has RETURNED. A frozen breadcrumb reading LOAD_RES means we are
// stuck inside the load; LOAD_RES|DONE means the load returned and we are stuck after it.
constexpr uint32_t PH_DONE = 0x8000u;
constexpr uint32_t WDOG_TICK_INTERVAL_MS = 500u;
constexpr uint32_t WDOG_TRACE_FRAME_COUNT = 30u;
constexpr uint32_t WDOG_TRACE_STEP_COUNT = 3000u;

enum TraceState : uint32_t {
    TRACE_OFF = 0,
    TRACE_ARMED,
    TRACE_ACTIVE,
};

enum TraceStepState : uint32_t {
    TRACE_STEPS_WAITING = 0,
    TRACE_STEPS_ACTIVE,
    TRACE_STEPS_DONE,
};

extern volatile uint32_t gSeq;    // bumped on every breadcrumb; frozen == main thread stopped
extern volatile uint32_t gPhase;
extern volatile uint32_t gSteps;  // display-list steps this frame
extern volatile uint32_t gOpcode;
extern volatile uint32_t gCmd;    // command pointer, as an integer
extern volatile uint32_t gFrame;
extern char gDetail[128];
extern char gTexturePath[128];
// Diagnosis switch: when non-zero every Enter/Leave transmits its own line immediately,
// instead of only being visible if a 500 ms tick happens to sample it. The wedge being
// hunted kills the whole PowerPC within one tick of the last breadcrumb, so the tick is
// the blind spot - not the instrumentation. Deliberately independent of gTraceState so it
// does not arm the MK64 display-list step flood.
extern volatile uint32_t gEventStream;
// Channel health. Measured 2026-09-13: at 322 datagrams/second the UDP output stopped dead -
// spdlog's WHBLogUdp broadcast and this file's own broadcast socket together - while the game
// ran on to GameEngine::Create: exit (proved by the card log, which is not on this channel).
// Silence on this channel is therefore NOT evidence about the game. These counters, plus the
// sequence number now carried on every streamed line, make loss measurable instead of
// invisible.
extern volatile uint32_t gEmitOk;
extern volatile uint32_t gEmitFail;
extern volatile uint32_t gTraceState;
extern volatile uint32_t gTraceFramesRemaining;
extern volatile uint32_t gTraceStepState;
extern volatile uint32_t gTraceStepsRemaining;

// MEM2 pressure. Textures are allocated with plain memalign(), i.e. out of the default
// (MEM2) heap -- and the "MEM1" ExpHeap is itself carved out of a memalign block, which
// is why mem1Free sat byte-identical across two frozen captures while the texture
// pointers climbed half a gigabyte. These count the heap the textures really come from.
// Written only by the graphics thread, read only by the watchdog: no atomics needed.
// Audio worker liveness. EndAudioFrame already carries a bounded wait added after an
// earlier freeze ("this would previously have frozen"), so a cross-thread condition
// variable in this port is known to lose wakeups on Cafe OS. In the quiet build that guard
// started firing ~11 s into each session, 2-24 s before the wedge. These say whether the
// audio worker is still turning over when everything stops.
extern volatile uint32_t gAudioSeq;    // ++ per completed audio frame
extern volatile uint32_t gAudioPhase;  // 0 = waiting for work, 1 = mixing

extern volatile uint32_t gTexBytes;   // cumulative allocation bytes passed to memalign
extern volatile uint32_t gTexCount;
extern volatile uint32_t gLastTexPtr;
extern volatile uint32_t gFlipCount;

// Raw UDP diagnostic output shared with GX2 callbacks. This must remain allocation-free.
void Emit(const char* fmt, ...);
void TraceEvent(uint32_t phase, const char* event);
void TraceStep(uint32_t opcode, const void* cmd, uint32_t steps);
void ArmTraceAfterTrackLoad();

inline void TexAlloc(const void* ptr, uint32_t size) {
    gLastTexPtr = (uint32_t)(uintptr_t)ptr;
    gTexBytes += size;
    ++gTexCount;
}

inline void SetDetail(const char* s) {
    if (s == nullptr) {
        gDetail[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i < sizeof(gDetail) - 1 && s[i] != '\0'; ++i) {
        gDetail[i] = s[i];
    }
    gDetail[i] = '\0';
}

inline void SetTexturePath(const char* s) {
    if (s == nullptr) {
        gTexturePath[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i < sizeof(gTexturePath) - 1 && s[i] != '\0'; ++i) {
        gTexturePath[i] = s[i];
    }
    gTexturePath[i] = '\0';
}

inline void SetDetailV(const char* fmt, va_list ap) {
    if (fmt == nullptr) {
        gDetail[0] = '\0';
        return;
    }
    const int n = vsnprintf(gDetail, sizeof(gDetail), fmt, ap);
    if (n < 0) {
        gDetail[0] = '\0';
    }
}

inline void Enter(uint32_t phase, const char* detail) {
    SetDetail(detail);
    gPhase = phase;
    ++gSeq;
    TraceEvent(phase, "enter");
}

inline void EnterFmt(uint32_t phase, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
inline void EnterFmt(uint32_t phase, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SetDetailV(fmt, ap);
    va_end(ap);
    gPhase = phase;
    ++gSeq;
    TraceEvent(phase, "enter");
}

inline void Leave(uint32_t phase) {
    gPhase = phase | PH_DONE;
    ++gSeq;
    TraceEvent(phase, "leave");
}

// Display-list inner loop: three stores and a counter, no string work.
inline void Step(uint32_t opcode, const void* cmd, uint32_t steps) {
    gOpcode = opcode;
    gCmd = (uint32_t)(uintptr_t)cmd;
    gSteps = steps;
    gPhase = PH_DL_STEP;
    ++gSeq;
    if (gTraceState == TRACE_ACTIVE && gTraceStepState == TRACE_STEPS_ACTIVE) {
        TraceStep(opcode, cmd, steps);
    }
}

inline void Frame(uint32_t n) {
    gFrame = n;
    if (gTraceState == TRACE_ARMED) {
        gTraceState = TRACE_ACTIVE;
        gTraceFramesRemaining = WDOG_TRACE_FRAME_COUNT;
    } else if (gTraceState == TRACE_ACTIVE) {
        if (gTraceFramesRemaining > 0) {
            --gTraceFramesRemaining;
        }
        if (gTraceFramesRemaining == 0) {
            gTraceState = TRACE_OFF;
        }
    }
}

// Marks a phase for the duration of a scope. Needed because the functions being
// instrumented have several return paths each.
struct Scope {
    uint32_t mPhase;
    Scope(uint32_t phase, const char* detail) : mPhase(phase) {
        Enter(phase, detail);
    }
    ~Scope() {
        Leave(mPhase);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

struct ScopeFmt {
    uint32_t mPhase;
    ScopeFmt(uint32_t phase, const char* fmt, ...) : mPhase(phase) {
        va_list ap;
        va_start(ap, fmt);
        SetDetailV(fmt, ap);
        va_end(ap);
        gPhase = phase;
        ++gSeq;
        TraceEvent(phase, "enter");
    }
    ~ScopeFmt() {
        Leave(mPhase);
    }
    ScopeFmt(const ScopeFmt&) = delete;
    ScopeFmt& operator=(const ScopeFmt&) = delete;
};

// Starts the watchdog thread. Safe to call more than once; only the first call does work.
// Transmits one "online" line immediately, so that later silence is evidence about the
// game and not about the socket.
void Start();

} // namespace Ship::WiiU::Watchdog

#define WDOG_CAT_(a, b) a##b
#define WDOG_CAT(a, b) WDOG_CAT_(a, b)

#define WDOG_ENTER(phase, detail) ::Ship::WiiU::Watchdog::Enter(phase, detail)
#define WDOG_ENTER_FMT(phase, fmt, ...) ::Ship::WiiU::Watchdog::EnterFmt(phase, fmt, __VA_ARGS__)
#define WDOG_EMIT(...) ::Ship::WiiU::Watchdog::Emit(__VA_ARGS__)
#define WDOG_LEAVE(phase) ::Ship::WiiU::Watchdog::Leave(phase)
#define WDOG_STEP(op, cmd, steps) ::Ship::WiiU::Watchdog::Step(op, cmd, steps)
#define WDOG_FRAME(n) ::Ship::WiiU::Watchdog::Frame(n)
#define WDOG_TEXTURE_PATH(path) ::Ship::WiiU::Watchdog::SetTexturePath(path)
#define WDOG_TEXTURE_DETAIL() ::Ship::WiiU::Watchdog::gTexturePath
#define WDOG_SCOPE(phase, detail) ::Ship::WiiU::Watchdog::Scope WDOG_CAT(wdogScope_, __LINE__)(phase, detail)
#define WDOG_SCOPE_FMT(phase, fmt, ...) \
    ::Ship::WiiU::Watchdog::ScopeFmt WDOG_CAT(wdogScopeFmt_, __LINE__)(phase, fmt, __VA_ARGS__)
#define WDOG_TEXALLOC(ptr, size) ::Ship::WiiU::Watchdog::TexAlloc(ptr, size)

#else

#define WDOG_ENTER(phase, detail) ((void)0)
#define WDOG_ENTER_FMT(phase, fmt, ...) ((void)0)
#define WDOG_EMIT(...) ((void)0)
#define WDOG_LEAVE(phase) ((void)0)
#define WDOG_STEP(op, cmd, steps) ((void)0)
#define WDOG_FRAME(n) ((void)0)
#define WDOG_TEXTURE_PATH(path) ((void)0)
#define WDOG_TEXTURE_DETAIL() nullptr
#define WDOG_SCOPE(phase, detail) ((void)0)
#define WDOG_SCOPE_FMT(phase, fmt, ...) ((void)0)
#define WDOG_TEXALLOC(ptr, size) ((void)0)

#endif // __WIIU__
