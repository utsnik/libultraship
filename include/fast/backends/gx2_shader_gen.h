/*  gx2_shader_gen.h - Fast3D GX2 shader generator for libultraship

    Created in 2022 by GaryOderNichts
*/
#pragma once

// CCFeatures moved from the deleted gfx_cc.h into fast/interpreter.h, where it sits at
// global scope. Only a pointer to it appears below, so forward-declare rather than include:
// interpreter.h drags in the whole libultraship dependency tree (nlohmann, spdlog, ...) and
// every consumer of this header would pay for it. The definition is included in the .cpp.
struct CCFeatures;
#include <gx2/shaders.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ShaderGroup {
    GX2VertexShader vertexShader;
    GX2PixelShader pixelShader;
    GX2FetchShader fetchShader;

    uint32_t stride;

    uint32_t numAttributes;
    GX2AttribStream attributes[13];
};

int gx2GenerateShaderGroup(struct ShaderGroup* group, struct CCFeatures* cc_features);

int gx2GenerateShaderGroupWithKey(struct ShaderGroup* group, struct CCFeatures* cc_features, uint64_t shader_id0,
                                  uint32_t shader_id1);

void gx2FreeShaderGroup(struct ShaderGroup* group);

#ifdef __cplusplus
}
#endif
