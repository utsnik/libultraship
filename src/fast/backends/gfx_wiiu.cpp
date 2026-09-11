#ifdef ENABLE_GX2

#include <stdio.h>
#include <time.h>
#include <malloc.h>

#include <spdlog/spdlog.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <coreinit/foreground.h>
#include <coreinit/memory.h>
#include <coreinit/memheap.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>

#include <gx2/state.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/swap.h>
#include <gx2/mem.h>
#include <gx2r/mem.h>

#include <whb/proc.h>
#include <proc_ui/procui.h>
#include <proc_ui/memory.h>

#include <vpad/input.h>
#include <padscore/kpad.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "libultraship/libultra/gbi.h"

#include "fast/backends/gfx_rendering_api.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_gx2.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"

// gfx_gx2.cpp lives in namespace Fast; its shutdown entry point is used below.
namespace Fast {
void gfx_gx2_shutdown(void);
}
using Fast::gfx_gx2_shutdown;
#include "fast/backends/gfx_wiiu.h"
#include "port/wiiu/WiiUWatchdog.h"

#include "fast/backends/imgui_impl_wiiu.h"
#include "fast/backends/imgui_impl_wiiu.h" // ImGui_ImplWiiU_ControllerInput
// (was menu/ImGuiImpl.h: that header belonged to the pre-8.x LUS GUI and is gone)
// (was misc/Hooks.h) The pre-8.x LUS hook system was removed upstream; the
// ExitGame hook had no Wii U-specific work beyond the teardown below.

// Do not include port/wiiu/WiiUImpl.h here: its CafeOS input headers pull in
// coreinit/time.h, which conflicts with libultra's OSTime typedef already
// visible in this translation unit. These are the only Wii U helper symbols
// needed by the GX2 backend.
namespace Ship {
namespace WiiU {
void Exit();
void Update();
VPADStatus* GetVPADStatus(VPADReadError* error);
KPADStatus* GetKPADStatus(WPADChan chan, KPADError* error);
} // namespace WiiU
} // namespace Ship

static MEMHeapHandle heap_MEM1 = nullptr;
static MEMHeapHandle heap_foreground = nullptr;

bool has_foreground = false;
static void* mem1_storage = nullptr;
static void* command_buffer_pool = nullptr;
static void* secondary_command_buffer = nullptr;
static uint32_t display_list_overrun_count = 0;
static volatile bool secondary_command_buffer_in_use = false;
GX2ContextState* context_state = nullptr;

static GX2TVRenderMode tv_render_mode;
static void* tv_scan_buffer = nullptr;
static uint32_t tv_scan_buffer_size = 0;
static uint32_t tv_width;
static uint32_t tv_height;

static GX2DrcRenderMode drc_render_mode;
static void* drc_scan_buffer = nullptr;
static uint32_t drc_scan_buffer_size = 0;

static int frame_divisor = 1;

static void gfx_wiiu_flip_callback(GX2EventType, void*) {
    Ship::WiiU::Watchdog::gFlipCount = Ship::WiiU::Watchdog::gFlipCount + 1;
}

static void gfx_wiiu_display_list_overrun_callback(GX2EventType, void* event_data) {
    GX2DisplayListOverrunData* data = static_cast<GX2DisplayListOverrunData*>(event_data);
    const uint32_t count = ++display_list_overrun_count;

    if (!data) {
        Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun count=%u data=null\n", count);
        return;
    }

    Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun count=%u\n", count);
    Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun oldList=%p\n", data->oldList);
    Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun oldSize=0x%08X\n", data->oldSize);
    Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun newList=%p\n", data->newList);
    Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun newSize=0x%08X\n", data->newSize);

    if (!secondary_command_buffer_in_use && secondary_command_buffer) {
        secondary_command_buffer_in_use = true;
        data->newList = secondary_command_buffer;
        data->newSize = 0x400000;
        Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun satisfied newList=%p newSize=0x%08X\n",
                                   data->newList, data->newSize);
    } else {
        Ship::WiiU::Watchdog::Emit("GX2: display-list-overrun UNSATISFIED secondary-buffer-in-use=%u\n",
                                   secondary_command_buffer_in_use ? 1u : 0u);
    }
}

// for ImGui DeltaTime
// (initialized to 1 to not trigger imguis assert on initial draw)
uint32_t frametime = 1;

bool gfx_wiiu_init_mem1(void) {
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    uint32_t size;
    void* base;

    size = MEMGetAllocatableSizeForFrmHeapEx(heap, 4);
    if (!size) {
        printf("%s: MEMGetAllocatableSizeForFrmHeapEx == 0", __FUNCTION__);
        return false;
    }

    base = MEMAllocFromFrmHeapEx(heap, size, 4);
    if (!base) {
        printf("%s: MEMAllocFromFrmHeapEx(heap, 0x%X, 4) failed", __FUNCTION__, size);
        return false;
    }

    heap_MEM1 = MEMCreateExpHeapEx(base, size, 0);
    if (!heap_MEM1) {
        printf("%s: MEMCreateExpHeapEx(%p, 0x%X, 0) failed", __FUNCTION__, base, size);
        return false;
    }

    return true;
}

void gfx_wiiu_close(void) {
}

void gfx_wiiu_destroy_mem1(void) {
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);

    if (heap_MEM1) {
        MEMDestroyExpHeap(heap_MEM1);
        heap_MEM1 = NULL;
    }
}

bool gfx_wiiu_init_foreground(void) {
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);
    uint32_t size;
    void* base;

    size = MEMGetAllocatableSizeForFrmHeapEx(heap, 4);
    if (!size) {
        printf("%s: MEMAllocFromFrmHeapEx(heap, 0x%X, 4)", __FUNCTION__, size);
        return false;
    }

    base = MEMAllocFromFrmHeapEx(heap, size, 4);
    if (!base) {
        printf("%s: MEMGetAllocatableSizeForFrmHeapEx == 0", __FUNCTION__);
        return false;
    }

    heap_foreground = MEMCreateExpHeapEx(base, size, 0);
    if (!heap_foreground) {
        printf("%s: MEMCreateExpHeapEx(%p, 0x%X, 0)", __FUNCTION__, base, size);
        return false;
    }

    return true;
}

void gfx_wiiu_destroy_foreground(void) {
    MEMHeapHandle foreground = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);

    if (heap_foreground) {
        MEMDestroyExpHeap(heap_foreground);
        heap_foreground = NULL;
    }

    MEMFreeToFrmHeap(foreground, MEM_FRM_HEAP_FREE_ALL);
}

uint32_t gfx_wiiu_mem1_free(void) {
    if (!heap_MEM1) {
        return 0;
    }

    return MEMGetTotalFreeSizeForExpHeap(heap_MEM1);
}

uint32_t gfx_wiiu_mem1_largest(void) {
    if (!heap_MEM1) {
        return 0;
    }

    return MEMGetAllocatableSizeForExpHeapEx(heap_MEM1, 4);
}

void* gfx_wiiu_alloc_mem1(uint32_t size, uint32_t alignment) {
    void* block;

    if (!heap_MEM1) {
        return NULL;
    }

    if (alignment < 4) {
        alignment = 4;
    }

    block = MEMAllocFromExpHeapEx(heap_MEM1, size, alignment);
    return block;
}

void gfx_wiiu_free_mem1(void* block) {
    if (!heap_MEM1) {
        return;
    }

    MEMFreeToExpHeap(heap_MEM1, block);
}

void* gfx_wiiu_alloc_foreground(uint32_t size, uint32_t alignment) {
    void* block;

    if (!heap_foreground) {
        return NULL;
    }

    if (alignment < 4) {
        alignment = 4;
    }

    block = MEMAllocFromExpHeapEx(heap_foreground, size, alignment);
    return block;
}

void gfx_wiiu_free_foreground(void* block) {
    if (!heap_foreground) {
        return;
    }

    MEMFreeToExpHeap(heap_foreground, block);
}

static uint32_t gfx_wiiu_proc_callback_acquired(void* context) {
    has_foreground = true;

    bool result = gfx_wiiu_init_foreground();
    if (!result) {
        SPDLOG_ERROR("gfx_wiiu: failed to initialize foreground heap");
        return 0;
    }

    tv_scan_buffer = gfx_wiiu_alloc_foreground(tv_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
    if (!tv_scan_buffer) {
        SPDLOG_ERROR("gfx_wiiu: failed to allocate TV scan buffer");
        return 0;
    }

    SPDLOG_INFO("gfx_wiiu: acquired: TV GX2Invalidate ...");
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tv_scan_buffer, tv_scan_buffer_size);
    SPDLOG_INFO("gfx_wiiu: acquired: TV GX2Invalidate complete; GX2SetTVBuffer ...");
    GX2SetTVBuffer(tv_scan_buffer, tv_scan_buffer_size, tv_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                   GX2_BUFFERING_MODE_DOUBLE);
    SPDLOG_INFO("gfx_wiiu: acquired: GX2SetTVBuffer complete");

    drc_scan_buffer = gfx_wiiu_alloc_foreground(drc_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
    if (!drc_scan_buffer) {
        SPDLOG_ERROR("gfx_wiiu: failed to allocate DRC scan buffer");
        return 0;
    }

    SPDLOG_INFO("gfx_wiiu: acquired: DRC GX2Invalidate ...");
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drc_scan_buffer, drc_scan_buffer_size);
    SPDLOG_INFO("gfx_wiiu: acquired: DRC GX2Invalidate complete; GX2SetDRCBuffer ...");
    GX2SetDRCBuffer(drc_scan_buffer, drc_scan_buffer_size, drc_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                    GX2_BUFFERING_MODE_DOUBLE);
    SPDLOG_INFO("gfx_wiiu: acquired: GX2SetDRCBuffer complete");

    return 0;
}

static uint32_t gfx_wiiu_proc_callback_released(void* context) {
    if (tv_scan_buffer) {
        gfx_wiiu_free_foreground(tv_scan_buffer);
        tv_scan_buffer = nullptr;
    }

    if (drc_scan_buffer) {
        gfx_wiiu_free_foreground(drc_scan_buffer);
        drc_scan_buffer = nullptr;
    }

    gfx_wiiu_destroy_foreground();

    has_foreground = false;

    return 0;
}

static void gfx_wiiu_init(const char* game_name, const char* gfx_api_name, bool start_in_fullscreen, uint32_t width,
                          uint32_t height) {
    SPDLOG_INFO("gfx_wiiu_init: WHBProcInit ...");
    WHBProcInit();
    SPDLOG_INFO("gfx_wiiu_init: WHBProcInit ok");

    uint32_t mem1_addr, mem1_size;
    SPDLOG_INFO("gfx_wiiu_init: OSGetMemBound ...");
    OSGetMemBound(OS_MEM1, &mem1_addr, &mem1_size);
    SPDLOG_INFO("gfx_wiiu_init: OSGetMemBound values addr=0x{:08X} size=0x{:08X}", mem1_addr, mem1_size);
    SPDLOG_INFO("gfx_wiiu_init: OSGetMemBound ok");

    SPDLOG_INFO("gfx_wiiu_init: memalign(mem1_storage) ... alignment=0x40 size=0x{:08X} ({})", mem1_size,
                mem1_size);
    mem1_storage = memalign(0x40, mem1_size);
    SPDLOG_INFO("gfx_wiiu_init: memalign(mem1_storage) returned ptr={} requested_size=0x{:08X}", mem1_storage,
                mem1_size);
    if (!mem1_storage) {
        SPDLOG_ERROR("gfx_wiiu_init: memalign(mem1_storage) failed for size=0x{:08X}", mem1_size);
        return;
    }
    SPDLOG_INFO("gfx_wiiu_init: memalign(mem1_storage) ok");

    SPDLOG_INFO("gfx_wiiu_init: ProcUISetMEM1Storage ... ptr={} size=0x{:08X}", mem1_storage, mem1_size);
    ProcUISetMEM1Storage(mem1_storage, mem1_size);
    SPDLOG_INFO("gfx_wiiu_init: ProcUISetMEM1Storage ok");

    SPDLOG_INFO("gfx_wiiu_init: gfx_wiiu_init_mem1 ...");
    bool result = gfx_wiiu_init_mem1();
    if (!result) {
        SPDLOG_ERROR("gfx_wiiu_init: gfx_wiiu_init_mem1 failed");
        return;
    }
    SPDLOG_INFO("gfx_wiiu_init: gfx_wiiu_init_mem1 ok");

    SPDLOG_INFO("gfx_wiiu_init: memalign(command_buffer_pool) ... alignment=0x{:X} size=0x{:X}",
                GX2_COMMAND_BUFFER_ALIGNMENT, 0x400000);
    command_buffer_pool = memalign(GX2_COMMAND_BUFFER_ALIGNMENT, 0x400000);
    SPDLOG_INFO("gfx_wiiu_init: memalign(command_buffer_pool) returned ptr={} requested_size=0x{:X}",
                command_buffer_pool, 0x400000);
    if (!command_buffer_pool) {
        SPDLOG_ERROR("gfx_wiiu_init: memalign(command_buffer_pool) failed for size=0x{:X}", 0x400000);
        return;
    }
    SPDLOG_INFO("gfx_wiiu_init: memalign(command_buffer_pool) ok");

    SPDLOG_INFO("gfx_wiiu_init: memalign(secondary_command_buffer) ... alignment=0x{:X} size=0x{:X}",
                GX2_COMMAND_BUFFER_ALIGNMENT, 0x400000);
    secondary_command_buffer = memalign(GX2_COMMAND_BUFFER_ALIGNMENT, 0x400000);
    SPDLOG_INFO("gfx_wiiu_init: memalign(secondary_command_buffer) returned ptr={} requested_size=0x{:X}",
                secondary_command_buffer, 0x400000);
    if (!secondary_command_buffer) {
        SPDLOG_ERROR("gfx_wiiu_init: memalign(secondary_command_buffer) failed for size=0x{:X}", 0x400000);
        return;
    }
    SPDLOG_INFO("gfx_wiiu_init: memalign(secondary_command_buffer) ok");

    uint32_t initAttribs[] = { GX2_INIT_CMD_BUF_BASE,
                               (uintptr_t)command_buffer_pool,
                               GX2_INIT_CMD_BUF_POOL_SIZE,
                               0x400000,
                               GX2_INIT_ARGC,
                               0,
                               GX2_INIT_ARGV,
                               0,
                               GX2_INIT_END };
    SPDLOG_INFO("gfx_wiiu_init: GX2Init ...");
    GX2Init(initAttribs);
    SPDLOG_INFO("gfx_wiiu_init: GX2Init ok");
    GX2SetEventCallback(GX2_EVENT_TYPE_DISPLAY_LIST_OVERRUN, gfx_wiiu_display_list_overrun_callback, nullptr);
    GX2SetEventCallback(GX2_EVENT_TYPE_FLIP, gfx_wiiu_flip_callback, nullptr);

    SPDLOG_INFO("gfx_wiiu_init: GX2GetSystemTVScanMode ...");
    auto tv_scan_mode = GX2GetSystemTVScanMode();
    SPDLOG_INFO("gfx_wiiu_init: GX2GetSystemTVScanMode returned mode={}", static_cast<int>(tv_scan_mode));
    SPDLOG_INFO("gfx_wiiu_init: GX2GetSystemTVScanMode ok");
    switch (tv_scan_mode) {
        case GX2_TV_SCAN_MODE_480I:
        case GX2_TV_SCAN_MODE_480P:
            tv_render_mode = GX2_TV_RENDER_MODE_WIDE_480P;
            tv_width = 854;
            tv_height = 480;
            break;
        case GX2_TV_SCAN_MODE_1080I:
        case GX2_TV_SCAN_MODE_1080P:
            tv_render_mode = GX2_TV_RENDER_MODE_WIDE_1080P;
            tv_width = 1920;
            tv_height = 1080;
            break;
        case GX2_TV_SCAN_MODE_720P:
        default:
            tv_render_mode = GX2_TV_RENDER_MODE_WIDE_720P;
            tv_width = 1280;
            tv_height = 720;
            break;
    }

    SPDLOG_INFO("gfx_wiiu_init: GX2GetSystemDRCScanMode ...");
    drc_render_mode = GX2GetSystemDRCScanMode();
    SPDLOG_INFO("gfx_wiiu_init: GX2GetSystemDRCScanMode returned mode={}", static_cast<int>(drc_render_mode));
    SPDLOG_INFO("gfx_wiiu_init: GX2GetSystemDRCScanMode ok");

    uint32_t unk;
    SPDLOG_INFO("gfx_wiiu_init: GX2CalcTVSize ... render_mode={} format={} buffering={}",
                static_cast<int>(tv_render_mode), static_cast<int>(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8),
                static_cast<int>(GX2_BUFFERING_MODE_DOUBLE));
    GX2CalcTVSize(tv_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &tv_scan_buffer_size,
                  &unk);
    SPDLOG_INFO("gfx_wiiu_init: GX2CalcTVSize ok size=0x{:X} unk=0x{:X}", tv_scan_buffer_size, unk);
    SPDLOG_INFO("gfx_wiiu_init: GX2CalcDRCSize ... render_mode={} format={} buffering={}",
                static_cast<int>(drc_render_mode), static_cast<int>(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8),
                static_cast<int>(GX2_BUFFERING_MODE_DOUBLE));
    GX2CalcDRCSize(drc_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE,
                   &drc_scan_buffer_size, &unk);
    SPDLOG_INFO("gfx_wiiu_init: GX2CalcDRCSize ok size=0x{:X} unk=0x{:X}", drc_scan_buffer_size, unk);

    SPDLOG_INFO("gfx_wiiu_init: ProcUIRegisterCallback(acquire) ...");
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, gfx_wiiu_proc_callback_acquired, nullptr, 100);
    SPDLOG_INFO("gfx_wiiu_init: ProcUIRegisterCallback(acquire) ok");
    SPDLOG_INFO("gfx_wiiu_init: ProcUIRegisterCallback(release) ...");
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, gfx_wiiu_proc_callback_released, nullptr, 100);
    SPDLOG_INFO("gfx_wiiu_init: ProcUIRegisterCallback(release) ok");

    SPDLOG_INFO("gfx_wiiu_init: gfx_wiiu_proc_callback_acquired ...");
    gfx_wiiu_proc_callback_acquired(nullptr);
    SPDLOG_INFO("gfx_wiiu_init: gfx_wiiu_proc_callback_acquired ok");

    SPDLOG_INFO("gfx_wiiu_init: memalign(context_state) ... alignment=0x{:X} size=0x{:X}",
                GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    context_state = (GX2ContextState*)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    SPDLOG_INFO("gfx_wiiu_init: memalign(context_state) returned ptr={} requested_size=0x{:X}",
                static_cast<const void*>(context_state), sizeof(GX2ContextState));
    if (!context_state) {
        SPDLOG_ERROR("gfx_wiiu_init: memalign(context_state) failed for size=0x{:X}", sizeof(GX2ContextState));
        return;
    }
    SPDLOG_INFO("gfx_wiiu_init: memalign(context_state) ok");

    SPDLOG_INFO("gfx_wiiu_init: GX2SetupContextStateEx ...");
    GX2SetupContextStateEx(context_state, TRUE);
    SPDLOG_INFO("gfx_wiiu_init: GX2SetupContextStateEx ok");
    SPDLOG_INFO("gfx_wiiu_init: GX2SetContextState ...");
    GX2SetContextState(context_state);
    SPDLOG_INFO("gfx_wiiu_init: GX2SetContextState ok");

    SPDLOG_INFO("gfx_wiiu_init: GX2SetTVScale ... width={} height={}", WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);
    GX2SetTVScale(WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);
    SPDLOG_INFO("gfx_wiiu_init: GX2SetTVScale ok");
    SPDLOG_INFO("gfx_wiiu_init: GX2SetDRCScale ... width={} height={}", WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);
    GX2SetDRCScale(WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);
    SPDLOG_INFO("gfx_wiiu_init: GX2SetDRCScale ok");

    SPDLOG_INFO("gfx_wiiu_init: GX2SetSwapInterval ... divisor={}", frame_divisor);
    GX2SetSwapInterval(frame_divisor);
    SPDLOG_INFO("gfx_wiiu_init: GX2SetSwapInterval ok");

    SPDLOG_INFO("gfx_wiiu_init: Fast3dGui::Init ...");
    Fast::GuiWindowInitData window_impl;
    window_impl.Backend = Fast::WindowBackend::FAST3D_WIIU_GX2;
    window_impl.Gx2.Width = WIIU_DEFAULT_FB_WIDTH;
    window_impl.Gx2.Height = WIIU_DEFAULT_FB_HEIGHT;
    std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())
        ->Init(window_impl);
    SPDLOG_INFO("gfx_wiiu_init: Fast3dGui::Init ok");

    SPDLOG_INFO("gfx_wiiu_init: complete ok");
}

static void gfx_wiiu_shutdown(void) {
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_shutdown enter\n");

    if (has_foreground) {
        gfx_wiiu_proc_callback_released(nullptr);
        gfx_wiiu_destroy_mem1();
    }

    GX2Shutdown();

    if (context_state) {
        free(context_state);
        context_state = nullptr;
    }

    if (command_buffer_pool) {
        free(command_buffer_pool);
        command_buffer_pool = nullptr;
    }

    if (secondary_command_buffer) {
        free(secondary_command_buffer);
        secondary_command_buffer = nullptr;
    }

    ProcUISetMEM1Storage(nullptr, 0);
    free(mem1_storage);

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_shutdown exit\n");
}

void gfx_wiiu_set_context_state(void) {
    GX2SetContextState(context_state);
}

static void gfx_wiiu_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_wiiu_set_fullscreen(bool enable) {
}

static void gfx_wiiu_get_active_window_refresh_rate(uint32_t* refresh_rate) {
    *refresh_rate = 60;
}

static void gfx_wiiu_show_cursor(bool hide) {
}

static void gfx_wiiu_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode),
                                            void (*on_all_keys_up)(void)) {
}

// The modern window owns the frame loop, so the old main_loop's exit path has to
// live somewhere: IsRunning() reports ProcUI's state and Destroy() performs the
// teardown that used to run after the while() fell through. Losing that teardown
// would leak the GX2 context and skip WHBProcShutdown on exit.
bool gfx_wiiu_is_running(void) {
    return WHBProcIsRunning();
}

void gfx_wiiu_teardown(void) {
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_teardown enter\n");

    Ship::WiiU::Exit();
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: WiiU::Exit exit\n");

    gfx_gx2_shutdown();
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown returned\n");
    gfx_wiiu_shutdown();
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_shutdown returned\n");
    WHBProcShutdown();

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_teardown exit\n");
}

static void gfx_wiiu_main_loop(void (*run_one_game_iter)(void)) {
    while (WHBProcIsRunning()) {
        run_one_game_iter();
    }

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_main_loop enter\n");

    Ship::WiiU::Exit();
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: WiiU::Exit exit\n");

    gfx_gx2_shutdown();
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown returned\n");
    gfx_wiiu_shutdown();
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_shutdown returned\n");
    WHBProcShutdown();

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_wiiu_main_loop exit\n");
}

static void gfx_wiiu_get_dimensions(uint32_t* width, uint32_t* height) {
    *width = WIIU_DEFAULT_FB_WIDTH;
    *height = WIIU_DEFAULT_FB_HEIGHT;
}

static void gfx_wiiu_handle_events(void) {
    Ship::WiiU::Update();

    ImGui_ImplWiiU_ControllerInput input{};

    VPADReadError vpad_error;
    input.vpad = Ship::WiiU::GetVPADStatus(&vpad_error);
    if (vpad_error != VPAD_READ_SUCCESS) {
        input.vpad = nullptr;
    }

    KPADError kpad_error;
    for (int i = 0; i < 4; i++) {
        input.kpad[i] = Ship::WiiU::GetKPADStatus((WPADChan)i, &kpad_error);
        if (kpad_error != KPAD_ERROR_OK) {
            input.kpad[i] = nullptr;
        }
    }

    // Was LUS::EventImpl + LUS::UpdateGui. Modern libultraship drives the GUI from
    // Fast3dWindow, so hand the gamepad state straight to the ImGui Wii U backend,
    // which is the only consumer that understands VPAD/WPAD.
    ImGui_ImplWiiU_ProcessInput(&input);
}

static bool gfx_wiiu_start_frame(void) {
    uint32_t swap_count, flip_count;
    OSTime last_flip, last_vsync;
    uint32_t wait_count = 0;

    while (true) {
        GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);

        if (flip_count >= swap_count) {
            break;
        }

        if (wait_count >= 10) {
            // GPU timed out, drop frame. Silent until now: if the GPU is falling behind
            // before the freeze, this is where it shows, and nothing recorded it.
            static uint32_t timeouts = 0;
            timeouts++;
            if (timeouts <= 5 || (timeouts % 60) == 0) {
                Ship::WiiU::Watchdog::Emit("GX2: frame dropped, GPU timeout n=%u swap=%u flip=%u\n",
                                           timeouts, swap_count, flip_count);
            }
            return false;
        }

        wait_count++;
        // Blocks on the GPU's vsync interrupt. If the display pipeline stops, this never
        // returns - which is one way the main thread could stop without any CPU fault.
        GX2WaitForVsync();
    }

    return true;
}

static void gfx_wiiu_swap_buffers_begin(void) {
    GX2SwapScanBuffers();
    GX2Flush();

    gfx_wiiu_set_context_state();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
}

static void gfx_wiiu_swap_buffers_end(void) {
    static OSTick tick = 0;
    frametime = OSTicksToMicroseconds(OSGetSystemTick() - tick);
    tick = OSGetSystemTick();
}

static double gfx_wiiu_get_time(void) {
    return 0.0;
}

static void gfx_wiiu_set_target_fps(int fps) {
    // use the nearest divisor
    int divisor = 60 / fps;
    if (divisor < 1) {
        divisor = 1;
    }

    if (frame_divisor != divisor) {
        GX2SetSwapInterval(divisor);
        frame_divisor = divisor;
    }
}

static void gfx_wiiu_set_maximum_frame_latency(int latency) {
    (void)latency;
}

static const char* gfx_wiiu_get_key_name(int scancode) {
    return "";
}

bool gfx_wiiu_can_disable_vsync() {
    return false;
}

// The C GfxWindowManagerAPI dispatch struct is retired: GfxWindowBackendWiiU
// below implements the abstract interface that replaced it.

#endif

// ---------------------------------------------------------------------------
// GfxWindowBackendWiiU — C++ facade over the ProcUI/GX2 window code above.
//
// Modern libultraship replaced the C `GfxWindowManagerAPI` struct (18 entries)
// with an abstract class (32 virtuals). Most additions are pointer input, which
// the Wii U does not have; those are implemented as honest no-ops rather than
// fabricated coordinates. GamePad touch reaches ImGui through imgui_impl_wiiu.
// ---------------------------------------------------------------------------

#include "fast/backends/gfx_window_backend_wiiu.h"

namespace Fast {

void GfxWindowBackendWiiU::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                uint32_t height, int32_t posX, int32_t posY) {
    // The Wii U framebuffer is fixed by the TV/GamePad scan-out; position and the
    // requested size are meaningless here and are deliberately ignored.
    (void)posX;
    (void)posY;
    gfx_wiiu_init(gameName, apiName, startFullScreen, width, height);
}

void GfxWindowBackendWiiU::Close() {
    gfx_wiiu_close();
}

void GfxWindowBackendWiiU::Destroy() {
    // Full teardown, mirroring what gfx_wiiu_main_loop() used to run once its
    // WHBProcIsRunning() loop exited.
    gfx_wiiu_teardown();
}

void GfxWindowBackendWiiU::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                void (*onAllKeysUp)()) {
    gfx_wiiu_set_keyboard_callbacks(onKeyDown, onKeyUp, onAllKeysUp);
}

void GfxWindowBackendWiiU::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) {
    // No mouse on Wii U.
    (void)onMouseButtonDown;
    (void)onMouseButtonUp;
}

void GfxWindowBackendWiiU::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool isNowFullscreen)) {
    gfx_wiiu_set_fullscreen_changed_callback(onFullscreenChanged);
}

void GfxWindowBackendWiiU::SetFullscreen(bool fullscreen) {
    gfx_wiiu_set_fullscreen(fullscreen);
}

bool GfxWindowBackendWiiU::IsFullscreen() {
    // Scan-out is always full screen; there is no windowed mode to report.
    return true;
}

void GfxWindowBackendWiiU::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    gfx_wiiu_get_active_window_refresh_rate(refreshRate);
}

void GfxWindowBackendWiiU::SetCursorVisibility(bool visible) {
    gfx_wiiu_show_cursor(visible);
}

void GfxWindowBackendWiiU::SetMousePos(int32_t posX, int32_t posY) {
    (void)posX;
    (void)posY;
}

void GfxWindowBackendWiiU::GetMousePos(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendWiiU::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendWiiU::GetMouseWheel(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

bool GfxWindowBackendWiiU::GetMouseState(uint32_t btn) {
    (void)btn;
    return false;
}

void GfxWindowBackendWiiU::SetMouseCapture(bool capture) {
    (void)capture;
}

bool GfxWindowBackendWiiU::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendWiiU::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    gfx_wiiu_get_dimensions(width, height);
    if (posX != nullptr) {
        *posX = 0;
    }
    if (posY != nullptr) {
        *posY = 0;
    }
}

void GfxWindowBackendWiiU::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    // Fixed scan-out: resizing is not possible, so this is a no-op rather than a
    // silent partial resize that would desync Fast3D's idea of the framebuffer.
    (void)width;
    (void)height;
    (void)posX;
    (void)posY;
}

Ship::WindowRect GfxWindowBackendWiiU::GetPrimaryMonitorRect() {
    uint32_t width = 0;
    uint32_t height = 0;
    gfx_wiiu_get_dimensions(&width, &height);
    return Ship::WindowRect{ 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) };
}

void GfxWindowBackendWiiU::HandleEvents() {
    gfx_wiiu_handle_events();
}

bool GfxWindowBackendWiiU::IsFrameReady() {
    return gfx_wiiu_start_frame();
}

bool GfxWindowBackendWiiU::IsRunning() {
    // The old backend drove its own loop via gfx_wiiu_main_loop(); the modern
    // window owns the loop and asks us whether ProcUI still wants us alive.
    return gfx_wiiu_is_running();
}

void GfxWindowBackendWiiU::SwapBuffersBegin() {
    gfx_wiiu_swap_buffers_begin();
}

void GfxWindowBackendWiiU::SwapBuffersEnd() {
    gfx_wiiu_swap_buffers_end();
}

double GfxWindowBackendWiiU::GetTime() {
    return gfx_wiiu_get_time();
}

int GfxWindowBackendWiiU::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendWiiU::SetTargetFps(int fps) {
    mTargetFps = fps;
    gfx_wiiu_set_target_fps(fps);
}

void GfxWindowBackendWiiU::SetMaxFrameLatency(int latency) {
    gfx_wiiu_set_maximum_frame_latency(latency);
}

const char* GfxWindowBackendWiiU::GetKeyName(int scancode) {
    return gfx_wiiu_get_key_name(scancode);
}

bool GfxWindowBackendWiiU::CanDisableVsync() {
    return gfx_wiiu_can_disable_vsync();
}

} // namespace Fast
