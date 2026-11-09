#pragma once

#include "../util/config/config.h"

#include "../dxgi/dxgi_options.h"

#include "../dxvk/dxvk_device.h"

#include "d3d11_include.h"

namespace dxvk {

  struct D3D11Options {
    D3D11Options(const Config& config);

    /// Force thread-group shared memory accesses to be volatile
    ///
    /// Workaround for compute shaders that read and
    /// write from the same shared memory location
    /// without explicit synchronization.
    bool forceComputeLdsBarriers = false;

    /// Force UAV synchronization insided compute shaders
    ///
    /// Workaround for compute shaders that access overlapping
    /// memory regions within a UAV without proper workgroup
    /// synchroniation. Will have a negative performance impact.
    bool forceComputeUavBarriers = false;

    /// Use relaxed memory barriers
    ///
    /// May improve performance in some games,
    /// but might also cause rendering issues.
    bool relaxedBarriers = false;

    /// Ignore graphics barriers
    ///
    /// May improve performance in some games,
    /// but might also cause rendering issues.
    bool relaxedGraphicsBarriers = false;

    /// LMU emulated quad-views foveated rendering. Off by default.
    bool lmuFoveation = false;

    /// Focus crop: the part of the field of view rendered at native resolution,
    /// as a fraction of the screen width and height (0..1), centred on the gaze.
    float lmuFoveationFocusCrop = 0.4f;

    /// Focus render density relative to native (1.5 = 1.5x1.5 shaded pixels
    /// per screen pixel). Clamped so the focus never exceeds the eye size.
    float lmuFoveationFocusScale = 1.0f;

    /// Periphery render scale, as a fraction of native resolution (0.25 =
    /// quarter). Independent of the focus crop.
    float lmuFoveationPeripheryScale = 0.5f;

    /// Per-eye nasal inset of the focus centre in NDC, used when gaze is
    /// unavailable. Applied toward the nose: opposite sign per eye.
    float lmuFoveationFocusInset = 0.25f;

    /// Feather width of the focus edge, as a fraction of the focus rectangle.
    float lmuFoveationFeather = 0.15f;

    /// Drive the focus centre from gaze in Z:\dev\shm\lmu_gaze.
    bool lmuFoveationGaze = false;

    /// Remap fragment SV_Position to true screen pixels in the foveated views,
    /// so screen-space lookups (shadow/AO/reflection masks) address the right
    /// place. On by default; a toggle for A/B validation.
    bool lmuFoveationFragCoordRemap = true;

    /// Disable foveation when focusCrop^2 + peripheryScale^2 reaches 1, i.e.
    /// when it would shade at least as many pixels as native rendering.
    bool lmuFoveationCostGuard = true;

    /// Draws a green border around the focus rectangle.
    bool lmuFoveationDebug = false;

    /// Maximum tessellation factor.
    ///
    /// Limits tessellation factors in tessellation
    /// control shaders. Values from 8 to 64 are
    /// supported, other values will be ignored.
    int32_t maxTessFactor = 0;

    /// Anisotropic filter override
    ///
    /// Enforces anisotropic filtering with the
    /// given anisotropy value for all samplers.
    int32_t samplerAnisotropy = -1;

    /// Mipmap LOD bias
    ///
    /// Enforces the given LOD bias for all samplers.
    float samplerLodBias = 0.0f;

    /// Clamps negative LOD bias
    bool clampNegativeLodBias = false;

    /// Override maximum frame latency if the app specifies
    /// a higher value. May help with frame timing issues.
    int32_t maxFrameLatency = 0;

    /// Defer surface creation until first present call. This
    /// fixes issues with games that create multiple swap chains
    /// for a single window that may interfere with each other.
    bool deferSurfaceCreation = false;

    /// Enables sample rate shading by interpolating fragment shader
    /// inputs at the sample location rather than pixel center,
    /// unless otherwise specified by the application.
    bool forceSampleRateShading = false;

    /// Forces the sample count of all textures to be 1, and
    /// performs the required shader and resolve fixups.
    bool disableMsaa = false;

    /// Dynamic resources with the given bind flags will be allocated
    /// in cached system memory. Enabled automatically when recording
    /// an api trace.
    uint32_t cachedDynamicResources = 0;

    /// Always lock immediate context on every API call. May be
    /// useful for debugging purposes or when applications have
    /// race conditions.
    bool enableContextLock = false;

    /// Whether to expose the driver command list feature. Enabled by
    /// default and generally beneficial, but some games may assume that
    /// this is not supported when running on an AMD GPU.
    bool exposeDriverCommandLists = true;

    /// Ensure that for the same D3D commands the output VK commands
    /// don't change between runs. Useful for comparative benchmarking,
    /// can negatively affect performance.
    bool reproducibleCommandStream = false;

    /// Whether to force a staging buffer for mapped images.
    /// Some games are broken and ignore row pitch.
    bool disableDirectImageMapping = false;

    /// Shader dump path
    std::string shaderDumpPath;
  };
  
}
