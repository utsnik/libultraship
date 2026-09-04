#ifdef ENABLE_GX2
#pragma once

#include <memory>
#include <map>
#include <set>
#include <unordered_map>

#include "gfx_rendering_api.h"
#include "../interpreter.h"

#include <gx2/texture.h>
#include <gx2/sampler.h>
#include <gx2/surface.h>
#include <gx2/context.h>
#include <gx2/shaders.h>

#include "gx2_shader_gen.h"

namespace Ship {
class ConsoleVariable;
class ResourceManager;
} // namespace Ship

namespace Fast {

/**
 * @brief GX2 (Wii U GPU7) shader program metadata cached by the Fast3D renderer.
 *
 * Unlike the desktop backends, GX2 consumes compiled GPU7 bytecode rather than
 * shader source, so `group` holds the generated vertex/pixel shader pair rather
 * than a driver-side program handle.
 */
struct ShaderProgram {
    ShaderGroup group;
    uint8_t numInputs;
    bool usedTextures[SHADER_MAX_TEXTURES];
    bool usedNoise;
    uint32_t windowParamsOffset;
    int32_t samplersLocation[SHADER_MAX_TEXTURES];
};

struct GX2TextureEntry {
    GX2Texture texture;
    bool textureUploaded;

    GX2Sampler sampler;
    bool samplerSet;
};

struct GX2Framebuffer {
    GX2ColorBuffer colorBuffer;
    GX2DepthBuffer depthBuffer;
    uint32_t textureId;
    bool hasDepthBuffer;
    uint32_t msaaLevel;
};

class GfxRenderingAPIGX2 : public GfxRenderingAPI {
  public:
    GfxRenderingAPIGX2(std::shared_ptr<Ship::ConsoleVariable> consoleVariable,
                       std::shared_ptr<Ship::ResourceManager> resourceManager);
    ~GfxRenderingAPIGX2() override;

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;

    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;

    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;

    void SetDepthTestAndMask(bool depthTest, bool zUpd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) override;

    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;

    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel, bool openglInvertY,
                                     bool renderTarget, bool hasDepthBuffer, bool canExtractDepth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;

    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;

  private:
    void ApplyDepthState();
    void ApplyBlendState();

    std::shared_ptr<Ship::ConsoleVariable> mConsoleVariables;
    std::shared_ptr<Ship::ResourceManager> mResourceManager;

    std::map<std::pair<uint64_t, uint64_t>, ShaderProgram> mShaderProgramPool;
    ShaderProgram* mCurrentShaderProgram = nullptr;

    std::map<uint32_t, GX2TextureEntry> mTextures;
    uint32_t mCurrentTextureIds[SHADER_MAX_TEXTURES] = {};
    int mCurrentTile = 0;
    uint32_t mNextTextureId = 1;

    std::map<int, GX2Framebuffer> mFramebuffers;
    int mNextFramebufferId = 0;
    int mCurrentFramebufferId = 0;

    FilteringMode mCurrentFilterMode = FILTER_THREE_POINT;
    bool mCurrentUseAlpha = false;
    float mCurrentNoiseScale = 0.0f;
};
} // namespace Fast
#endif
