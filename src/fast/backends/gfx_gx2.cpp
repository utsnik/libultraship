/*  gfx_gx2.cpp - Fast3D GX2 backend for libultraship

    Created in 2022 by GaryOderNichts
*/
#ifdef ENABLE_GX2

#include "fast/backends/gfx_gx2.h"
#include "port/wiiu/WiiUWatchdog.h"

#include "ship/window/Window.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <malloc.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "libultraship/libultra/gbi.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/Context.h"

#include "fast/interpreter.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_wiiu.h"

#include <gx2/texture.h>
#include <gx2/draw.h>
#include <gx2/clear.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2/event.h>
#include <gx2/utils.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/display.h>
#include "fast/backends/gx2_shader_gen.h"

#include <proc_ui/procui.h>
#include <coreinit/memory.h>
#include <coreinit/cache.h>
#include <coreinit/time.h>

#include "fast/backends/imgui_impl_gx2.h"

// WUT exports these GX2 GPU-cycle imports, but the installed event header does
// not declare them. Their ABI is the Cafe OS pointer form used by the GX2
// imports: the GPU writes a 64-bit cycle sample into the supplied address.
extern "C" {
void GX2SampleTopGPUCycle(uint64_t* result);
void GX2SampleBottomGPUCycle(uint64_t* result);
uint64_t GX2GPUTimeToCPUTime(uint64_t gpuTime);
}

// The whole translation unit lives in namespace Fast: the ported GPU7 code uses
// FilteringMode, GfxClipParameters and ShaderProgram, all of which libultraship
// moved into that namespace.
namespace Fast {

// The old backend called the free function CVarGetInteger() from
// core/bridge/consolevariablebridge.h. Modern libultraship reads cvars through a
// Ship::ConsoleVariable instance handed to the backend at construction. The GPU7
// code below is file-scope, so the instance is published here by the constructor
// and this shim keeps the ~20 original call sites untouched.
static std::shared_ptr<Ship::ConsoleVariable> gGx2ConsoleVariables;

static int32_t CVarGetInteger(const char* name, int32_t defaultValue) {
    // Before Init() runs (or if the backend was built without cvars) fall back to
    // the caller's default rather than dereferencing null.
    if (gGx2ConsoleVariables == nullptr) {
        return defaultValue;
    }
    return gGx2ConsoleVariables->GetInteger(name, defaultValue);
}

// Named GX2TextureEntry rather than Texture: libultraship has a Fast::Texture
// resource class, and this file now lives in namespace Fast.
struct GX2TextureEntry {
    GX2Texture texture;
    bool texture_uploaded;

    GX2Sampler sampler;
    bool sampler_set;

    // For ImGui rendering
    ImGui_ImplGX2_Texture imtex;
};

#define ALIGN(x, align) (((x) + ((align)-1)) & ~((align)-1))



struct Framebuffer {
    GX2ColorBuffer color_buffer;
    bool colorBufferMem1;
    GX2DepthBuffer depth_buffer;
    bool depthBufferMem1;

    GX2Texture texture;
    GX2Sampler sampler;

    // For ImGui rendering
    ImGui_ImplGX2_Texture imtex;
};

static struct Framebuffer main_framebuffer;
static std::map<int, struct Framebuffer*> framebuffer_registry = {{0, &main_framebuffer}};
static struct Framebuffer* current_framebuffer;

static std::map<std::pair<uint64_t, uint64_t>, struct ShaderProgram> shader_program_pool;
static struct ShaderProgram* current_shader_program;

static struct GX2TextureEntry* current_texture;
static int current_tile;

// The texture and sampler last selected for each tile (sampler slot). The interpreter selects and
// uploads a draw's textures BEFORE it loads that draw's shader, and a bind is only possible while the
// current shader has a sampler for the tile. When the previous shader had none, the bind was skipped
// and nothing rebound it, so the draw sampled whatever texture the slot held before. Prerendered room
// backgrounds (a textured copy after an untextured draw) came out white. load_shader rebinds these.
static GX2Texture* tile_bound_texture[6];
static GX2Sampler* tile_bound_sampler[6];

// 8 MiB per frame arena. Exhaustion keeps the serialized fallback below.
#define DRAW_BUFFER_SIZE 0x800000

struct DepthReadbackRequest {
    struct Framebuffer* framebuffer;
    std::pair<float, float> coordinate;
};

struct DrawBufferSlot {
    uint8_t* buffer = nullptr;
    OSTime submitted_timestamp = 0;
    uint32_t cpu_microseconds = 0;
    bool gpu_timing_pending = false;
    GX2DepthBuffer depth_read_buffer = {};
    std::vector<DepthReadbackRequest> depth_read_requests;
    bool depth_readback_pending = false;
};

static DrawBufferSlot draw_buffer_slots[2];
// GPU-written timestamp pair per slot ([0]=top, [1]=bottom), each on its own cache line so
// CPU writebacks of neighbouring data can never clobber what the GPU wrote.
alignas(64) static uint64_t gpu_timing_samples[2][8];
static uint64_t* gfx_gx2_gpu_samples(DrawBufferSlot& slot) {
    return gpu_timing_samples[&slot - draw_buffer_slots];
}
static uint32_t draw_buffer_slot_index = 0;
static bool draw_buffer_double_buffered = false;
static uint8_t* draw_buffer = nullptr;
static uint8_t* draw_ptr = nullptr;
static uint32_t draw_buffer_frame_high_water = 0;
static OSTime frame_start_time = 0;
static uint64_t perf_gpu_us = 0;
static uint32_t perf_gpu_frames = 0;
static uint32_t perf_gpu_max_us = 0;
static uint64_t perf_slot_wait_us = 0;
static uint32_t perf_texture_uploads = 0;
static uint64_t perf_gx2_draws = 0;
static uint64_t perf_gx2_triangles = 0;
static uint64_t perf_osblockmove_us = 0;
static uint64_t perf_gx2_invalidate_us = 0;
static uint64_t perf_gx2_set_attrib_buffer_us = 0;
static uint64_t perf_gx2_draw_ex_us = 0;

static constexpr size_t PERF_FLUSH_COUNT = 8;
static constexpr size_t PERF_FLUSH_TEXTURE = 0;
static constexpr size_t PERF_FLUSH_SAMPLER = 1;
static constexpr size_t PERF_FLUSH_SHADER = 2;
static constexpr size_t PERF_FLUSH_DEPTH_ZMODE = 3;
static constexpr size_t PERF_FLUSH_VIEWPORT_SCISSOR = 4;
static constexpr size_t PERF_FLUSH_ALPHA = 5;
static constexpr size_t PERF_FLUSH_TRIANGLE_CAP = 6;
static constexpr size_t PERF_FLUSH_EXPLICIT = 7;

extern "C" void FastGetAndResetInterpreterPerf(uint64_t* vertices, uint64_t flushCounts[]);
extern "C" void FastGetAndResetCullDlPerf(uint64_t* tested, uint64_t* rejected);
extern "C" void FastGetAndResetSubDlPerf(uint64_t* tested, uint64_t* culled);
extern "C" void FastGetAndResetVtxBatchPerf(uint64_t* tested, uint64_t* culled, uint64_t* staleTris);
extern "C" uint64_t FastGetAndResetFalseCull(void);
extern "C" uint64_t FastGetAndResetStaleTrisSubDl(void);
// Defined by SoH z_room.c (cullable room entries tested / rejected by the side-plane test).
extern "C" uint32_t gWiiURoomEntriesTested __attribute__((weak));
extern "C" uint32_t gWiiURoomEntriesSideCulled __attribute__((weak));

static std::map<std::pair<struct Framebuffer*, std::pair<float, float>>, uint16_t> depth_readback_cache;

static uint32_t frame_count;
static bool gfx_gx2_trace_first_frame = true;
static bool gfx_gx2_trace_first_draw = true;
static float current_noise_scale;
static FilteringMode current_filter_mode = FILTER_LINEAR;
static BOOL current_depth_test = TRUE;
static BOOL current_depth_write = TRUE;
static GX2CompareFunction current_depth_compare_function = GX2_COMPARE_FUNC_LESS;

static float current_viewport_x = 0.0f;
static float current_viewport_y = 0.0f;
static float current_viewport_width = WIIU_DEFAULT_FB_WIDTH;
static float current_viewport_height = WIIU_DEFAULT_FB_HEIGHT;

static uint32_t current_scissor_x = 0;
static uint32_t current_scissor_y = 0;
static uint32_t current_scissor_width = WIIU_DEFAULT_FB_WIDTH;
static uint32_t current_scissor_height = WIIU_DEFAULT_FB_HEIGHT;

static bool current_zmode_decal = false;
static bool current_SSDB = -2.0f;
static bool current_use_alpha = false;

static uint32_t gfx_gx2_elapsed_microseconds(OSTime start, OSTime end) {
    if (end <= start) {
        return 0;
    }
    const uint64_t microseconds = OSTicksToMicroseconds(end - start);
    return microseconds > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(microseconds);
}

static bool gfx_gx2_resize_depth_readback(DrawBufferSlot& slot, uint32_t capacity) {
    if (slot.depth_read_buffer.surface.image && slot.depth_read_buffer.surface.width >= capacity) {
        return true;
    }

    GX2DepthBuffer replacement;
    memcpy(&replacement, &main_framebuffer.depth_buffer, sizeof(replacement));
    replacement.surface.image = nullptr;
    replacement.surface.mipmaps = nullptr;
    replacement.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    replacement.surface.width = capacity;
    replacement.surface.height = 1;
    GX2CalcSurfaceSizeAndAlignment(&replacement.surface);

    replacement.surface.image =
        gfx_wiiu_alloc_mem1(replacement.surface.imageSize, replacement.surface.alignment);
    if (!replacement.surface.image) {
        SPDLOG_ERROR("gfx_gx2: failed to allocate depth readback buffer for {} coordinates", capacity);
        return false;
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER, replacement.surface.image,
                  replacement.surface.imageSize);
    if (slot.depth_read_buffer.surface.image) {
        gfx_wiiu_free_mem1(slot.depth_read_buffer.surface.image);
    }
    slot.depth_read_buffer = replacement;
    return true;
}

static void gfx_gx2_collect_depth_readback(DrawBufferSlot& slot) {
    if (!slot.depth_readback_pending || slot.submitted_timestamp == 0 ||
        slot.submitted_timestamp > GX2GetRetiredTimeStamp()) {
        return;
    }

    // The frame timestamp is also the fence for these GPU writes. Invalidate
    // the CPU cache only after that fence has retired and before reading them.
    DCInvalidateRange(slot.depth_read_buffer.surface.image, slot.depth_read_buffer.surface.imageSize);
    const uint32_t* depth_values = static_cast<const uint32_t*>(slot.depth_read_buffer.surface.image);
    for (size_t i = 0; i < slot.depth_read_requests.size(); ++i) {
        const uint32_t bits = __builtin_bswap32(depth_values[i]);
        float depth;
        memcpy(&depth, &bits, sizeof(depth));
        const DepthReadbackRequest& request = slot.depth_read_requests[i];
        depth_readback_cache[{ request.framebuffer, request.coordinate }] =
            static_cast<uint16_t>(depth * 65532.0f);
    }

    slot.depth_readback_pending = false;
    slot.depth_read_requests.clear();
}

static void gfx_gx2_collect_retired_depth_readbacks() {
    const OSTime retired_timestamp = GX2GetRetiredTimeStamp();
    DrawBufferSlot* first = nullptr;
    DrawBufferSlot* second = nullptr;

    for (DrawBufferSlot& slot : draw_buffer_slots) {
        if (!slot.depth_readback_pending || slot.submitted_timestamp == 0 ||
            slot.submitted_timestamp > retired_timestamp) {
            continue;
        }
        if (!first || slot.submitted_timestamp < first->submitted_timestamp) {
            second = first;
            first = &slot;
        } else {
            second = &slot;
        }
    }

    // Apply completed slots in submission order so duplicate coordinates keep
    // the newest retired value.
    if (first) {
        gfx_gx2_collect_depth_readback(*first);
    }
    if (second) {
        gfx_gx2_collect_depth_readback(*second);
    }
}

static void gfx_gx2_draw_done(const char* detail) {
    const OSTime start = OSGetSystemTime();
    {
        WDOG_SCOPE(::Ship::WiiU::Watchdog::PH_GX2_DRAW_DONE, detail);
        GX2DrawDone();
    }
    Ship::WiiU::Watchdog::RecordGX2Wait(gfx_gx2_elapsed_microseconds(start, OSGetSystemTime()));
}

static void gfx_gx2_collect_gpu_timing(DrawBufferSlot& slot) {
    if (!slot.gpu_timing_pending) {
        return;
    }

    // The timestamp output is written by the GPU. This function is called only
    // after the slot's submission timestamp has retired, so reading it cannot
    // introduce a hidden synchronization point.
    uint64_t* samples = gfx_gx2_gpu_samples(slot);
    DCInvalidateRange(samples, sizeof(gpu_timing_samples[0]));
    const uint64_t top_cpu_time = GX2GPUTimeToCPUTime(samples[0]);
    const uint64_t bottom_cpu_time = GX2GPUTimeToCPUTime(samples[1]);
    uint32_t gpu_microseconds = 0;
    if (bottom_cpu_time > top_cpu_time) {
        gpu_microseconds = gfx_gx2_elapsed_microseconds(top_cpu_time, bottom_cpu_time);
    }
    Ship::WiiU::Watchdog::RecordFrameTiming(gpu_microseconds, slot.cpu_microseconds);
    perf_gpu_us += gpu_microseconds;
    ++perf_gpu_frames;
    perf_gpu_max_us = std::max(perf_gpu_max_us, gpu_microseconds);
    slot.gpu_timing_pending = false;
}

// Texture images the GPU may still sample are freed only after the submission that can reference them has
// retired. A GX2DrawDone per reallocation stalled the CPU for a whole GPU frame (~43 ms) and was the in-play
// stutter in Hyrule Field; freeing without any fence hard-freezes the console. fence 0 = not yet submitted:
// stamped with the frame's submitted timestamp in end_frame, which covers every draw recorded so far.
struct PendingImageFree {
    void* image;
    uint32_t size;
    OSTime fence;
};
static std::vector<PendingImageFree> pending_image_frees;

static void gfx_gx2_defer_image_free(void* image, uint32_t size) {
    pending_image_frees.push_back({ image, size, 0 });
}

static void gfx_gx2_stamp_pending_image_frees(OSTime fence) {
    for (PendingImageFree& pending : pending_image_frees) {
        if (pending.fence == 0) {
            pending.fence = fence;
        }
    }
}

// all = true only after a GX2DrawDone (nothing can still be in flight).
static void gfx_gx2_release_pending_image_frees(bool all) {
    if (pending_image_frees.empty()) {
        return;
    }
    const OSTime retired = GX2GetRetiredTimeStamp();
    size_t kept = 0;
    for (PendingImageFree& pending : pending_image_frees) {
        if (all || (pending.fence != 0 && pending.fence <= retired)) {
            WDOG_TEXFREE(pending.size);
            free(pending.image);
        } else {
            pending_image_frees[kept++] = pending;
        }
    }
    pending_image_frees.resize(kept);
}

static void gfx_gx2_wait_for_draw_buffer_slot(DrawBufferSlot& slot) {
    if (slot.submitted_timestamp != 0) {
        const OSTime retired_timestamp = GX2GetRetiredTimeStamp();
        if (slot.submitted_timestamp > retired_timestamp) {
            const OSTime start = OSGetSystemTime();
            {
                WDOG_SCOPE(::Ship::WiiU::Watchdog::PH_GX2_SLOT_WAIT, "gx2-slot-wait");
                GX2WaitTimeStamp(slot.submitted_timestamp);
            }
            const uint32_t wait_us = gfx_gx2_elapsed_microseconds(start, OSGetSystemTime());
            perf_slot_wait_us += wait_us;
            Ship::WiiU::Watchdog::RecordGX2SlotWait(wait_us);
        }
    }

    gfx_gx2_collect_gpu_timing(slot);
}

static void gfx_gx2_prepare_draw_buffer_slot() {
    DrawBufferSlot& slot = draw_buffer_slots[draw_buffer_slot_index];
    if (draw_buffer_double_buffered) {
        gfx_gx2_wait_for_draw_buffer_slot(slot);
        gfx_gx2_collect_retired_depth_readbacks();
        slot.submitted_timestamp = 0;
    }
    gfx_gx2_release_pending_image_frees(false);
    draw_buffer = slot.buffer;
    draw_ptr = draw_buffer;

    uint64_t* samples = gfx_gx2_gpu_samples(slot);
    samples[0] = 0;
    samples[1] = 0;
    DCFlushRange(samples, sizeof(gpu_timing_samples[0]));
    slot.cpu_microseconds = 0;
    slot.gpu_timing_pending = true;
    GX2SampleTopGPUCycle(&samples[0]);
}

static inline GX2SamplerVar* GX2GetPixelSamplerVar(const GX2PixelShader* shader, const char* name) {
    for (uint32_t i = 0; i < shader->samplerVarCount; ++i) {
        if (strcmp(name, shader->samplerVars[i].name) == 0) {
            return &shader->samplerVars[i];
        }
    }

    return nullptr;
}

static inline int32_t GX2GetPixelSamplerVarLocation(const GX2PixelShader* shader, const char* name) {
    GX2SamplerVar* sampler = GX2GetPixelSamplerVar(shader, name);
    return sampler ? sampler->location : -1;
}

static inline int32_t GX2GetPixelUniformVarOffset(const GX2PixelShader* shader, const char* name) {
    GX2UniformVar* uniform = GX2GetPixelUniformVar(shader, name);
    return uniform ? uniform->offset : -1;
}

static const char* gfx_gx2_get_name() {
    return "GX2";
}

static int gfx_gx2_get_max_texture_size() {
    // TODO: This should be a define from the Wii U toolchain, but there isn't one yet
    return 8192;
}

static void gfx_gx2_init_framebuffer(struct Framebuffer* buffer, uint32_t width, uint32_t height) {
    SPDLOG_INFO("gfx_gx2: framebuffer descriptor setup {}x{} ...", width, height);
    memset(&buffer->color_buffer, 0, sizeof(GX2ColorBuffer));
    buffer->color_buffer.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    buffer->color_buffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    buffer->color_buffer.surface.width = width;
    buffer->color_buffer.surface.height = height;
    buffer->color_buffer.surface.depth = 1;
    buffer->color_buffer.surface.mipLevels = 1;
    buffer->color_buffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    buffer->color_buffer.surface.aa = GX2_AA_MODE1X;
    buffer->color_buffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    buffer->color_buffer.viewNumSlices = 1;

    memset(&buffer->depth_buffer, 0, sizeof(GX2DepthBuffer));
    buffer->depth_buffer.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER | GX2_SURFACE_USE_TEXTURE;
    buffer->depth_buffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    buffer->depth_buffer.surface.width = width;
    buffer->depth_buffer.surface.height = height;
    buffer->depth_buffer.surface.depth = 1;
    buffer->depth_buffer.surface.mipLevels = 1;
    buffer->depth_buffer.surface.format = GX2_SURFACE_FORMAT_FLOAT_R32;
    buffer->depth_buffer.surface.aa = GX2_AA_MODE1X;
    buffer->depth_buffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    buffer->depth_buffer.viewNumSlices = 1;
    buffer->depth_buffer.depthClear = 1.0f;
    SPDLOG_INFO("gfx_gx2: framebuffer descriptor setup {}x{} complete", width, height);
}

static struct GfxClipParameters gfx_gx2_get_clip_parameters(void) {
    return { false, false };
}

static void gfx_gx2_set_uniforms(struct ShaderProgram* prg) {
    static bool trace_first_uniform_upload = true;
    const bool trace = trace_first_uniform_upload;
    float window_params_array[4] = { current_noise_scale, (float)frame_count, 0.0f, 0.0f };

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame shader: GX2SetPixelUniformReg ... offset={}", prg->window_params_offset);
    }
    GX2SetPixelUniformReg(prg->window_params_offset, 4, window_params_array);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame shader: GX2SetPixelUniformReg complete");
        trace_first_uniform_upload = false;
    }
}

static void gfx_gx2_unload_shader(struct ShaderProgram* old_prg) {
    current_shader_program = nullptr;
}

static void gfx_gx2_load_shader(struct ShaderProgram* new_prg) {
    static bool trace_first_shader_load = true;
    const bool trace = trace_first_shader_load;
    current_shader_program = new_prg;

    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetFetchShader ...");
    }
    {
        WDOG_SCOPE_ARGS(::Ship::WiiU::Watchdog::PH_GX2_SET_FETCH_SHADER, "shader_id0=0x%08X%08X shader_id1=0x%08X",
                        static_cast<uint32_t>(new_prg->shader_id0 >> 32), static_cast<uint32_t>(new_prg->shader_id0),
                        static_cast<uint32_t>(new_prg->shader_id1));
        GX2SetFetchShader(&new_prg->group.fetchShader);
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetFetchShader complete; GX2SetVertexShader ...");
    }
    {
        WDOG_SCOPE_ARGS(::Ship::WiiU::Watchdog::PH_GX2_SET_VERTEX_SHADER, "shader_id0=0x%08X%08X shader_id1=0x%08X",
                        static_cast<uint32_t>(new_prg->shader_id0 >> 32), static_cast<uint32_t>(new_prg->shader_id0),
                        static_cast<uint32_t>(new_prg->shader_id1));
        GX2SetVertexShader(&new_prg->group.vertexShader);
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetVertexShader complete; GX2SetPixelShader ...");
    }
    {
        WDOG_SCOPE_ARGS(::Ship::WiiU::Watchdog::PH_GX2_SET_PIXEL_SHADER, "shader_id0=0x%08X%08X shader_id1=0x%08X",
                        static_cast<uint32_t>(new_prg->shader_id0 >> 32), static_cast<uint32_t>(new_prg->shader_id0),
                        static_cast<uint32_t>(new_prg->shader_id1));
        GX2SetPixelShader(&new_prg->group.pixelShader);
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetPixelShader complete; uniforms ...");
    }

    gfx_gx2_set_uniforms(new_prg);

    for (int tile = 0; tile < 6; tile++) {
        const int32_t location = new_prg->samplers_location[tile];
        if (location == -1) {
            continue;
        }
        if (tile_bound_texture[tile]) {
            GX2SetPixelTexture(tile_bound_texture[tile], location);
        }
        if (tile_bound_sampler[tile]) {
            GX2SetPixelSampler(tile_bound_sampler[tile], location);
        }
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load complete");
        trace_first_shader_load = false;
    }
}

static struct ShaderProgram* gfx_gx2_create_and_load_new_shader(uint64_t shader_id0, uint64_t shader_id1) {
    static bool trace_first_shader_creation = true;
    const bool trace = trace_first_shader_creation;
    struct CCFeatures cc_features;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation begin id0=0x{:016X} id1=0x{:08X}", shader_id0, shader_id1);
    }
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    struct ShaderProgram* prg = &shader_program_pool[std::make_pair(shader_id0, shader_id1)];
    WDOG_SHADER_PROGRAM_POOL_SIZE(shader_program_pool.size());
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;

    printf("Generating shader: %016llx-%08x\n", shader_id0, shader_id1);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation: GX2 shader compile/generate (gx2GenerateShaderGroup) ...");
    }
    // The GX2 shader generator's key is still a 32-bit option mask. Keep the
    // complete upstream key in the cache while passing the supported low bits
    // to the generator.
    const int shader_result = gx2GenerateShaderGroupWithKey(&prg->group, &cc_features, shader_id0,
                                                            static_cast<uint32_t>(shader_id1));
    if (shader_result != 0) {
        SPDLOG_ERROR("gfx_gx2: shader creation: gx2GenerateShaderGroup failed result={}", shader_result);
        printf("Failed to generate shader\n");
        current_shader_program = nullptr;
        trace_first_shader_creation = false;
        return nullptr;
    }
#if WIIU_DIAGNOSTICS
    Ship::WiiU::Watchdog::RecordShaderProgramCreated();
#endif
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation: GX2 shader compile/generate returned; shader upload/invalidate path ...");
    }

    prg->num_inputs = cc_features.numInputs;
    prg->used_textures[0] = cc_features.usedTextures[0];
    prg->used_textures[1] = cc_features.usedTextures[1];

    gfx_gx2_load_shader(prg);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation: sampler/uniform lookup ...");
    }

    prg->window_params_offset = GX2GetPixelUniformVarOffset(&prg->group.pixelShader, "window_params");
    prg->samplers_location[0] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTex0");
    prg->samplers_location[1] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTex1");
    prg->samplers_location[2] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexMask0");
    prg->samplers_location[3] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexMask1");
    prg->samplers_location[4] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexBlend0");
    prg->samplers_location[5] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexBlend1");

    prg->used_noise = cc_features.opt_alpha && cc_features.opt_noise;

    printf("Generated and loaded shader\n");

    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation complete; shader program upload and GX2Invalidate calls returned id0=0x{:016X} id1=0x{:08X}",
                    shader_id0, shader_id1);
        trace_first_shader_creation = false;
    }

    return prg;
}

static struct ShaderProgram* gfx_gx2_lookup_shader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = shader_program_pool.find(std::make_pair(shader_id0, shader_id1));
    return it == shader_program_pool.end() ? nullptr : &it->second;
}

static void gfx_gx2_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t gfx_gx2_new_texture(void) {
    WDOG_SCOPE(::Ship::WiiU::Watchdog::PH_GX2_NEW_TEXTURE, nullptr);
    // some 32-bit trickery :P
    struct GX2TextureEntry* tex = (struct GX2TextureEntry*)calloc(1, sizeof(struct GX2TextureEntry));
    if (!tex) {
        // Was an unchecked dereference of null on the next line.
        WDOG_EMIT("GX2TEX: !! new_texture calloc failed size=%u\n", (unsigned int)sizeof(struct GX2TextureEntry));
        SPDLOG_ERROR("gfx_gx2: texture entry allocation failed");
        return 0;
    }

    tex->imtex.Texture = &tex->texture;
    tex->imtex.Sampler = &tex->sampler;

    return (uint32_t)tex;
}

static void gfx_gx2_delete_texture(uint32_t texture_id) {
    struct GX2TextureEntry* tex = (struct GX2TextureEntry*)texture_id;
    for (int tile = 0; tile < 6; tile++) {
        if (tile_bound_texture[tile] == &tex->texture) {
            tile_bound_texture[tile] = nullptr;
        }
        if (tile_bound_sampler[tile] == &tex->sampler) {
            tile_bound_sampler[tile] = nullptr;
        }
    }

    if (tex->texture.surface.image) {
        gfx_gx2_defer_image_free(tex->texture.surface.image, tex->texture.surface.imageSize);
    }

    free((void*)tex);
}

static void gfx_gx2_set_pixel_texture(int tile, int32_t sampler_location, GX2Texture* texture) {
    WDOG_SCOPE_ARGS(::Ship::WiiU::Watchdog::PH_GX2_SET_PIXEL_TEXTURE,
                    "tile=%d samplerLocation=%d width=%u height=%u pitch=%u imageSize=%u", (uint32_t)tile,
                    (uint32_t)sampler_location, static_cast<uint32_t>(texture->surface.width),
                    static_cast<uint32_t>(texture->surface.height), static_cast<uint32_t>(texture->surface.pitch),
                    static_cast<uint32_t>(texture->surface.imageSize));
    GX2SetPixelTexture(texture, sampler_location);
}

static void gfx_gx2_select_texture(int tile, uint32_t texture_id) {
    WDOG_SCOPE_ARGS(::Ship::WiiU::Watchdog::PH_GX2_SELECT_TEXTURE, "tile=%d id=0x%08X", (uint32_t)tile,
                    (uint32_t)texture_id);
    static bool trace_first_texture_select = true;
    const bool trace = trace_first_texture_select;
    struct GX2TextureEntry* tex = (struct GX2TextureEntry*)texture_id;
    if (!tex) {
        SPDLOG_ERROR("gfx_gx2: texture selection received a null texture");
        return;
    }
    current_texture = tex;
    current_tile = tile;
    tile_bound_texture[tile] = tex->texture_uploaded ? &tex->texture : nullptr;
    tile_bound_sampler[tile] = tex->sampler_set ? &tex->sampler : nullptr;

    if (current_shader_program) {
        int32_t sampler_location = current_shader_program->samplers_location[tile];
        if (sampler_location != -1) {
            if (tex->texture_uploaded) {
                if (trace) {
                    SPDLOG_INFO("gfx_gx2: first frame texture select: GX2SetPixelTexture ... tile={} location={}", tile,
                                sampler_location);
                }
                gfx_gx2_set_pixel_texture(tile, sampler_location, &tex->texture);
                if (trace) {
                    SPDLOG_INFO("gfx_gx2: first frame texture select: GX2SetPixelTexture complete");
                }
            }

            if (tex->sampler_set) {
                if (trace) {
                    SPDLOG_INFO("gfx_gx2: first frame texture select: GX2SetPixelSampler ... tile={} location={}", tile,
                                sampler_location);
                }
                GX2SetPixelSampler(&tex->sampler, sampler_location);
                if (trace) {
                    SPDLOG_INFO("gfx_gx2: first frame texture select: GX2SetPixelSampler complete");
                }
            }
        }
    }
    if (trace) {
        trace_first_texture_select = false;
    }
}

static void gfx_gx2_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    // Distinct phase from the inner GX2Invalidate breadcrumb below. Both used to report
    // "gx2-tex-upload(returned)" with the same detail, so a frozen breadcrumb could not say
    // whether the wedge was after GX2Invalidate or after this whole function returned.
    WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_TEX_UPLOAD_FN, "%ux%u path=%s", (unsigned int)width,
                   (unsigned int)height, WDOG_TEXTURE_DETAIL());
#if WIIU_DIAGNOSTICS
    const OSTime uploadStart = OSGetSystemTime();
#endif
    static bool trace_first_texture_upload = true;
    const bool trace = trace_first_texture_upload;
    struct GX2TextureEntry* tex = current_texture;
    if (!tex) {
        SPDLOG_ERROR("gfx_gx2: texture upload requested without a current texture");
        return;
    }

    if ((tex->texture.surface.width != width) || (tex->texture.surface.height != height) ||
        !tex->texture.surface.image) {

        if (tex->texture.surface.image) {
            // The GPU may still be sampling this texture from an earlier draw.
            // Freeing it here lets the allocator hand the block to something
            // else while the GPU is still reading it, which wedges the GPU and
            // hard-freezes the console with no CPU-side error at all. The
            // framebuffer path already guards its frees this way.
            gfx_gx2_defer_image_free(tex->texture.surface.image, tex->texture.surface.imageSize);
            tex->texture.surface.image = nullptr;
        }

        memset(&tex->texture, 0, sizeof(GX2Texture));
        tex->texture.surface.use = GX2_SURFACE_USE_TEXTURE;
        tex->texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        tex->texture.surface.width = width;
        tex->texture.surface.height = height;
        tex->texture.surface.depth = 1;
        tex->texture.surface.mipLevels = 1;
        tex->texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        tex->texture.surface.aa = GX2_AA_MODE1X;
        tex->texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        tex->texture.viewFirstMip = 0;
        tex->texture.viewNumMips = 1;
        tex->texture.viewFirstSlice = 0;
        tex->texture.viewNumSlices = 1;
        tex->texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame texture: GX2CalcSurfaceSizeAndAlignment ...");
        }
        GX2CalcSurfaceSizeAndAlignment(&tex->texture.surface);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame texture: GX2CalcSurfaceSizeAndAlignment complete size=0x{:X}",
                        tex->texture.surface.imageSize);
            SPDLOG_INFO("gfx_gx2: first frame texture: GX2InitTextureRegs ...");
        }
        GX2InitTextureRegs(&tex->texture);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame texture: GX2InitTextureRegs complete; allocation ...");
        }
        WDOG_ENTER(::Ship::WiiU::Watchdog::PH_TEX_ALLOC, nullptr);

        tex->texture.surface.image = memalign(tex->texture.surface.alignment, tex->texture.surface.imageSize);
        WDOG_TEXALLOC(tex->texture.surface.image, tex->texture.surface.imageSize);
        // Log every allocation only for the first 64, then every 256th, and always a failure: two
        // log lines (spdlog + UDP) plus two MEM1 heap walks per texture stretched every scene-load
        // hitch with the 512 pack (~2,600 textures by the first scene, 2026-09-25).
        const bool texAllocFailed = tex->texture.surface.image == nullptr;
        const uint32_t texAllocCount = ::Ship::WiiU::Watchdog::gTexCount;
        const bool logTexAlloc = texAllocFailed || texAllocCount < 64 || (texAllocCount & 255u) == 0;
        const uint32_t mem1Free = logTexAlloc ? gfx_wiiu_mem1_free() : 0;
        if (texAllocFailed) {
            const uint32_t mem1Largest = gfx_wiiu_mem1_largest();
            SPDLOG_ERROR("gfx_gx2: !! texture allocation failed dimensions={}x{} imageSize={} ptr={} mem1Free={} "
                         "mem1Largest={}",
                         width, height, tex->texture.surface.imageSize,
                         static_cast<const void*>(tex->texture.surface.image), mem1Free, mem1Largest);
        }
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame texture: allocation returned ptr={}",
                        static_cast<const void*>(tex->texture.surface.image));
        }
        // Raw channel, not spdlog: the 2026-09-12 capture ends inside this function and
        // contains none of the SPDLOG lines above it, so the spdlog path is lossy exactly
        // where it matters. These numbers are the ones that identify a bogus surface.
        if (logTexAlloc) {
            WDOG_EMIT("GX2TEX: alloc %ux%u pitch=%u imageSize=%u align=%u ptr=0x%08X mem1Free=%u count=%u\n",
                      (unsigned int)width, (unsigned int)height, (unsigned int)tex->texture.surface.pitch,
                      (unsigned int)tex->texture.surface.imageSize, (unsigned int)tex->texture.surface.alignment,
                      (unsigned int)(uintptr_t)tex->texture.surface.image, (unsigned int)mem1Free,
                      (unsigned int)texAllocCount);
        }
        WDOG_LEAVE(::Ship::WiiU::Watchdog::PH_TEX_ALLOC);
    }

    uint8_t* buf = (uint8_t*)tex->texture.surface.image;
    if (!buf) {
        SPDLOG_ERROR("gfx_gx2: texture upload allocation failed");
        return;
    }

    // Temporary: the console hard-freezes between a resource load and the next
    // shader generation, twice at the same 26x16 font glyph. Log the surface
    // geometry for every upload so we can see whether the row writes stay
    // inside imageSize -- an overrun here corrupts the heap silently.
    const uint32_t needed = (height ? ((height - 1) * tex->texture.surface.pitch * 4) + (width * 4) : 0);
    if (needed > tex->texture.surface.imageSize) {
        SPDLOG_ERROR("gfx_gx2: refusing texture upload that would overrun the surface");
        return;
    }

    for (uint32_t y = 0; y < height; ++y) {
        memcpy(buf + (y * tex->texture.surface.pitch * 4), rgba32_buf + (y * width * 4), width * 4);
    }

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame texture: GX2Invalidate ...");
    }
    WDOG_ENTER_FMT(::Ship::WiiU::Watchdog::PH_TEX_UPLOAD, "GX2Invalidate ptr=0x%08X size=%u %ux%u pitch=%u",
                   (unsigned int)(uintptr_t)tex->texture.surface.image,
                   (unsigned int)tex->texture.surface.imageSize, (unsigned int)width, (unsigned int)height,
                   (unsigned int)tex->texture.surface.pitch);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, tex->texture.surface.image, tex->texture.surface.imageSize);
    WDOG_LEAVE(::Ship::WiiU::Watchdog::PH_TEX_UPLOAD);
    ++perf_texture_uploads;
#if WIIU_DIAGNOSTICS
    {
        const uint32_t uploadMicroseconds =
            static_cast<uint32_t>(OSTicksToMicroseconds(OSGetSystemTime() - uploadStart));
        Ship::WiiU::Watchdog::RecordTextureUpload(uploadMicroseconds);
        // Name the textures behind in-play stutters (one ~40 ms upload per slow frame in Hyrule Field).
        if (uploadMicroseconds > 20000) {
            const char* path = WDOG_TEXTURE_DETAIL();
            Ship::WiiU::Watchdog::Emit("TEXSLOW: ms=%u %ux%u path=%s\n", uploadMicroseconds / 1000,
                                       (unsigned int)width, (unsigned int)height, path != nullptr ? path : "?");
        }
    }
#endif
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame texture: GX2Invalidate complete");
    }

    if (current_shader_program && current_shader_program->samplers_location[current_tile] != -1) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame texture: GX2SetPixelTexture ...");
        }
        gfx_gx2_set_pixel_texture(current_tile, current_shader_program->samplers_location[current_tile], &tex->texture);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame texture: GX2SetPixelTexture complete");
        }
    }

    tex->texture_uploaded = true;
    tile_bound_texture[current_tile] = &tex->texture;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame texture upload complete size={}x{}", width, height);
        trace_first_texture_upload = false;
    }
}

static GX2TexClampMode gfx_cm_to_gx2(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GX2_TEX_CLAMP_MODE_CLAMP;
        case G_TX_MIRROR | G_TX_WRAP:
            return GX2_TEX_CLAMP_MODE_MIRROR;
        case G_TX_MIRROR | G_TX_CLAMP:
            return GX2_TEX_CLAMP_MODE_MIRROR_ONCE;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GX2_TEX_CLAMP_MODE_WRAP;
    }

    return GX2_TEX_CLAMP_MODE_WRAP;
}

static void gfx_gx2_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    WDOG_SCOPE_ARGS(::Ship::WiiU::Watchdog::PH_GX2_SET_SAMPLER, "tile=%d linear=%d cms=%u cmt=%u", (uint32_t)tile,
                    linear_filter ? 1u : 0u, (uint32_t)cms, (uint32_t)cmt);
    static bool trace_first_sampler_update = true;
    const bool trace = trace_first_sampler_update;
    struct GX2TextureEntry* tex = current_texture;
    if (!tex) {
        SPDLOG_ERROR("gfx_gx2: sampler update requested without a current texture");
        return;
    }

    current_tile = tile;

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame sampler: GX2InitSampler ...");
    }
    GX2InitSampler(&tex->sampler, GX2_TEX_CLAMP_MODE_CLAMP,
                   (linear_filter && current_filter_mode == FILTER_LINEAR) ? GX2_TEX_XY_FILTER_MODE_LINEAR
                                                                           : GX2_TEX_XY_FILTER_MODE_POINT);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame sampler: GX2InitSampler complete; GX2InitSamplerClamping ...");
    }

    GX2InitSamplerClamping(&tex->sampler, gfx_cm_to_gx2(cms), gfx_cm_to_gx2(cmt), GX2_TEX_CLAMP_MODE_WRAP);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame sampler: GX2InitSamplerClamping complete");
    }

    if (current_shader_program && current_shader_program->samplers_location[tile] != -1) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame sampler: GX2SetPixelSampler ...");
        }
        GX2SetPixelSampler(&tex->sampler, current_shader_program->samplers_location[tile]);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame sampler: GX2SetPixelSampler complete");
        }
    }

    tex->sampler_set = true;
    tile_bound_sampler[tile] = &tex->sampler;
    if (trace) {
        trace_first_sampler_update = false;
    }
}

static void gfx_gx2_set_depth_test_and_mask(bool depth_test, bool z_upd) {
    static bool trace_first_depth_state = true;
    const bool trace = trace_first_depth_state;
    current_depth_test = depth_test || z_upd;
    current_depth_write = z_upd;
    current_depth_compare_function = depth_test ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_ALWAYS;

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetDepthOnlyControl ...");
    }
    GX2SetDepthOnlyControl(current_depth_test, current_depth_write, current_depth_compare_function);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetDepthOnlyControl complete");
        trace_first_depth_state = false;
    }
}

static void gfx_gx2_set_zmode_decal(bool zmode_decal) {
    static bool trace_first_zmode_state = true;
    const bool trace = trace_first_zmode_state;
    current_zmode_decal = zmode_decal;
    if (zmode_decal) {
        // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
        // fighting
        const int n64modeFactor = 120;
        const int noVanishFactor = 100;
        float SSDB = -2.0f;
        switch (CVarGetInteger("gDirtPathFix", 0)) {
            // scaled z-fighting (N64 mode like)
            case 1:
                if (current_framebuffer) {
                    SSDB = -1.0f * (float)current_framebuffer->color_buffer.surface.height / n64modeFactor;
                }
                break;
            // no vanishing paths
            case 2:
                if (current_framebuffer) {
                    SSDB = -1.0f * (float)current_framebuffer->color_buffer.surface.height / noVanishFactor;
                }
                break;
            // disabled
            case 0:
            default:
                SSDB = -2.0f;
        }

        current_SSDB = SSDB;
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset(decal) ...");
        }
        GX2SetPolygonOffset(SSDB, SSDB, SSDB, SSDB, 0.0f);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset(decal) complete; GX2SetPolygonControl ...");
        }
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, TRUE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, TRUE, TRUE, FALSE);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonControl(decal) complete");
            trace_first_zmode_state = false;
        }
    } else {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset ...");
        }
        GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset complete; GX2SetPolygonControl ...");
        }
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonControl complete");
            trace_first_zmode_state = false;
        }
    }
}

static void gfx_gx2_set_viewport(int x, int y, int width, int height) {
    static bool trace_first_viewport_state = true;
    const bool trace = trace_first_viewport_state;
    uint32_t buffer_height = current_framebuffer->color_buffer.surface.height;

    current_viewport_x = x;
    current_viewport_y = buffer_height - y - height;
    current_viewport_width = width;
    current_viewport_height = height;

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetViewport ... x={} y={} width={} height={}", current_viewport_x,
                    current_viewport_y, current_viewport_width, current_viewport_height);
    }
    GX2SetViewport(current_viewport_x, current_viewport_y, current_viewport_width, current_viewport_height, 0.0f, 1.0f);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetViewport complete");
        trace_first_viewport_state = false;
    }
}

static void gfx_gx2_set_scissor(int x, int y, int width, int height) {
    static bool trace_first_scissor_state = true;
    const bool trace = trace_first_scissor_state;
    uint32_t buffer_height = current_framebuffer->color_buffer.surface.height;
    uint32_t buffer_width = current_framebuffer->color_buffer.surface.width;

    current_scissor_x = std::min((uint32_t)width, (uint32_t)x);
    current_scissor_y = std::min((uint32_t)height, buffer_height - y - height);
    current_scissor_width = std::min((uint32_t)width, buffer_width);
    current_scissor_height = std::min((uint32_t)height, buffer_height);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetScissor ... x={} y={} width={} height={}", current_scissor_x,
                    current_scissor_y, current_scissor_width, current_scissor_height);
    }
    GX2SetScissor(current_scissor_x, current_scissor_y, current_scissor_width, current_scissor_height);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetScissor complete");
        trace_first_scissor_state = false;
    }
}

static void gfx_gx2_set_use_alpha(bool use_alpha) {
    static bool trace_first_alpha_state = true;
    const bool trace = trace_first_alpha_state;
    current_use_alpha = use_alpha;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetColorControl ... use_alpha={}", use_alpha);
    }
    GX2SetColorControl(GX2_LOGIC_OP_COPY, use_alpha ? 0xff : 0, FALSE, TRUE);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetColorControl complete");
        trace_first_alpha_state = false;
    }
}

// Copy a batch into the draw arena. dst is GX2_VERTEX_BUFFER_ALIGNMENT-aligned and the arena end is too, so
// every 32-byte line up to ALIGN(len, 32) lies inside this draw's reservation: zero-allocate each line with
// dcbz before writing it, so the stores do not first read a line of cold arena memory from RAM (which is
// what made OSBlockMove cost 2-4 ms/frame for ~1-2 MB). Off switch: gWiiU.ArenaDcbzCopy 0.
static void gfx_gx2_copy_to_arena(uint8_t* dst, const float* src, size_t len) {
    const uint32_t* s = (const uint32_t*)src;
    const size_t words = len / sizeof(uint32_t);
    size_t w = 0;
    for (uint8_t* line = dst; line < dst + len; line += 32) {
        __asm__ volatile("dcbz 0,%0" : : "r"(line) : "memory");
        uint32_t* d = (uint32_t*)line;
        const size_t end = std::min(words, w + 8);
        for (size_t i = 0; w < end; i++, w++) {
            d[i] = s[w];
        }
    }
}

static void gfx_gx2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    WDOG_SCOPE(::Ship::WiiU::Watchdog::PH_GX2_DRAW_TRIANGLES, "draw triangles");
    const bool trace = gfx_gx2_trace_first_draw;
    if (!current_shader_program) {
        if (trace) {
            SPDLOG_ERROR("gfx_gx2: first draw skipped: no current shader");
            gfx_gx2_trace_first_draw = false;
        }
        return;
    }

    if (!draw_buffer || !draw_ptr) {
        if (trace) {
            SPDLOG_ERROR("gfx_gx2: first draw skipped: draw buffer is not initialized");
            gfx_gx2_trace_first_draw = false;
        }
        return;
    }

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw begin vertices={} triangles={} bytes={}", buf_vbo_len,
                    buf_vbo_num_tris, sizeof(float) * buf_vbo_len);
    }

    size_t vbo_len = sizeof(float) * buf_vbo_len;

    if (draw_ptr + vbo_len >= draw_buffer + DRAW_BUFFER_SIZE) {
        SPDLOG_WARN("gfx_gx2: draw-buffer arena exhausted; serializing GPU for fallback");
        gfx_gx2_draw_done("draw-buffer arena exhaustion");
        draw_ptr = draw_buffer;
    }

    float* new_vbo = (float*)draw_ptr;
    draw_ptr += ALIGN(vbo_len, GX2_VERTEX_BUFFER_ALIGNMENT);
    const uint32_t draw_buffer_used = static_cast<uint32_t>(draw_ptr - draw_buffer);
    if (draw_buffer_used > draw_buffer_frame_high_water) {
        draw_buffer_frame_high_water = draw_buffer_used;
    }

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: OSBlockMove ...");
    }
    static uint32_t dcbzCountdown = 0;
    static bool dcbzCopy = true;
    if (dcbzCountdown-- == 0) {
        dcbzCopy = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger("gWiiU.ArenaDcbzCopy", 1) != 0;
        dcbzCountdown = 1024;
    }
    uint64_t tick = OSGetSystemTick();
    if (dcbzCopy) {
        gfx_gx2_copy_to_arena((uint8_t*)new_vbo, buf_vbo, vbo_len);
    } else {
        OSBlockMove(new_vbo, buf_vbo, vbo_len, FALSE);
    }
    perf_osblockmove_us += OSTicksToMicroseconds(OSGetSystemTick() - tick);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: OSBlockMove complete; GX2Invalidate ...");
    }
    tick = OSGetSystemTick();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, new_vbo, vbo_len);
    perf_gx2_invalidate_us += OSTicksToMicroseconds(OSGetSystemTick() - tick);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: GX2Invalidate complete; GX2SetAttribBuffer ...");
    }
    tick = OSGetSystemTick();
    GX2SetAttribBuffer(0, vbo_len, current_shader_program->group.stride, new_vbo);
    perf_gx2_set_attrib_buffer_us += OSTicksToMicroseconds(OSGetSystemTick() - tick);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: GX2SetAttribBuffer complete; GX2DrawEx ...");
    }
    tick = OSGetSystemTick();
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3 * buf_vbo_num_tris, 0, 1);
    perf_gx2_draw_ex_us += OSTicksToMicroseconds(OSGetSystemTick() - tick);
    ++perf_gx2_draws;
    perf_gx2_triangles += buf_vbo_num_tris;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: GX2DrawEx complete");
        gfx_gx2_trace_first_draw = false;
    }
}

static void gfx_gx2_init(void) {
    WDOG_HEAPMARK("before gfx_gx2_init");
    SPDLOG_INFO("gfx_gx2_init: framebuffer setup begin {}x{}", WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);
    // Init the default framebuffer
    gfx_gx2_init_framebuffer(&main_framebuffer, WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);

    SPDLOG_INFO("gfx_gx2_init: GX2CalcSurfaceSizeAndAlignment(color) ...");
    GX2CalcSurfaceSizeAndAlignment(&main_framebuffer.color_buffer.surface);
    SPDLOG_INFO("gfx_gx2_init: GX2CalcSurfaceSizeAndAlignment(color) complete size=0x{:X} alignment=0x{:X}",
                main_framebuffer.color_buffer.surface.imageSize, main_framebuffer.color_buffer.surface.alignment);
    SPDLOG_INFO("gfx_gx2_init: GX2InitColorBufferRegs ...");
    GX2InitColorBufferRegs(&main_framebuffer.color_buffer);
    SPDLOG_INFO("gfx_gx2_init: GX2InitColorBufferRegs complete");

    SPDLOG_INFO("gfx_gx2_init: main color-buffer allocation ... size=0x{:X}",
                main_framebuffer.color_buffer.surface.imageSize);
    main_framebuffer.color_buffer.surface.image = gfx_wiiu_alloc_mem1(main_framebuffer.color_buffer.surface.imageSize,
                                                                      main_framebuffer.color_buffer.surface.alignment);
    SPDLOG_INFO("gfx_gx2_init: main color-buffer allocation returned ptr={}",
                main_framebuffer.color_buffer.surface.image);
    if (!main_framebuffer.color_buffer.surface.image) {
        SPDLOG_ERROR("gfx_gx2: failed to allocate main color buffer");
        return;
    }

    SPDLOG_INFO("gfx_gx2_init: GX2CalcSurfaceSizeAndAlignment(depth) ...");
    GX2CalcSurfaceSizeAndAlignment(&main_framebuffer.depth_buffer.surface);
    SPDLOG_INFO("gfx_gx2_init: GX2CalcSurfaceSizeAndAlignment(depth) complete size=0x{:X} alignment=0x{:X}",
                main_framebuffer.depth_buffer.surface.imageSize, main_framebuffer.depth_buffer.surface.alignment);
    SPDLOG_INFO("gfx_gx2_init: GX2InitDepthBufferRegs ...");
    GX2InitDepthBufferRegs(&main_framebuffer.depth_buffer);
    SPDLOG_INFO("gfx_gx2_init: GX2InitDepthBufferRegs complete");

    SPDLOG_INFO("gfx_gx2_init: main depth-buffer allocation ... size=0x{:X}",
                main_framebuffer.depth_buffer.surface.imageSize);
    main_framebuffer.depth_buffer.surface.image = gfx_wiiu_alloc_mem1(main_framebuffer.depth_buffer.surface.imageSize,
                                                                      main_framebuffer.depth_buffer.surface.alignment);
    SPDLOG_INFO("gfx_gx2_init: main depth-buffer allocation returned ptr={}",
                main_framebuffer.depth_buffer.surface.image);
    if (!main_framebuffer.depth_buffer.surface.image) {
        SPDLOG_ERROR("gfx_gx2: failed to allocate main depth buffer");
        return;
    }

    main_framebuffer.imtex.Texture = &main_framebuffer.texture;
    main_framebuffer.imtex.Sampler = &main_framebuffer.sampler;

    // Each frame slot owns its GPU destination so a copy can stay in flight
    // while the CPU records the following frame.
    for (DrawBufferSlot& slot : draw_buffer_slots) {
        if (!gfx_gx2_resize_depth_readback(slot, 32)) {
            return;
        }
    }

    SPDLOG_INFO("gfx_gx2_init: GX2SetColorBuffer ...");
    GX2SetColorBuffer(&main_framebuffer.color_buffer, GX2_RENDER_TARGET_0);
    SPDLOG_INFO("gfx_gx2_init: GX2SetColorBuffer complete; GX2SetDepthBuffer ...");
    GX2SetDepthBuffer(&main_framebuffer.depth_buffer);
    SPDLOG_INFO("gfx_gx2_init: GX2SetDepthBuffer complete");

    current_framebuffer = &main_framebuffer;

    // Allocate two frame arenas so the CPU can prepare the next frame while
    // the GPU consumes the previous one. The second allocation is optional:
    // retain the proven single-arena/DrawDone path if MEM2 cannot provide it.
    SPDLOG_INFO("gfx_gx2_init: draw-buffer allocation ... size=0x{:X} per slot", DRAW_BUFFER_SIZE);
    draw_buffer_slots[0].buffer = (uint8_t*)memalign(GX2_VERTEX_BUFFER_ALIGNMENT, DRAW_BUFFER_SIZE);
    SPDLOG_INFO("gfx_gx2_init: draw-buffer slot 0 returned ptr={}",
                static_cast<const void*>(draw_buffer_slots[0].buffer));
    if (!draw_buffer_slots[0].buffer) {
        SPDLOG_ERROR("gfx_gx2: failed to allocate draw buffer");
        return;
    }

    draw_buffer_slots[1].buffer = (uint8_t*)memalign(GX2_VERTEX_BUFFER_ALIGNMENT, DRAW_BUFFER_SIZE);
    SPDLOG_INFO("gfx_gx2_init: draw-buffer slot 1 returned ptr={}",
                static_cast<const void*>(draw_buffer_slots[1].buffer));
    if (!draw_buffer_slots[1].buffer) {
        SPDLOG_ERROR("gfx_gx2: second draw-buffer slot allocation failed; using single arena with per-frame GX2DrawDone");
        draw_buffer_double_buffered = false;
    } else {
        draw_buffer_double_buffered = true;
    }
    draw_buffer_slot_index = 0;
    draw_buffer = draw_buffer_slots[0].buffer;
    draw_ptr = draw_buffer;

    SPDLOG_INFO("gfx_gx2_init: GX2SetRasterizerClipControl ...");
    GX2SetRasterizerClipControl(TRUE, FALSE);
    SPDLOG_INFO("gfx_gx2_init: GX2SetRasterizerClipControl complete; GX2SetBlendControl ...");

    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);
    SPDLOG_INFO("gfx_gx2_init: GX2SetBlendControl complete; framebuffer setup complete");
    WDOG_HEAPMARK("after gfx_gx2_init");
}

void gfx_gx2_shutdown(void) {
    // Idempotent: the teardown path runs TWICE. Fast3dWindow::Close drives the window
    // manager's teardown and the GfxRenderingAPIGX2 destructor calls this again, so the
    // second pass used to walk freed GX2 state and fault with an ISI to address 0,
    // hanging the console on every exit - AFTER a first pass that completed cleanly.
    static bool sShutdown = false;
    if (sShutdown) {
        Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown already done - ignoring\n");
        return;
    }
    sShutdown = true;

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown enter\n");

    if (has_foreground) {
        gfx_gx2_draw_done("shutdown");
        gfx_gx2_release_pending_image_frees(true);

        for (DrawBufferSlot& slot : draw_buffer_slots) {
            if (slot.depth_read_buffer.surface.image) {
                gfx_wiiu_free_mem1(slot.depth_read_buffer.surface.image);
                slot.depth_read_buffer.surface.image = nullptr;
            }
        }

        if (main_framebuffer.color_buffer.surface.image) {
            gfx_wiiu_free_mem1(main_framebuffer.color_buffer.surface.image);
            main_framebuffer.color_buffer.surface.image = nullptr;
        }

        if (main_framebuffer.depth_buffer.surface.image) {
            gfx_wiiu_free_mem1(main_framebuffer.depth_buffer.surface.image);
            main_framebuffer.depth_buffer.surface.image = nullptr;
        }
    }

    for (DrawBufferSlot& slot : draw_buffer_slots) {
        if (slot.buffer) {
            free(slot.buffer);
            slot.buffer = nullptr;
        }
        slot.submitted_timestamp = 0;
        slot.gpu_timing_pending = false;
        slot.depth_read_requests.clear();
        slot.depth_readback_pending = false;
    }
    depth_readback_cache.clear();
    draw_buffer_double_buffered = false;
    draw_buffer_slot_index = 0;
    draw_buffer = nullptr;
    draw_ptr = nullptr;

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown exit\n");
}

static void gfx_gx2_on_resize(void) {
}

static void gfx_gx2_start_frame(void) {
    const bool trace = gfx_gx2_trace_first_frame;
    frame_start_time = OSGetSystemTime();
    gfx_gx2_prepare_draw_buffer_slot();
    draw_buffer_frame_high_water = 0;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state setup begin");
    }
    // Restore state since ImGui modified it when rendering
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetViewport (restore) ...");
    }
    GX2SetViewport(current_viewport_x, current_viewport_y, current_viewport_width, current_viewport_height, 0.0f, 1.0f);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetViewport (restore) complete; GX2SetScissor ...");
    }
    GX2SetScissor(current_scissor_x, current_scissor_y, current_scissor_width, current_scissor_height);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetScissor (restore) complete; GX2SetColorControl ...");
    }
    GX2SetColorControl(GX2_LOGIC_OP_COPY, current_use_alpha ? 0xff : 0, FALSE, TRUE);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetColorControl complete; GX2SetBlendControl ...");
    }
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetBlendControl complete; GX2SetDepthOnlyControl ...");
    }
    GX2SetDepthOnlyControl(current_depth_test, current_depth_write, current_depth_compare_function);

    if (current_zmode_decal) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset (restore decal) ...");
        }
        GX2SetPolygonOffset(current_SSDB, current_SSDB, current_SSDB, current_SSDB, 0.0f);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset complete; GX2SetPolygonControl ...");
        }
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, TRUE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, TRUE, TRUE, FALSE);
    } else {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset (restore) ...");
        }
        GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonOffset complete; GX2SetPolygonControl ...");
        }
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
    }

    frame_count++;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame state: GX2SetPolygonControl complete; state setup complete");
    }
}

static void gfx_gx2_enqueue_depth_readback(DrawBufferSlot& slot) {
    if (slot.depth_read_requests.empty()) {
        return;
    }

    if (slot.depth_read_requests.size() > UINT32_MAX) {
        SPDLOG_ERROR("gfx_gx2: depth readback request count exceeds GX2 surface capacity");
        slot.depth_read_requests.clear();
        return;
    }

    const uint32_t request_count = static_cast<uint32_t>(slot.depth_read_requests.size());
    if (!gfx_gx2_resize_depth_readback(slot, request_count)) {
        slot.depth_read_requests.clear();
        return;
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER,
                  slot.depth_read_buffer.surface.image, slot.depth_read_buffer.surface.imageSize);

    // GX2CopySurfaceEx accepts at most 25 source rectangles per call. Multiple
    // calls target disjoint pixels in this slot's linear destination.
    for (uint32_t first = 0; first < request_count; first += 25) {
        const uint32_t count = std::min<uint32_t>(25, request_count - first);
        GX2Rect source_rects[25];
        GX2Point destination_points[25];

        for (uint32_t i = 0; i < count; ++i) {
            const DepthReadbackRequest& request = slot.depth_read_requests[first + i];
            const GX2Surface& depth_surface = request.framebuffer->depth_buffer.surface;
            const int32_t x = static_cast<int32_t>(
                std::clamp(request.coordinate.first, 0.0f, static_cast<float>(depth_surface.width - 1)));
            const int32_t y = static_cast<int32_t>(
                std::clamp(request.coordinate.second, 0.0f, static_cast<float>(depth_surface.height - 1)));
            source_rects[i] = GX2Rect{ x, static_cast<int32_t>(depth_surface.height) - y, x + 1,
                                       static_cast<int32_t>(depth_surface.height) - y + 1 };
            destination_points[i] = GX2Point{ static_cast<int32_t>(first + i), 0 };
        }

        // A batch can contain requests for different framebuffers, while one
        // GX2CopySurfaceEx call has only one source. Keep contiguous requests
        // grouped by source by issuing individual copies where necessary.
        uint32_t group_start = 0;
        while (group_start < count) {
            struct Framebuffer* source = slot.depth_read_requests[first + group_start].framebuffer;
            uint32_t group_count = 1;
            while (group_start + group_count < count &&
                   slot.depth_read_requests[first + group_start + group_count].framebuffer == source) {
                ++group_count;
            }
            GX2CopySurfaceEx(&source->depth_buffer.surface, 0, 0, &slot.depth_read_buffer.surface, 0, 0,
                             group_count, &source_rects[group_start], &destination_points[group_start]);
            group_start += group_count;
        }
    }

    // CopySurfaceEx changes the GX2 context state. Restore the render state
    // without waiting; the queued copies remain part of this frame.
    gfx_wiiu_set_context_state();

    slot.depth_readback_pending = true;
}

// One performance line a minute in every build flavour, release included: average fps over the minute,
// the worst 5-second window, CPU ms per frame and the scene, so a player's log file (logs/) says how the
// game ran. One SD write a minute. The game may define wiiu_perf_scene() to name the scene.
extern "C" int wiiu_perf_scene(void) __attribute__((weak));

static void gfx_gx2_perf_tick(uint32_t cpu_us) {
    static OSTime minuteStart = 0, windowStart = 0;
    static uint32_t minuteFrames = 0, windowFrames = 0;
    static uint64_t cpuSum = 0;
    static uint64_t vsyncWaitUs = 0;
    static uint32_t droppedFrames = 0;
    static uint64_t depthRequests = 0;
    static float worst = 0.0f;
    const OSTime now = OSGetSystemTime();
    if (minuteStart == 0) {
        uint64_t ignoredVertices = 0;
        uint64_t ignoredFlushCounts[PERF_FLUSH_COUNT] = {};
        FastGetAndResetInterpreterPerf(&ignoredVertices, ignoredFlushCounts);
        uint64_t ignoredCull[2] = {};
        FastGetAndResetCullDlPerf(&ignoredCull[0], &ignoredCull[1]);
        FastGetAndResetSubDlPerf(&ignoredCull[0], &ignoredCull[1]);
        uint64_t ignoredBatch[3] = {};
        FastGetAndResetVtxBatchPerf(&ignoredBatch[0], &ignoredBatch[1], &ignoredBatch[2]);
        FastGetAndResetFalseCull();
        FastGetAndResetStaleTrisSubDl();
        minuteStart = windowStart = now;
        minuteFrames = windowFrames = 0;
        cpuSum = 0;
        vsyncWaitUs = 0;
        droppedFrames = 0;
        depthRequests = 0;
        perf_gpu_us = 0;
        perf_gpu_frames = 0;
        perf_gpu_max_us = 0;
        perf_slot_wait_us = 0;
        perf_texture_uploads = 0;
        perf_gx2_draws = 0;
        perf_gx2_triangles = 0;
        perf_osblockmove_us = 0;
        perf_gx2_invalidate_us = 0;
        perf_gx2_set_attrib_buffer_us = 0;
        perf_gx2_draw_ex_us = 0;
        worst = 0.0f;
        return;
    }
    minuteFrames++;
    windowFrames++;
    cpuSum += cpu_us;
    vsyncWaitUs += gfx_wiiu_perf_vsync_wait_us;
    gfx_wiiu_perf_vsync_wait_us = 0;
    droppedFrames += gfx_wiiu_perf_dropped_frames;
    gfx_wiiu_perf_dropped_frames = 0;
    depthRequests += draw_buffer_slots[draw_buffer_slot_index].depth_read_requests.size();
    const float windowSec = OSTicksToMicroseconds(now - windowStart) / 1e6f;
    if (windowSec >= 5.0f) {
        const float fps = windowFrames / windowSec;
        if (worst == 0.0f || fps < worst) {
            worst = fps;
        }
        windowStart = now;
        windowFrames = 0;
    }
    const float minuteSec = OSTicksToMicroseconds(now - minuteStart) / 1e6f;
    if (minuteSec >= 60.0f) {
        const float frames = minuteFrames ? static_cast<float>(minuteFrames) : 1.0f;
        const float cpuMs = cpuSum / 1000.0f / frames;
        const float vsyncMs = vsyncWaitUs / 1000.0f / frames;
        const float slotMs = perf_slot_wait_us / 1000.0f / frames;
        const float intervalMs = minuteSec * 1000.0f / frames;
        const float otherMs = intervalMs - cpuMs - vsyncMs - slotMs;
        const float gpuMs = perf_gpu_frames ? perf_gpu_us / 1000.0f / perf_gpu_frames : 0.0f;
        const float depthPerFrame = depthRequests / frames;
        uint64_t gfxSpVertexCount = 0;
        uint64_t flushCounts[PERF_FLUSH_COUNT] = {};
        FastGetAndResetInterpreterPerf(&gfxSpVertexCount, flushCounts);
        uint64_t cullDlTested = 0;
        uint64_t cullDlRejected = 0;
        FastGetAndResetCullDlPerf(&cullDlTested, &cullDlRejected);
        uint64_t subDlTested = 0;
        uint64_t subDlCulled = 0;
        FastGetAndResetSubDlPerf(&subDlTested, &subDlCulled);
        uint64_t batchTested = 0;
        uint64_t batchCulled = 0;
        uint64_t staleTris = 0;
        FastGetAndResetVtxBatchPerf(&batchTested, &batchCulled, &staleTris);
        const uint64_t falseCull = FastGetAndResetFalseCull();
        const uint64_t staleTrisSubDl = FastGetAndResetStaleTrisSubDl();
        uint32_t roomTested = 0;
        uint32_t roomCulled = 0;
        if (&gWiiURoomEntriesTested != nullptr && &gWiiURoomEntriesSideCulled != nullptr) {
            roomTested = gWiiURoomEntriesTested;
            roomCulled = gWiiURoomEntriesSideCulled;
            gWiiURoomEntriesTested = 0;
            gWiiURoomEntriesSideCulled = 0;
        }
        const struct mallinfo heapInfo = mallinfo();
        uint32_t bigFree = 0;
        uint32_t bigLargest = 0;
        wiiu_get_perf_big_heap(&bigFree, &bigLargest);
        SPDLOG_INFO("PERF: {:.1f} fps avg over {:.0f} s, worst 5 s {:.1f} fps, cpu {:.1f} ms/frame, gpu {:.1f} ms/frame max {:.1f} ms, vsync {:.1f} ms/frame, dropped {}, slot {:.1f} ms/frame, other {:.1f} ms/frame, texUploads {}, texCache {} freeTexIds {} depthCache {} depthReq {:.2f}/frame, GX2Draws {:.2f}/frame triangles {:.2f}/frame GfxSpVertex {:.2f}/frame cullDL {:.1f} tested {:.1f} rejected/frame, rooms {:.1f} tested {:.1f} sideCulled/frame, subDL {:.1f} tested {:.1f} culled/frame, vtxBatch {:.1f} tested {:.1f} culled/frame staleTris {} (subDL {}) falseCull {}, Flush texture {:.2f}/frame sampler {:.2f}/frame shader {:.2f}/frame depthZmode {:.2f}/frame viewportScissor {:.2f}/frame alpha {:.2f}/frame triCap {:.2f}/frame explicit {:.2f}/frame, OSBlockMove {:.1f} us/frame GX2Invalidate {:.1f} us/frame GX2SetAttribBuffer {:.1f} us/frame GX2DrawEx {:.1f} us/frame, shaderPool {} resourceCache {} heapUsed {} heapArena {} bigFree {} bigLargest {} scene {:#x}",
                    minuteFrames / minuteSec, minuteSec, worst, cpuMs, gpuMs, perf_gpu_max_us / 1000.0f,
                    vsyncMs, droppedFrames, slotMs, otherMs, perf_texture_uploads,
                    Ship::WiiU::Watchdog::gTextureCacheSize, Ship::WiiU::Watchdog::gFreeTextureIdsSize,
                    depth_readback_cache.size(), depthPerFrame, perf_gx2_draws / frames, perf_gx2_triangles / frames,
                    gfxSpVertexCount / frames, cullDlTested / frames, cullDlRejected / frames, roomTested / frames, roomCulled / frames, subDlTested / frames, subDlCulled / frames, batchTested / frames, batchCulled / frames, staleTris, staleTrisSubDl, falseCull, flushCounts[PERF_FLUSH_TEXTURE] / frames,
                    flushCounts[PERF_FLUSH_SAMPLER] / frames, flushCounts[PERF_FLUSH_SHADER] / frames,
                    flushCounts[PERF_FLUSH_DEPTH_ZMODE] / frames, flushCounts[PERF_FLUSH_VIEWPORT_SCISSOR] / frames,
                    flushCounts[PERF_FLUSH_ALPHA] / frames, flushCounts[PERF_FLUSH_TRIANGLE_CAP] / frames,
                    flushCounts[PERF_FLUSH_EXPLICIT] / frames, perf_osblockmove_us / frames,
                    perf_gx2_invalidate_us / frames, perf_gx2_set_attrib_buffer_us / frames, perf_gx2_draw_ex_us / frames,
                    Ship::WiiU::Watchdog::gShaderProgramPoolSize,
                    Ship::WiiU::Watchdog::gResourceCacheSize, (uint32_t)heapInfo.uordblks,
                    (uint32_t)heapInfo.arena, bigFree, bigLargest, wiiu_perf_scene ? wiiu_perf_scene() : -1);
        minuteStart = windowStart = now;
        minuteFrames = windowFrames = 0;
        cpuSum = 0;
        vsyncWaitUs = 0;
        droppedFrames = 0;
        depthRequests = 0;
        perf_gpu_us = 0;
        perf_gpu_frames = 0;
        perf_gpu_max_us = 0;
        perf_slot_wait_us = 0;
        perf_texture_uploads = 0;
        perf_gx2_draws = 0;
        perf_gx2_triangles = 0;
        perf_osblockmove_us = 0;
        perf_gx2_invalidate_us = 0;
        perf_gx2_set_attrib_buffer_us = 0;
        perf_gx2_draw_ex_us = 0;
        worst = 0.0f;
    }
}

static void gfx_gx2_end_frame(void) {
    const bool trace = gfx_gx2_trace_first_frame;
    DrawBufferSlot& slot = draw_buffer_slots[draw_buffer_slot_index];
    Ship::WiiU::Watchdog::gDrawBufferHighWaterBytes = draw_buffer_frame_high_water;

    // Queue depth copies into this frame's slot before the flush. They retire
    // under the same timestamp as the draw arena and timing samples.
    gfx_gx2_enqueue_depth_readback(slot);

    // Close the GPU-side timing interval and submit the command buffer. The
    // submitted timestamp is the fence for both the draw arena and its timing
    // samples; neither is read until this timestamp has retired.
    GX2SampleBottomGPUCycle(&gfx_gx2_gpu_samples(slot)[1]);
    GX2Flush();
    const OSTime submit_time = OSGetSystemTime();
    slot.cpu_microseconds = gfx_gx2_elapsed_microseconds(frame_start_time, submit_time);
    slot.submitted_timestamp = GX2GetLastSubmittedTimeStamp();
    gfx_gx2_stamp_pending_image_frees(slot.submitted_timestamp);
    gfx_gx2_perf_tick(slot.cpu_microseconds);

    if (draw_buffer_double_buffered) {
        draw_buffer_slot_index = (draw_buffer_slot_index + 1) % 2;
        draw_buffer = draw_buffer_slots[draw_buffer_slot_index].buffer;
        draw_ptr = draw_buffer;
    } else {
        // Allocation failure at init deliberately preserves the old behavior:
        // the single arena is serialized once per frame.
        gfx_gx2_draw_done("GX2DrawDone");
        gfx_gx2_release_pending_image_frees(true);
        gfx_gx2_collect_gpu_timing(slot);
        gfx_gx2_collect_depth_readback(slot);
        slot.submitted_timestamp = 0;
        draw_ptr = draw_buffer;
    }

    // gfx_wiiu_start_frame lets a frame begin with the previous swap still pending, so
    // without this the copy below can overwrite the scan buffer that swap is about to
    // show (GamePad tearing). The draws are already flushed, so the GPU keeps working
    // while we wait. Off switch for A/B runs: gWiiU.CopyAfterFlip 0.
    if (Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger("gWiiU.CopyAfterFlip", 1)) {
        uint32_t swap_count, flip_count;
        OSTime last_flip, last_vsync;
        for (int waits = 0; waits < 10; waits++) {
            GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);
            if (swap_count == flip_count) {
                break;
            }
            GX2WaitForVsync();
        }
    }

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: GX2CopyColorBufferToScanBuffer(TV) ...");
    }
    GX2CopyColorBufferToScanBuffer(&main_framebuffer.color_buffer, GX2_SCAN_TARGET_TV);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: GX2CopyColorBufferToScanBuffer(TV) complete; DRC ...");
    }
    GX2CopyColorBufferToScanBuffer(&main_framebuffer.color_buffer, GX2_SCAN_TARGET_DRC);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: GX2CopyColorBufferToScanBuffer(DRC) complete");
        gfx_gx2_trace_first_frame = false;
    }
#if WIIU_DIAGNOSTICS
    Ship::WiiU::Watchdog::ReportHitchFrame(gfx_gx2_elapsed_microseconds(frame_start_time, OSGetSystemTime()),
                                           wiiu_perf_scene ? wiiu_perf_scene() : -1);
#endif
}

static void gfx_gx2_finish_render(void) {
}

static int gfx_gx2_create_framebuffer(void) {
    SPDLOG_INFO("gfx_gx2: CreateFramebuffer begin");
    struct Framebuffer* buffer = (struct Framebuffer*)calloc(1, sizeof(struct Framebuffer));
    if (!buffer) {
        SPDLOG_ERROR("gfx_gx2: framebuffer allocation failed");
        return 0;
    }

    SPDLOG_INFO("gfx_gx2: CreateFramebuffer: GX2InitSampler ...");
    GX2InitSampler(&buffer->sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    SPDLOG_INFO("gfx_gx2: CreateFramebuffer: GX2InitSampler complete");

    buffer->imtex.Texture = &buffer->texture;
    buffer->imtex.Sampler = &buffer->sampler;

    // some more 32-bit shenanigans :D
    SPDLOG_INFO("gfx_gx2: CreateFramebuffer complete ptr={}", static_cast<const void*>(buffer));
    const int framebuffer_id = static_cast<int>(reinterpret_cast<uintptr_t>(buffer));
    framebuffer_registry[framebuffer_id] = buffer;
    return framebuffer_id;
}

static struct Framebuffer* gfx_gx2_lookup_framebuffer(int framebuffer_id) {
    const auto framebuffer = framebuffer_registry.find(framebuffer_id);
    return framebuffer == framebuffer_registry.end() ? nullptr : framebuffer->second;
}

static void gfx_gx2_update_framebuffer_parameters(int fb, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                  bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                  bool can_extract_depth) {
    // NO logging here: this runs per frame, and every spdlog emit on this port is a
    // BLOCKING sendto over UDP. Two lines a frame was ~45 datagrams/sec of pure stall.
    // (The Banjo port lost 60 fps -> 3.7 fps to exactly this pattern.)
    struct Framebuffer* buffer = (struct Framebuffer*)fb;

    // we don't support updating the main buffer (fb 0)
    if (!buffer) {
        return;
    }

    if (buffer->texture.surface.width == width && buffer->texture.surface.height == height) {
        return;
    }

    // make sure the GPU no longer writes to the buffer
    gfx_gx2_draw_done("framebuffer resize");

    if (buffer->texture.surface.image) {
        if (buffer->colorBufferMem1) {
            gfx_wiiu_free_mem1(buffer->texture.surface.image);
        } else {
            free(buffer->texture.surface.image);
        }
        buffer->texture.surface.image = nullptr;
    }

    if (buffer->depth_buffer.surface.image) {
        if (buffer->depthBufferMem1) {
            gfx_wiiu_free_mem1(buffer->depth_buffer.surface.image);
        } else {
            free(buffer->depth_buffer.surface.image);
        }
        buffer->depth_buffer.surface.image = nullptr;
    }

    gfx_gx2_init_framebuffer(buffer, width, height);

    GX2CalcSurfaceSizeAndAlignment(&buffer->depth_buffer.surface);
    GX2InitDepthBufferRegs(&buffer->depth_buffer);

    buffer->depth_buffer.surface.image =
        gfx_wiiu_alloc_mem1(buffer->depth_buffer.surface.imageSize, buffer->depth_buffer.surface.alignment);
    // fall back to mem2
    if (!buffer->depth_buffer.surface.image) {
        buffer->depth_buffer.surface.image =
            memalign(buffer->depth_buffer.surface.alignment, buffer->depth_buffer.surface.imageSize);
        if (!buffer->depth_buffer.surface.image) {
            SPDLOG_ERROR("gfx_gx2: !! depth-buffer MEM2 fallback allocation FAILED size={}",
                         buffer->depth_buffer.surface.imageSize);
        }
        buffer->depthBufferMem1 = false;
    } else {
        buffer->depthBufferMem1 = true;
    }
    if (!buffer->depth_buffer.surface.image) {
        SPDLOG_ERROR("gfx_gx2: framebuffer depth-buffer allocation failed");
        return;
    }

    GX2CalcSurfaceSizeAndAlignment(&buffer->color_buffer.surface);
    GX2InitColorBufferRegs(&buffer->color_buffer);

    memset(&buffer->texture, 0, sizeof(GX2Texture));
    buffer->texture.surface.use = GX2_SURFACE_USE_TEXTURE;
    buffer->texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    buffer->texture.surface.width = width;
    buffer->texture.surface.height = height;
    buffer->texture.surface.depth = 1;
    buffer->texture.surface.mipLevels = 1;
    buffer->texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    buffer->texture.surface.aa = GX2_AA_MODE1X;
    buffer->texture.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    buffer->texture.viewFirstMip = 0;
    buffer->texture.viewNumMips = 1;
    buffer->texture.viewFirstSlice = 0;
    buffer->texture.viewNumSlices = 1;
    buffer->texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    GX2CalcSurfaceSizeAndAlignment(&buffer->texture.surface);
    GX2InitTextureRegs(&buffer->texture);

    // the texture and color buffer share a buffer
    if (buffer->color_buffer.surface.imageSize != buffer->texture.surface.imageSize) {
        SPDLOG_ERROR("gfx_gx2: framebuffer color/texture sizes differ");
        return;
    }

    buffer->texture.surface.image =
        gfx_wiiu_alloc_mem1(buffer->texture.surface.imageSize, buffer->texture.surface.alignment);
    // fall back to mem2
    if (!buffer->texture.surface.image) {
        buffer->texture.surface.image = memalign(buffer->texture.surface.alignment, buffer->texture.surface.imageSize);
        if (!buffer->texture.surface.image) {
            SPDLOG_ERROR("gfx_gx2: !! framebuffer-texture MEM2 fallback allocation FAILED size={}",
                         buffer->texture.surface.imageSize);
        }
        buffer->colorBufferMem1 = false;
    } else {
        buffer->colorBufferMem1 = true;
    }
    if (!buffer->texture.surface.image) {
        SPDLOG_ERROR("gfx_gx2: framebuffer color-buffer allocation failed");
        return;
    }

    buffer->color_buffer.surface.image = buffer->texture.surface.image;
    SPDLOG_INFO("gfx_gx2: UpdateFramebufferParameters complete fb={} size={}x{}", fb, width, height);
}

void gfx_gx2_start_draw_to_framebuffer(int fb, float noise_scale) {
    const bool trace = gfx_gx2_trace_first_frame;
    struct Framebuffer* buffer = (struct Framebuffer*)fb;

    // fb 0 = main buffer
    if (!buffer) {
        buffer = &main_framebuffer;
    }

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: StartDrawToFramebuffer ptr={} ...", static_cast<const void*>(buffer));
    }

    if (noise_scale != 0.0f) {
        current_noise_scale = 1.0f / noise_scale;
    }

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: GX2SetColorBuffer ...");
    }
    GX2SetColorBuffer(&buffer->color_buffer, GX2_RENDER_TARGET_0);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: GX2SetColorBuffer complete; GX2SetDepthBuffer ...");
    }
    GX2SetDepthBuffer(&buffer->depth_buffer);

    current_framebuffer = buffer;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first frame: GX2SetDepthBuffer complete; StartDrawToFramebuffer complete");
    }
}

void gfx_gx2_clear_framebuffer(bool clear_color, bool clear_depth) {
    const bool trace = gfx_gx2_trace_first_frame;
    struct Framebuffer* buffer = current_framebuffer;

    if (!buffer) {
        SPDLOG_ERROR("gfx_gx2: ClearFramebuffer skipped: no current framebuffer");
        return;
    }

    // The old C interface always cleared both attachments. The modern interface
    // asks for them independently, and Fast3D issues depth-only clears between
    // framebuffer passes -- clearing colour there would wipe the scene.
    if (clear_color) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2ClearColor ...");
        }
        GX2ClearColor(&buffer->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2ClearColor complete");
        }
    }

    if (clear_depth) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2ClearDepthStencilEx ...");
        }
        GX2ClearDepthStencilEx(&buffer->depth_buffer, buffer->depth_buffer.depthClear,
                               buffer->depth_buffer.stencilClear, GX2_CLEAR_FLAGS_BOTH);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2ClearDepthStencilEx complete");
        }
    }

    if (clear_color || clear_depth) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2SetContextState ...");
        }
        gfx_wiiu_set_context_state();
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2SetContextState complete");
        }
    }
}

void gfx_gx2_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    const bool trace = gfx_gx2_trace_first_frame;
    struct Framebuffer* src_buffer = gfx_gx2_lookup_framebuffer(fb_id_source);
    struct Framebuffer* target_buffer = gfx_gx2_lookup_framebuffer(fb_id_target);

    if (!src_buffer || !target_buffer) {
        static uint32_t rejection_logs = 0;
        if (rejection_logs < 4) {
            ++rejection_logs;
            Ship::WiiU::Watchdog::Emit("GX2: rejected framebuffer copy source=%d target=%d\n", fb_id_source,
                                       fb_id_target);
        }
        return;
    }

    if (src_buffer->color_buffer.surface.aa == GX2_AA_MODE1X) {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2CopySurface (resolve) ...");
        }
        GX2CopySurface(&src_buffer->color_buffer.surface, src_buffer->color_buffer.viewMip,
                       src_buffer->color_buffer.viewFirstSlice, &target_buffer->color_buffer.surface,
                       target_buffer->color_buffer.viewMip, target_buffer->color_buffer.viewFirstSlice);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2CopySurface (resolve) complete");
        }
    } else {
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2ResolveAAColorBuffer ...");
        }
        GX2ResolveAAColorBuffer(&src_buffer->color_buffer, &target_buffer->color_buffer.surface,
                                target_buffer->color_buffer.viewMip, target_buffer->color_buffer.viewFirstSlice);
        if (trace) {
            SPDLOG_INFO("gfx_gx2: first frame: GX2ResolveAAColorBuffer complete");
        }
    }
}

void* gfx_gx2_get_framebuffer_texture_id(int fb_id) {
    struct Framebuffer* buffer = (struct Framebuffer*)fb_id;

    // fb 0 = main buffer
    if (!buffer) {
        buffer = &main_framebuffer;
    }

    return &buffer->imtex;
}

void gfx_gx2_select_texture_fb(int fb) {
    struct Framebuffer* buffer = (struct Framebuffer*)fb;
    if (!buffer) {
        SPDLOG_ERROR("gfx_gx2: texture framebuffer selection received a null framebuffer");
        return;
    }

    if (!current_shader_program) {
        SPDLOG_ERROR("gfx_gx2: texture framebuffer selection has no current shader");
        return;
    }
    tile_bound_texture[0] = &buffer->texture;
    tile_bound_sampler[0] = &buffer->sampler;
    uint32_t location = current_shader_program->samplers_location[0];
    gfx_gx2_set_pixel_texture(0, location, &buffer->texture);
    GX2SetPixelSampler(&buffer->sampler, location);
}

static std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
gfx_gx2_get_pixel_depth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    struct Framebuffer* buffer = (struct Framebuffer*)fb_id;

    // fb 0 = main buffer
    if (!buffer) {
        buffer = &main_framebuffer;
    }

    // Poll only: never wait in the depth path. Retired slots update the cache;
    // an in-flight result remains eligible on a later frame.
    gfx_gx2_collect_retired_depth_readbacks();

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
    DrawBufferSlot& slot = draw_buffer_slots[draw_buffer_slot_index];
    for (const auto& coordinate : coordinates) {
        const auto cached = depth_readback_cache.find({ buffer, coordinate });
        res.emplace(coordinate, cached == depth_readback_cache.end() ? 65532 : cached->second);

        const auto already_queued = std::find_if(
            slot.depth_read_requests.begin(), slot.depth_read_requests.end(),
            [buffer, &coordinate](const DepthReadbackRequest& request) {
                return request.framebuffer == buffer && request.coordinate == coordinate;
            });
        if (already_queued == slot.depth_read_requests.end()) {
            slot.depth_read_requests.push_back({ buffer, coordinate });
        }
    }

    if (slot.depth_read_requests.size() <= UINT32_MAX) {
        gfx_gx2_resize_depth_readback(slot, static_cast<uint32_t>(slot.depth_read_requests.size()));
    }

    return res;
}

void gfx_gx2_set_texture_filter(FilteringMode mode) {
    // three-point is not implemented in the shaders yet
    if (mode == FILTER_THREE_POINT) {
        mode = FILTER_LINEAR;
    }

    current_filter_mode = mode;
    gfx_texture_cache_clear();
}

FilteringMode gfx_gx2_get_texture_filter(void) {
    return current_filter_mode;
}

ImGui_ImplGX2_Texture* gfx_gx2_texture_for_imgui(uint32_t texture_id) {
    struct GX2TextureEntry* tex = (struct GX2TextureEntry*)texture_id;
    return &tex->imtex;
}


// ---------------------------------------------------------------------------
// GfxRenderingAPIGX2 — C++ facade over the GX2 backend above.
//
// Modern libultraship replaced the C `GfxRenderingAPI` function-pointer struct
// with an abstract class. The GPU7 code above is deliberately left as
// file-scope statics rather than being restructured into members: there is only
// ever one GPU, the original implementation is proven on hardware, and a
// mechanical re-seating of 800+ lines of register state into `this->` would risk
// silent transcription bugs for no behavioural gain.
// ---------------------------------------------------------------------------

GfxRenderingAPIGX2::GfxRenderingAPIGX2(std::shared_ptr<Ship::ConsoleVariable> consoleVariable,
                                       std::shared_ptr<Ship::ResourceManager> resourceManager)
    : mConsoleVariables(std::move(consoleVariable)), mResourceManager(std::move(resourceManager)) {
    gGx2ConsoleVariables = mConsoleVariables;
}

GfxRenderingAPIGX2::~GfxRenderingAPIGX2() {
    gfx_gx2_shutdown();
}

const char* GfxRenderingAPIGX2::GetName() {
    return gfx_gx2_get_name();
}
int GfxRenderingAPIGX2::GetMaxTextureSize() {
    return gfx_gx2_get_max_texture_size();
}
GfxClipParameters GfxRenderingAPIGX2::GetClipParameters() {
    return gfx_gx2_get_clip_parameters();
}

void GfxRenderingAPIGX2::UnloadShader(ShaderProgram* oldPrg) {
    gfx_gx2_unload_shader(oldPrg);
}
void GfxRenderingAPIGX2::LoadShader(ShaderProgram* newPrg) {
    gfx_gx2_load_shader(newPrg);
}
ShaderProgram* GfxRenderingAPIGX2::CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) {
    return gfx_gx2_create_and_load_new_shader(shaderId0, shaderId1);
}
ShaderProgram* GfxRenderingAPIGX2::LookupShader(uint64_t shaderId0, uint64_t shaderId1) {
    return gfx_gx2_lookup_shader(shaderId0, shaderId1);
}
void GfxRenderingAPIGX2::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    gfx_gx2_shader_get_info(prg, numInputs, usedTextures);
}
void GfxRenderingAPIGX2::ClearShaderCache() {
    // New in the modern interface. Free every generated GX2 shader group and drop
    // the pool; the next draw regenerates on demand via CreateAndLoadNewShader.
    if (!shader_program_pool.empty() && !gfx_wiiu_gx2_is_down()) {
        gfx_gx2_draw_done("shader destruction");
    }
    for (auto& entry : shader_program_pool) {
        gx2FreeShaderGroup(&entry.second.group);
    }
    shader_program_pool.clear();
    WDOG_SHADER_PROGRAM_POOL_SIZE(shader_program_pool.size());
    current_shader_program = nullptr;
}

uint32_t GfxRenderingAPIGX2::NewTexture() {
    return gfx_gx2_new_texture();
}
void GfxRenderingAPIGX2::SelectTexture(int tile, uint32_t textureId) {
    gfx_gx2_select_texture(tile, textureId);
}
void GfxRenderingAPIGX2::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    gfx_gx2_upload_texture(rgba32Buf, width, height);
}
void GfxRenderingAPIGX2::SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) {
    gfx_gx2_set_sampler_parameters(sampler, linearFilter, cms, cmt);
}
void GfxRenderingAPIGX2::DeleteTexture(uint32_t texId) {
    gfx_gx2_delete_texture(texId);
}
void GfxRenderingAPIGX2::SetTextureFilter(FilteringMode mode) {
    gfx_gx2_set_texture_filter(mode);
}
FilteringMode GfxRenderingAPIGX2::GetTextureFilter() {
    return gfx_gx2_get_texture_filter();
}

void GfxRenderingAPIGX2::SetDepthTestAndMask(bool depthTest, bool zUpd) {
    gfx_gx2_set_depth_test_and_mask(depthTest, zUpd);
}
void GfxRenderingAPIGX2::SetZmodeDecal(bool decal) {
    gfx_gx2_set_zmode_decal(decal);
}
void GfxRenderingAPIGX2::SetViewport(int x, int y, int width, int height) {
    gfx_gx2_set_viewport(x, y, width, height);
}
void GfxRenderingAPIGX2::SetScissor(int x, int y, int width, int height) {
    gfx_gx2_set_scissor(x, y, width, height);
}
void GfxRenderingAPIGX2::SetUseAlpha(bool useAlpha) {
    gfx_gx2_set_use_alpha(useAlpha);
}
void GfxRenderingAPIGX2::DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) {
    gfx_gx2_draw_triangles(bufVbo, bufVboLen, bufVboNumTris);
}

void GfxRenderingAPIGX2::Init() {
    gfx_gx2_init();
}
void GfxRenderingAPIGX2::OnResize() {
    gfx_gx2_on_resize();
}
void GfxRenderingAPIGX2::StartFrame() {
    gfx_gx2_start_frame();
}
void GfxRenderingAPIGX2::EndFrame() {
    gfx_gx2_end_frame();
}
void GfxRenderingAPIGX2::FinishRender() {
    gfx_gx2_finish_render();
}

int GfxRenderingAPIGX2::CreateFramebuffer() {
    return gfx_gx2_create_framebuffer();
}
void GfxRenderingAPIGX2::UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel,
                                                     bool openglInvertY, bool renderTarget, bool hasDepthBuffer,
                                                     bool canExtractDepth) {
    gfx_gx2_update_framebuffer_parameters(fbId, width, height, msaaLevel, openglInvertY, renderTarget, hasDepthBuffer,
                                          canExtractDepth);
}
void GfxRenderingAPIGX2::StartDrawToFramebuffer(int fbId, float noiseScale) {
    gfx_gx2_start_draw_to_framebuffer(fbId, noiseScale);
}
void GfxRenderingAPIGX2::ClearFramebuffer(bool color, bool depth) {
    // Signature changed: the old backend always cleared both. GX2 clears colour
    // and depth through separate calls, so honour the flags rather than ignoring
    // them — Fast3D relies on depth-only clears between framebuffer passes.
    gfx_gx2_clear_framebuffer(color, depth);
}
void GfxRenderingAPIGX2::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
    gfx_gx2_resolve_msaa_color_buffer(fbIdTarget, fbIdSrc);
}
std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIGX2::GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) {
    return gfx_gx2_get_pixel_depth(fbId, coordinates);
}
void* GfxRenderingAPIGX2::GetFramebufferTextureId(int fbId) {
    return gfx_gx2_get_framebuffer_texture_id(fbId);
}
void GfxRenderingAPIGX2::SelectTextureFb(int fbId) {
    gfx_gx2_select_texture_fb(fbId);
}

void GfxRenderingAPIGX2::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                         int dstX0, int dstY0, int dstX1, int dstY1) {
    // New upstream; GPU7 has no scaling blit exposed through GX2. Fall back to the
    // MSAA resolve path when the rectangles match, which is how Fast3D uses it in
    // practice, and no-op otherwise rather than corrupting the target.
    if (srcX1 - srcX0 == dstX1 - dstX0 && srcY1 - srcY0 == dstY1 - dstY0) {
        gfx_gx2_resolve_msaa_color_buffer(fbDstId, fbSrcId);
    }
}

void GfxRenderingAPIGX2::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    // New upstream, used for screenshots//framebuffer effects. Not implemented for
    // GX2 yet: leave the caller's buffer untouched rather than returning garbage.
    (void)fbId;
    (void)width;
    (void)height;
    (void)rgba16Buf;
}

void GfxRenderingAPIGX2::SetSrgbMode() {
    mSrgbMode = true;
}

void GfxRenderingAPIGX2::SetCurrentPrimDepth(float depth) {
    // GX2's generated shader path has no primitive-depth uniform equivalent;
    // retain the value for interface/state parity without changing the GPU7
    // shader ABI.
    mCurrentPrimDepth = depth;
    mPrimDepthDirty = false;
}

ImTextureID GfxRenderingAPIGX2::GetTextureById(int id) {
    return (ImTextureID)(uintptr_t)gfx_gx2_get_framebuffer_texture_id(id);
}

} // namespace Fast

#endif // ENABLE_GX2
