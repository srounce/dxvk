#pragma once

// LMU emulated quad-views foveated rendering.
//
// The game renders each eye's scene into one MSAA colour+depth target (both
// eyes through the same target, one after the other). When enabled, that
// target is shadowed by a two-layer array sized
// max(peripheryScale, focusCrop * focusScale) x eye and rendered in ONE
// multiview pass, each layer through its own viewport anchored top-left in
// its layer:
//
//   layer 0 (periphery): the full field of view through a viewport of
//     peripheryScale x eye, i.e. at peripheryScale density;
//   layer 1 (focus): the focusCrop-sized region around the gaze, magnified by
//     1/focusCrop in the vertex shader, through a viewport of
//     focusCrop * focusScale x eye, i.e. at focusScale times native density
//     (1.0 = exactly native). Geometry outside the crop is clipped.
//
// A view only shades its own viewport, so per-pixel cost is
// peripheryScale^2 + (focusCrop * focusScale)^2 of a native eye and the knobs
// are independent. At the game's MSAA resolve, layer 0 is upscaled over the
// game's resolved image and layer 1 is composited into the focus rectangle
// (bilinearly filtered down when focusScale > 1) with a feathered edge; a focus that
// runs off a screen edge is clipped, not re-centred, so both eyes stay aligned.
//
// Everything the game reads back from the eye target is redirected: clears
// and attachment binds to the shadow; colour SRVs to the shadow (their
// consumers compute coordinates from the image's own size); depth SRVs to a
// full-size MSAA depth bilinearly reconstructed from the periphery, refreshed
// on demand at SRV bind from the shadow's current contents, because the game
// samples it mid-frame before its own resolve and both eyes share one target
// (once-per-resolve would hand each eye the other eye's depth). Fragment
// SV_Position is remapped to true screen pixels by the shader patch in
// dxvk_shader_ir.cpp so screen-space lookups derived from it stay correct.
//
// Gaze arrives through a seqlocked shared-memory file written by the OpenXR
// layer; the fixed nasal inset is the fallback for headsets without it.

#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../dxvk/dxvk_framebuffer.h"
#include "../dxvk/dxvk_image.h"
#include "../dxvk/dxvk_shader.h"

namespace dxvk {

  class D3D11Device;

  /**
   * \brief Shared-memory gaze sample
   *
   * Written by the OpenXR gaze layer to /dev/shm/lmu_gaze, mapped here as
   * Z:\dev\shm\lmu_gaze. Per-eye focus centre in clip-space NDC (+X right,
   * +Y up). Seqlock: \c sequence is odd while a write is in progress.
   */
  struct D3D11GazeShm {
    uint32_t sequence;
    uint32_t valid;
    float    leftX,  leftY;
    float    rightX, rightY;
    double   timestampNs;
  };

  /**
   * \brief Result of binding a render target
   *
   * Per-view viewport scales relative to the eye viewport (both 1.0 unless
   * the bound target is the foveated eye shadow), and the per-eye push block
   * for the shader patch (identity for any other bind, so single-view draws
   * never see a stale transform). Layout matches DxvkLmuFoveaPushData:
   * focusScale[2], focusOffset[2], fragScale[2], fragOffset[2].
   */
  struct D3D11FoveaBind {
    float periphery = 1.0f;
    float focus     = 1.0f;
    std::array<float, 8> push = { 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f };

    static_assert(sizeof(push) == sizeof(DxvkLmuFoveaPushData));

    bool active() const {
      return periphery != 1.0f || focus != 1.0f;
    }
  };

  /**
   * \brief Prepared foveated resolve
   *
   * What the context needs to resolve the shadow layers and produce the
   * game's resolved image: upscale the periphery over it, then composite the
   * focus into its rectangle.
   */
  struct D3D11FoveatedResolve {
    bool              active = false;
    Rc<DxvkImage>     msaa;                 ///< shadow colour (resolve source)
    Rc<DxvkImage>     resolved;             ///< shadow-sized single-sample scratch
    Rc<DxvkImageView> resolvedView;         ///< layer 0 (periphery), sampled
    Rc<DxvkImageView> focusView;            ///< layer 1 (focus), sampled, or null
    Rc<DxvkImageView> dstView;              ///< the game's resolved image, attachment
    VkExtent2D        dstExtent = { };
    uint32_t          layerCount = 1u;
    VkExtent2D        peripherySrcExtent = { };  ///< periphery sub-rect in layer 0
    VkExtent2D        focusSrcExtent = { };      ///< focus sub-rect in layer 1
    VkOffset2D        focusFullOffset = { };     ///< full focus rect (may be off-image)
    VkExtent2D        focusFullExtent = { };
    VkOffset2D        focusOffset = { };         ///< visible part of the focus rect
    VkExtent2D        focusExtent = { };
    float             feather = 0.0f;
    bool              debugBorder = false;
  };

  /**
   * \brief On-demand depth refresh for an eye SRV bind
   */
  struct D3D11FoveatedSrvUpscale {
    bool              active = false;
    Rc<DxvkImageView> depthDst;             ///< full-size MSAA depth, attachment
    Rc<DxvkImageView> depthSrc;             ///< shadow depth layer 0, sampled 2D-MS
    VkExtent2D        srcExtent = { };      ///< periphery sub-rect in shadow px
    VkExtent2D        dstExtent = { };      ///< native eye px
  };

  struct D3D11FoveationTarget {
    Rc<DxvkImage>     sourceColor;          ///< the game's eye MSAA colour
    Rc<DxvkImage>     sourceDepth;          ///< its paired depth
    Rc<DxvkImage>     shadowColor;          ///< two-layer MSAA colour
    Rc<DxvkImage>     shadowDepth;          ///< two-layer MSAA depth
    VkExtent2D        shadowExtent = { };
    VkExtent2D        peripheryExtent = { }; ///< peripheryScale x eye
    VkExtent2D        focusExtent = { };     ///< focusCrop x eye, the on-screen rect
    VkExtent2D        focusRenderExtent = { }; ///< focusCrop * focusScale x eye, the viewport

    Rc<DxvkImage>     shadowResolved;       ///< single-sample scratch, two layers
    Rc<DxvkImageView> shadowResolvedView;   ///< layer 0
    Rc<DxvkImageView> shadowFocusView;      ///< layer 1
    DxvkImage*        resolveDst = nullptr;
    Rc<DxvkImageView> resolveDstView;

    Rc<DxvkImage>     depthUpscaled;        ///< full-size MSAA depth for eye-depth readers
    Rc<DxvkImageView> depthUpscaledView;    ///< attachment view
    Rc<DxvkImageView> shadowDepthMsView;    ///< sampled 2D-MS view of shadow depth layer 0

    /// Set when the shadow is bound as a render target, cleared when the
    /// depth upscale is refreshed from it.
    bool              srvDirty = false;
  };

  class D3D11FoveationManager {

  public:

    D3D11FoveationManager(D3D11Device* pDevice);
    ~D3D11FoveationManager();

    bool isEnabled() const {
      return m_enabled;
    }

    /**
     * \brief Records an imported (OpenXR swapchain) image extent
     *
     * Eye targets are recognised by their size relative to the swapchain.
     */
    void noteImportedImage(
      const DxvkImageCreateInfo&  info);

    /**
     * \brief Substitutes the eye target at render-target bind
     *
     * Detects the eye target on first sight, swaps the attachments for the
     * shadow (two layers, view mask 0x3), latches the gaze for the eye about
     * to render, and returns the per-view viewport scales and push block.
     * Mutates \c attachments; call before they are moved into the CS chunk.
     */
    D3D11FoveaBind onBindRenderTargets(
            DxvkRenderTargets&    attachments);

    /**
     * \brief Redirects a clear to the shadow
     */
    Rc<DxvkImageView> remapAttachmentView(
      const Rc<DxvkImageView>&    view);

    /**
     * \brief Redirects a shader resource view of the eye target
     *
     * Colour goes to the shadow (layer 0); depth to the full-size upscaled
     * depth, created on first use.
     */
    Rc<DxvkImageView> remapShaderResourceView(
      const Rc<DxvkImageView>&    view);

    /**
     * \brief Depth refresh to run before a draw that samples the eye
     *
     * Active when \c view samples the eye depth or colour and the shadow has
     * been drawn to since the last refresh. Clears the dirty flag.
     */
    D3D11FoveatedSrvUpscale takeSrvUpscale(
      const Rc<DxvkImageView>&    view);

    /**
     * \brief Redirects a plain resolve source to the shadow
     */
    Rc<DxvkImage> remapResolveSource(
      const Rc<DxvkImage>&        source);

    /**
     * \brief Prepares the foveated resolve of the eye into \c dst
     *
     * Inactive when \c srcEye is not a detected target. Advances the eye
     * parity.
     */
    D3D11FoveatedResolve prepareResolve(
      const Rc<DxvkImage>&        srcEye,
      const Rc<DxvkImage>&        dst);

  private:

    static constexpr uint32_t kGazeStaleMax = 200u;

    D3D11Device*  m_device;
    bool          m_enabled        = false;
    bool          m_multiview      = false;
    bool          m_debug          = false;
    bool          m_gaze           = false;
    bool          m_fragRemap      = true;
    float         m_peripheryScale = 0.5f;
    float         m_focusCrop      = 0.4f;
    float         m_focusScale     = 1.0f;   ///< focus density relative to native
    float         m_scale          = 1.0f;   ///< shadow scale = max(periphery, crop * focusScale)
    float         m_focusInset     = 0.25f;
    float         m_feather        = 0.15f;

    // Eye parity, advanced once per foveated resolve. The gaze and focus rect
    // are latched once per eye, at its first bind after the previous resolve,
    // so the vertex transform, the fragment remap and the composite all use
    // the same values within an eye.
    uint32_t      m_eyeIndex   = 0u;
    uint32_t      m_latchedEye = ~0u;
    float         m_latchX[2]  = { 0.0f, 0.0f };   ///< focus centre, NDC
    float         m_latchY[2]  = { 0.0f, 0.0f };
    int32_t       m_latchFx[2] = { 0, 0 };         ///< focus rect origin, eye px
    int32_t       m_latchFy[2] = { 0, 0 };

    void*         m_gazeFile    = nullptr;
    void*         m_gazeMapping = nullptr;
    D3D11GazeShm* m_gazePtr     = nullptr;
    bool          m_gazeOpenTried = false;
    float         m_gazeCache[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool          m_gazeHave  = false;
    uint32_t      m_gazeStale = 0u;

    dxvk::mutex   m_mutex;

    std::vector<VkExtent2D>                              m_swapExtents;
    std::unordered_map<DxvkImage*, D3D11FoveationTarget> m_targets;

    bool matchesSwapchain(
            VkExtent3D            extent,
            VkExtent2D&           swapOut) const;

    D3D11FoveationTarget* detect(
      const DxvkRenderTargets&    attachments);

    Rc<DxvkImage> createShadow(
      const Rc<DxvkImage>&        source,
      const char*                 role);

    void ensureDepthUpscaled(
            D3D11FoveationTarget& target);

    void latchEye(
            uint32_t              eye,
      const D3D11FoveationTarget& target);

    bool readGaze(float& lx, float& ly, float& rx, float& ry);

    void openGaze();

  };

}
