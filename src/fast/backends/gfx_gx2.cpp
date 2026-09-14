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

#include <map>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "libultraship/libultra/gbi.h"
#include "ship/config/ConsoleVariable.h"

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

#include "fast/backends/imgui_impl_gx2.h"

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
static GX2DepthBuffer depthReadBuffer;
static struct Framebuffer* current_framebuffer;

static std::map<std::pair<uint64_t, uint32_t>, struct ShaderProgram> shader_program_pool;
static struct ShaderProgram* current_shader_program;

static struct GX2TextureEntry* current_texture;
static int current_tile;

// 96 Mb (should be more than enough to draw everything without waiting for the GPU)
#define DRAW_BUFFER_SIZE 0x6000000
static uint8_t* draw_buffer = nullptr;
static uint8_t* draw_ptr = nullptr;

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
        WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_GX2_SET_FETCH_SHADER,
                       "shader_id0=0x%016llX shader_id1=0x%08X", static_cast<unsigned long long>(new_prg->shader_id0),
                       static_cast<unsigned int>(new_prg->shader_id1));
        GX2SetFetchShader(&new_prg->group.fetchShader);
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetFetchShader complete; GX2SetVertexShader ...");
    }
    {
        WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_GX2_SET_VERTEX_SHADER,
                       "shader_id0=0x%016llX shader_id1=0x%08X", static_cast<unsigned long long>(new_prg->shader_id0),
                       static_cast<unsigned int>(new_prg->shader_id1));
        GX2SetVertexShader(&new_prg->group.vertexShader);
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetVertexShader complete; GX2SetPixelShader ...");
    }
    {
        WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_GX2_SET_PIXEL_SHADER,
                       "shader_id0=0x%016llX shader_id1=0x%08X", static_cast<unsigned long long>(new_prg->shader_id0),
                       static_cast<unsigned int>(new_prg->shader_id1));
        GX2SetPixelShader(&new_prg->group.pixelShader);
    }
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load: GX2SetPixelShader complete; uniforms ...");
    }

    gfx_gx2_set_uniforms(new_prg);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader load complete");
        trace_first_shader_load = false;
    }
}

static struct ShaderProgram* gfx_gx2_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    static bool trace_first_shader_creation = true;
    const bool trace = trace_first_shader_creation;
    struct CCFeatures cc_features;
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation begin id0=0x{:016X} id1=0x{:08X}", shader_id0, shader_id1);
    }
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    struct ShaderProgram* prg = &shader_program_pool[std::make_pair(shader_id0, shader_id1)];
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;

    printf("Generating shader: %016llx-%08x\n", shader_id0, shader_id1);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: shader creation: GX2 shader compile/generate (gx2GenerateShaderGroup) ...");
    }
    const int shader_result = gx2GenerateShaderGroupWithKey(&prg->group, &cc_features, shader_id0, shader_id1);
    if (shader_result != 0) {
        SPDLOG_ERROR("gfx_gx2: shader creation: gx2GenerateShaderGroup failed result={}", shader_result);
        printf("Failed to generate shader\n");
        current_shader_program = nullptr;
        trace_first_shader_creation = false;
        return nullptr;
    }
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

static struct ShaderProgram* gfx_gx2_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
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

    if (tex->texture.surface.image) {
        free(tex->texture.surface.image);
    }

    free((void*)tex);
}

static void gfx_gx2_set_pixel_texture(int tile, int32_t sampler_location, GX2Texture* texture) {
    WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_GX2_SET_PIXEL_TEXTURE,
                   "tile=%d samplerLocation=%d width=%u height=%u pitch=%u imageSize=%u", tile, sampler_location,
                   static_cast<unsigned int>(texture->surface.width), static_cast<unsigned int>(texture->surface.height),
                   static_cast<unsigned int>(texture->surface.pitch), static_cast<unsigned int>(texture->surface.imageSize));
    GX2SetPixelTexture(texture, sampler_location);
}

static void gfx_gx2_select_texture(int tile, uint32_t texture_id) {
    WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_GX2_SELECT_TEXTURE, "tile=%d id=0x%08X", tile,
                   (unsigned int)texture_id);
    static bool trace_first_texture_select = true;
    const bool trace = trace_first_texture_select;
    struct GX2TextureEntry* tex = (struct GX2TextureEntry*)texture_id;
    if (!tex) {
        SPDLOG_ERROR("gfx_gx2: texture selection received a null texture");
        return;
    }
    current_texture = tex;
    current_tile = tile;

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
            GX2DrawDone();
            free(tex->texture.surface.image);
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
        const uint32_t mem1Free = gfx_wiiu_mem1_free();
        const uint32_t mem1Largest = gfx_wiiu_mem1_largest();
        if (!tex->texture.surface.image) {
            SPDLOG_ERROR("gfx_gx2: !! texture allocation failed dimensions={}x{} imageSize={} ptr={} mem1Free={} "
                         "mem1Largest={}",
                         width, height, tex->texture.surface.imageSize,
                         static_cast<const void*>(tex->texture.surface.image), mem1Free, mem1Largest);
        } else {
            SPDLOG_INFO("gfx_gx2: texture allocation dimensions={}x{} imageSize={} ptr={} mem1Free={} mem1Largest={}",
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
        WDOG_EMIT("GX2TEX: alloc %ux%u pitch=%u imageSize=%u align=%u ptr=0x%08X mem1Free=%u count=%u\n",
                  (unsigned int)width, (unsigned int)height, (unsigned int)tex->texture.surface.pitch,
                  (unsigned int)tex->texture.surface.imageSize, (unsigned int)tex->texture.surface.alignment,
                  (unsigned int)(uintptr_t)tex->texture.surface.image, (unsigned int)mem1Free,
                  (unsigned int)::Ship::WiiU::Watchdog::gTexCount);
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
    WDOG_SCOPE_FMT(::Ship::WiiU::Watchdog::PH_GX2_SET_SAMPLER, "tile=%d linear=%d cms=%u cmt=%u", tile,
                   linear_filter ? 1 : 0, (unsigned int)cms, (unsigned int)cmt);
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
        printf("Waiting on GPU!!!\n");
        GX2DrawDone();
        draw_ptr = draw_buffer;
    }

    float* new_vbo = (float*)draw_ptr;
    draw_ptr += ALIGN(vbo_len, GX2_VERTEX_BUFFER_ALIGNMENT);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: OSBlockMove ...");
    }
    OSBlockMove(new_vbo, buf_vbo, vbo_len, FALSE);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: OSBlockMove complete; GX2Invalidate ...");
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, new_vbo, vbo_len);

    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: GX2Invalidate complete; GX2SetAttribBuffer ...");
    }
    GX2SetAttribBuffer(0, vbo_len, current_shader_program->group.stride, new_vbo);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: GX2SetAttribBuffer complete; GX2DrawEx ...");
    }
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3 * buf_vbo_num_tris, 0, 1);
    if (trace) {
        SPDLOG_INFO("gfx_gx2: first draw: GX2DrawEx complete");
        gfx_gx2_trace_first_draw = false;
    }
}

static void gfx_gx2_init(void) {
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

    // create a linear aligned copy of the depth buffer to read pixels to
    memcpy(&depthReadBuffer, &main_framebuffer.depth_buffer, sizeof(GX2DepthBuffer));

    depthReadBuffer.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    depthReadBuffer.surface.width = 32;
    depthReadBuffer.surface.height = 1;

    SPDLOG_INFO("gfx_gx2_init: GX2CalcSurfaceSizeAndAlignment(depth readback) ...");
    GX2CalcSurfaceSizeAndAlignment(&depthReadBuffer.surface);
    SPDLOG_INFO("gfx_gx2_init: GX2CalcSurfaceSizeAndAlignment(depth readback) complete size=0x{:X} alignment=0x{:X}",
                depthReadBuffer.surface.imageSize, depthReadBuffer.surface.alignment);

    SPDLOG_INFO("gfx_gx2_init: depth readback allocation ... size=0x{:X}", depthReadBuffer.surface.imageSize);
    depthReadBuffer.surface.image =
        gfx_wiiu_alloc_mem1(depthReadBuffer.surface.imageSize, depthReadBuffer.surface.alignment);
    SPDLOG_INFO("gfx_gx2_init: depth readback allocation returned ptr={}", depthReadBuffer.surface.image);
    if (!depthReadBuffer.surface.image) {
        SPDLOG_ERROR("gfx_gx2: failed to allocate depth readback buffer");
        return;
    }
    SPDLOG_INFO("gfx_gx2_init: depth readback GX2Invalidate ...");
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER, depthReadBuffer.surface.image,
                  depthReadBuffer.surface.imageSize);
    SPDLOG_INFO("gfx_gx2_init: depth readback GX2Invalidate complete");

    SPDLOG_INFO("gfx_gx2_init: GX2SetColorBuffer ...");
    GX2SetColorBuffer(&main_framebuffer.color_buffer, GX2_RENDER_TARGET_0);
    SPDLOG_INFO("gfx_gx2_init: GX2SetColorBuffer complete; GX2SetDepthBuffer ...");
    GX2SetDepthBuffer(&main_framebuffer.depth_buffer);
    SPDLOG_INFO("gfx_gx2_init: GX2SetDepthBuffer complete");

    current_framebuffer = &main_framebuffer;

    // allocate draw buffer
    SPDLOG_INFO("gfx_gx2_init: draw-buffer allocation ... size=0x{:X}", DRAW_BUFFER_SIZE);
    draw_buffer = (uint8_t*)memalign(GX2_VERTEX_BUFFER_ALIGNMENT, DRAW_BUFFER_SIZE);
    SPDLOG_INFO("gfx_gx2_init: draw-buffer allocation returned ptr={}", static_cast<const void*>(draw_buffer));
    if (!draw_buffer) {
        SPDLOG_ERROR("gfx_gx2: failed to allocate draw buffer");
        return;
    }
    draw_ptr = draw_buffer;

    SPDLOG_INFO("gfx_gx2_init: GX2SetRasterizerClipControl ...");
    GX2SetRasterizerClipControl(TRUE, FALSE);
    SPDLOG_INFO("gfx_gx2_init: GX2SetRasterizerClipControl complete; GX2SetBlendControl ...");

    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);
    SPDLOG_INFO("gfx_gx2_init: GX2SetBlendControl complete; framebuffer setup complete");
}

void gfx_gx2_shutdown(void) {
    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown enter\n");

    if (has_foreground) {
        GX2DrawDone();

        if (depthReadBuffer.surface.image) {
            gfx_wiiu_free_mem1(depthReadBuffer.surface.image);
            depthReadBuffer.surface.image = nullptr;
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

    if (draw_buffer) {
        free(draw_buffer);
        draw_buffer = nullptr;
        draw_ptr = nullptr;
    }

    Ship::WiiU::Watchdog::Emit("SHUTDOWN: gfx_gx2_shutdown exit\n");
}

static void gfx_gx2_on_resize(void) {
}

static void gfx_gx2_start_frame(void) {
    const bool trace = gfx_gx2_trace_first_frame;
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

static void gfx_gx2_end_frame(void) {
    const bool trace = gfx_gx2_trace_first_frame;
    draw_ptr = draw_buffer;

    {
        WDOG_SCOPE(::Ship::WiiU::Watchdog::PH_GX2_DRAW_DONE, "GX2DrawDone");
        GX2DrawDone();
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
    SPDLOG_INFO("gfx_gx2: UpdateFramebufferParameters fb={} size={}x{} msaa={} ...", fb, width, height, msaa_level);
    struct Framebuffer* buffer = (struct Framebuffer*)fb;

    // we don't support updating the main buffer (fb 0)
    if (!buffer) {
        SPDLOG_INFO("gfx_gx2: UpdateFramebufferParameters fb=0 uses the initialized main framebuffer");
        return;
    }

    if (buffer->texture.surface.width == width && buffer->texture.surface.height == height) {
        SPDLOG_INFO("gfx_gx2: UpdateFramebufferParameters unchanged; complete");
        return;
    }

    // make sure the GPU no longer writes to the buffer
    GX2DrawDone();

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

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
    GX2Rect srcRects[25];
    GX2Point dstPoints[25];
    size_t num_coordinates = coordinates.size();
    while (num_coordinates > 0) {
        size_t numRects = 25;
        if (num_coordinates < numRects) {
            numRects = num_coordinates;
        }
        num_coordinates -= numRects;

        // initialize rects and points
        for (size_t i = 0; i < numRects; ++i) {
            const auto& c = *std::next(coordinates.begin(), num_coordinates + i);
            const int32_t x = (int32_t)std::clamp(c.first, 0.0f, (float)(buffer->depth_buffer.surface.width - 1));
            const int32_t y = (int32_t)std::clamp(c.second, 0.0f, (float)(buffer->depth_buffer.surface.height - 1));

            srcRects[i] = GX2Rect{ x, (int32_t)buffer->depth_buffer.surface.height - y, x + 1,
                                   (int32_t)(buffer->depth_buffer.surface.height - y) + 1 };

            // dst points will be spread over the x-axis of the buffer
            dstPoints[i] = GX2Point{ i, 0 };
        }

        // Invalidate the buffer first
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER, depthReadBuffer.surface.image,
                      depthReadBuffer.surface.imageSize);

        // Perform the copy
        GX2CopySurfaceEx(&buffer->depth_buffer.surface, 0, 0, &depthReadBuffer.surface, 0, 0, numRects, srcRects,
                         dstPoints);

        // Wait for draws to be done and restore context, in case GPU was used
        GX2DrawDone();
        gfx_wiiu_set_context_state();

        // read the pixels from the depthReadBuffer
        for (size_t i = 0; i < numRects; ++i) {
            uint32_t tmp = __builtin_bswap32(*((uint32_t*)depthReadBuffer.surface.image + i));
            float val = *(float*)&tmp;

            const auto& c = *std::next(coordinates.begin(), num_coordinates + i);
            res.emplace(c, val * 65532.0f);
        }
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
ShaderProgram* GfxRenderingAPIGX2::CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) {
    return gfx_gx2_create_and_load_new_shader(shaderId0, shaderId1);
}
ShaderProgram* GfxRenderingAPIGX2::LookupShader(uint64_t shaderId0, uint32_t shaderId1) {
    return gfx_gx2_lookup_shader(shaderId0, shaderId1);
}
void GfxRenderingAPIGX2::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    gfx_gx2_shader_get_info(prg, numInputs, usedTextures);
}
void GfxRenderingAPIGX2::ClearShaderCache() {
    // New in the modern interface. Free every generated GX2 shader group and drop
    // the pool; the next draw regenerates on demand via CreateAndLoadNewShader.
    for (auto& entry : shader_program_pool) {
        gx2FreeShaderGroup(&entry.second.group);
    }
    shader_program_pool.clear();
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

ImTextureID GfxRenderingAPIGX2::GetTextureById(int id) {
    return (ImTextureID)(uintptr_t)gfx_gx2_get_framebuffer_texture_id(id);
}

} // namespace Fast

#endif // ENABLE_GX2
