#ifndef GFX_WIIU_H
#define GFX_WIIU_H
#ifdef ENABLE_GX2

#include <vpad/input.h>
#include <padscore/kpad.h>

#include "fast/backends/gfx_rendering_api.h"

// make the default fb always 1080p to not mess with scaling
// Render target, NOT the TV output. GX2SetTVScale/GX2SetDRCScale upscale this to the
// scan mode the TV reports, so output stays 1080p while the GPU draws far fewer pixels.
// KEEP THIS AT THE TV's NATIVE SIZE. Anything lower is upscaled by GX2 and magnifies the
// WHOLE UI - at 1280x720 the menu came out 1.5x oversized on a 1080p set.
// And there is no performance reason to lower it: the ~20 fps measured here is Ocarina of
// Time's NATIVE frame rate, not a GPU limit (a rock-steady 20.0 means frame-paced; a
// GPU-bound reading wobbles). Do not "optimise" it again - that round trip cost a rebuild
// and a visibly wrong menu.
#define WIIU_DEFAULT_FB_WIDTH 1920
#define WIIU_DEFAULT_FB_HEIGHT 1080

extern bool has_foreground;
extern uint32_t frametime;
extern uint64_t gfx_wiiu_perf_vsync_wait_us;
extern uint32_t gfx_wiiu_perf_dropped_frames;
extern "C" void wiiu_get_perf_big_heap(uint32_t* free_bytes, uint32_t* largest_free_bytes);

bool gfx_wiiu_init_mem1(void);

void gfx_wiiu_destroy_mem1(void);

bool gfx_wiiu_init_foreground(void);

void gfx_wiiu_destroy_foreground(void);

uint32_t gfx_wiiu_mem1_free(void);

uint32_t gfx_wiiu_mem1_largest(void);

void* gfx_wiiu_alloc_mem1(uint32_t size, uint32_t alignment);

void gfx_wiiu_free_mem1(void* block);

void* gfx_wiiu_alloc_foreground(uint32_t size, uint32_t alignment);

void gfx_wiiu_free_foreground(void* block);

void gfx_wiiu_set_context_state(void);

bool gfx_wiiu_gx2_is_down(void);

#endif
#endif
