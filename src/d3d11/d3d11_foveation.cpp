#include "d3d11_device.h"
#include "d3d11_foveation.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk {

  // Single-layer 2D view of one array layer, for the resolve, blit and
  // composite paths (colour) and the depth upscale (depth aspect).
  static DxvkImageViewKey MakeLayerViewKey(
    const Rc<DxvkImage>&  image,
          VkImageUsageFlags usage,
          VkImageAspectFlags aspects,
          uint32_t          layer) {
    DxvkImageViewKey key = { };
    key.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    key.usage      = usage;
    key.format     = image->info().format;
    key.aspects    = aspects;
    key.mipIndex   = 0u;
    key.mipCount   = 1u;
    key.layerIndex = uint16_t(layer);
    key.layerCount = 1u;
    key.packedSwizzle = DxvkImageViewKey::packSwizzle(VkComponentMapping {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY });
    return key;
  }


  static DxvkImageViewKey LayeredKey(DxvkImageViewKey key, uint32_t layers) {
    key.layerIndex = 0u;
    key.layerCount = uint16_t(layers);
    key.viewType   = layers > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    return key;
  }


  // Plausible HDR scene colour: colour-only, float/unorm, not block compressed,
  // R,G,B present, wider than 32bpp or a packed HDR float format. Excludes
  // RGBA8 probes and shadow maps and 1/2-channel buffers.
  static bool IsSceneColorFormat(VkFormat format) {
    const DxvkFormatInfo* fi = lookupFormatInfo(format);

    if (!fi || fi->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
      return false;

    if (fi->flags.any(DxvkFormatFlag::SampledUInt, DxvkFormatFlag::SampledSInt,
                      DxvkFormatFlag::BlockCompressed))
      return false;

    const VkColorComponentFlags rgb =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    if ((fi->componentMask & rgb) != rgb)
      return false;

    return fi->elementSize >= 8u
        || format == VK_FORMAT_B10G11R11_UFLOAT_PACK32
        || format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
  }


  D3D11FoveationManager::D3D11FoveationManager(D3D11Device* pDevice)
  : m_device(pDevice) {
    const D3D11Options* opts = pDevice->GetOptions();

    m_enabled        = opts->lmuFoveation;
    m_debug          = opts->lmuFoveationDebug;
    m_gaze           = opts->lmuFoveationGaze;
    m_fragRemap      = opts->lmuFoveationFragCoordRemap;
    m_peripheryScale = std::clamp(opts->lmuFoveationPeripheryScale, 0.1f, 1.0f);
    m_focusCrop      = std::clamp(opts->lmuFoveationFocusCrop,      0.1f, 1.0f);
    m_focusScale     = std::clamp(opts->lmuFoveationFocusScale, 1.0f, 1.0f / m_focusCrop);
    m_focusInset     = opts->lmuFoveationFocusInset;
    m_feather        = std::clamp(opts->lmuFoveationFeather, 0.0f, 0.5f);
    m_scale          = std::max(m_peripheryScale, m_focusCrop * m_focusScale);

    if (!m_enabled)
      return;

    const auto& feat = pDevice->GetDXVKDevice()->features();
    m_multiview = feat.vk11.multiview && feat.vk12.shaderOutputViewportIndex;

    // A shadow the size of the eye target would render nothing smaller; there
    // is no foveation to do.
    if (!m_multiview || m_scale >= 1.0f) {
      Logger::info(str::format("LMU-FOVEATION: inactive (",
        !m_multiview ? "multiview or vertex viewport index unavailable" : "shadow scale 1.0", ")"));
      m_enabled = false;
      return;
    }

    // Per-pixel cost relative to a native eye. At or above 1 the two views
    // shade more pixels than the eye they replace.
    const float focusDensity = m_focusCrop * m_focusScale;
    const float cost = m_peripheryScale * m_peripheryScale + focusDensity * focusDensity;

    if (cost >= 1.0f) {
      Logger::warn(str::format("LMU-FOVEATION: pixel cost ", cost,
        " of native (periphery ", m_peripheryScale, "^2 + focus ", focusDensity, "^2)",
        opts->lmuFoveationCostGuard ? "; disabled" : "; cost guard off, continuing"));

      if (opts->lmuFoveationCostGuard) {
        m_enabled = false;
        return;
      }
    }

    Logger::info(str::format("LMU-FOVEATION: enabled, focus crop ", m_focusCrop,
      ", focus scale ", m_focusScale,
      ", periphery scale ", m_peripheryScale, ", shadow scale ", m_scale,
      ", gaze ", (m_gaze ? "yes" : "no"),
      ", fragcoord remap ", (m_fragRemap ? "yes" : "no")));
  }


  D3D11FoveationManager::~D3D11FoveationManager() {
    if (m_gazePtr)     UnmapViewOfFile(m_gazePtr);
    if (m_gazeMapping) CloseHandle(m_gazeMapping);
    if (m_gazeFile)    CloseHandle(m_gazeFile);
  }


  void D3D11FoveationManager::noteImportedImage(const DxvkImageCreateInfo& info) {
    if (!m_enabled || !(info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      return;

    VkExtent2D extent = { info.extent.width, info.extent.height };

    std::lock_guard lock(m_mutex);

    for (const auto& e : m_swapExtents) {
      if (e.width == extent.width && e.height == extent.height)
        return;
    }

    m_swapExtents.push_back(extent);
    Logger::info(str::format("LMU-FOVEATION: swapchain image ", extent.width, "x", extent.height));
  }


  bool D3D11FoveationManager::matchesSwapchain(VkExtent3D extent, VkExtent2D& swapOut) const {
    for (const auto& e : m_swapExtents) {
      if (!e.width || !e.height)
        continue;

      double rw = double(extent.width) / double(e.width);

      if (rw >= 0.4 && rw <= 1.05) {
        swapOut = e;
        return true;
      }
    }

    return false;
  }


  Rc<DxvkImage> D3D11FoveationManager::createShadow(const Rc<DxvkImage>& source, const char* role) {
    // The source's create info (format, sample count, usage, view formats)
    // verbatim, with two layers and the scaled extent. The eye targets are
    // shared (D3DKMT) resources; the copy must not be.
    DxvkImageCreateInfo info = source->info();
    info.numLayers = 2u;
    info.shared    = VK_FALSE;
    info.debugName = role;

    // Even sizes keep MSAA sample positions well defined.
    info.extent.width  = std::max(2u, uint32_t(std::lround(info.extent.width  * m_scale)) & ~1u);
    info.extent.height = std::max(2u, uint32_t(std::lround(info.extent.height * m_scale)) & ~1u);

    try {
      auto image = m_device->GetDXVKDevice()->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      Logger::info(str::format("LMU-FOVEATION: created ", role, " ",
        info.extent.width, "x", info.extent.height, "x2 vkfmt=", uint32_t(info.format),
        " s=", uint32_t(info.sampleCount)));
      return image;
    } catch (const DxvkError& e) {
      Logger::warn(str::format("LMU-FOVEATION: failed to create ", role, ": ", e.message()));
      return nullptr;
    }
  }


  D3D11FoveationTarget* D3D11FoveationManager::detect(const DxvkRenderTargets& attachments) {
    const auto& colorView = attachments.color[0].view;
    const auto& depthView = attachments.depth.view;

    if (colorView == nullptr || depthView == nullptr)
      return nullptr;

    DxvkImage* colorImage = colorView->image();
    DxvkImage* depthImage = depthView->image();

    if (!colorImage || !depthImage)
      return nullptr;

    const auto& c = colorImage->info();
    const auto& d = depthImage->info();

    if (!(c.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
     || !(c.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
      return nullptr;

    if (c.extent.width != d.extent.width || c.extent.height != d.extent.height)
      return nullptr;

    double aspect = double(c.extent.height) / double(c.extent.width);

    if (aspect < 0.5 || aspect > 1.25)
      return nullptr;

    if (c.sampleCount == VK_SAMPLE_COUNT_1_BIT && !IsSceneColorFormat(c.format))
      return nullptr;

    VkExtent2D swap = { };

    if (!matchesSwapchain(c.extent, swap))
      return nullptr;

    D3D11FoveationTarget target = { };
    target.sourceColor = colorView->image();
    target.sourceDepth = depthView->image();

    Logger::info(str::format("LMU-FOVEATION: detected eye target ",
      c.extent.width, "x", c.extent.height, " vkfmt=", uint32_t(c.format),
      " s=", uint32_t(c.sampleCount), " depth vkfmt=", uint32_t(d.format),
      " (swapchain ", swap.width, "x", swap.height, ")"));

    target.shadowColor = createShadow(target.sourceColor, "lmu-shadow-color");
    target.shadowDepth = createShadow(target.sourceDepth, "lmu-shadow-depth");

    if (target.shadowColor == nullptr || target.shadowDepth == nullptr) {
      target.shadowColor = nullptr;
      target.shadowDepth = nullptr;
    } else {
      auto e = target.shadowColor->info().extent;
      target.shadowExtent = { e.width, e.height };

      // Per-view sub-rectangles in eye pixels; both fit inside the shadow.
      auto sub = [] (float s, uint32_t n) {
        return std::max(2u, uint32_t(std::lround(double(s) * double(n))));
      };
      target.peripheryExtent = { sub(m_peripheryScale, c.extent.width), sub(m_peripheryScale, c.extent.height) };
      target.focusExtent     = { sub(m_focusCrop,      c.extent.width), sub(m_focusCrop,      c.extent.height) };
      target.focusRenderExtent = { sub(m_focusCrop * m_focusScale, c.extent.width),
                                   sub(m_focusCrop * m_focusScale, c.extent.height) };
    }

    auto result = m_targets.insert({ colorImage, std::move(target) });
    return &result.first->second;
  }


  void D3D11FoveationManager::latchEye(uint32_t eye, const D3D11FoveationTarget& target) {
    float lx, ly, rx, ry;

    if (m_gaze && readGaze(lx, ly, rx, ry)) {
      m_latchX[eye] = std::clamp(eye ? rx : lx, -1.0f, 1.0f);
      m_latchY[eye] = std::clamp(eye ? ry : ly, -1.0f, 1.0f);
    } else {
      // Fixed nasal inset: toward the nose, opposite sign per eye.
      m_latchX[eye] = eye ? -m_focusInset : m_focusInset;
      m_latchY[eye] = 0.0f;
    }

    // Focus rectangle origin in eye pixels, centred on the gaze. NDC y is up,
    // image y is down. It may extend past the image; it is clipped, never
    // re-centred, so the two eyes stay aligned.
    const auto& ext = target.sourceColor->info().extent;
    double cx = (double(m_latchX[eye]) + 1.0) * 0.5 * double(ext.width);
    double cy = (1.0 - double(m_latchY[eye])) * 0.5 * double(ext.height);

    m_latchFx[eye] = int32_t(std::lround(cx - 0.5 * double(target.focusExtent.width)));
    m_latchFy[eye] = int32_t(std::lround(cy - 0.5 * double(target.focusExtent.height)));
  }


  D3D11FoveaBind D3D11FoveationManager::onBindRenderTargets(DxvkRenderTargets& attachments) {
    D3D11FoveaBind bind;

    if (!m_enabled)
      return bind;

    const auto& colorView = attachments.color[0].view;

    if (colorView == nullptr)
      return bind;

    DxvkImage* colorImage = colorView->image();

    std::lock_guard lock(m_mutex);

    auto entry = m_targets.find(colorImage);

    // Detection needs the paired depth; colour-only sub-passes on the eye are
    // recognised once the depth pass has registered the target.
    if (entry == m_targets.end()) {
      if (!detect(attachments))
        return bind;

      entry = m_targets.find(colorImage);
    }

    D3D11FoveationTarget& target = entry->second;

    if (target.shadowColor == nullptr)
      return bind;

    const auto& depthView = attachments.depth.view;
    bool substituteDepth = depthView != nullptr && depthView->image() == target.sourceDepth.ptr();

    // Two-layer multiview needs every attachment layered. A foreign depth
    // cannot be layered, so such a pass renders the periphery layer only.
    bool layered = depthView == nullptr || substituteDepth;
    uint32_t layers = layered ? 2u : 1u;

    attachments.color[0].view = target.shadowColor->createView(LayeredKey(colorView->info(), layers));

    if (substituteDepth)
      attachments.depth.view = target.shadowDepth->createView(LayeredKey(depthView->info(), layers));

    attachments.viewMask = layered ? 0x3u : 0u;
    target.srvDirty = true;

    // Gaze and focus rect are latched once per eye, at its first bind.
    uint32_t eye = m_eyeIndex & 1u;

    if (m_latchedEye != m_eyeIndex) {
      m_latchedEye = m_eyeIndex;
      latchEye(eye, target);
    }

    // Viewports: the periphery and focus sub-rectangles as fractions of the
    // eye viewport (square pixels, so width sets the factor).
    const double eyeWidth = double(target.sourceColor->info().extent.width);
    bind.periphery = float(double(target.peripheryExtent.width) / eyeWidth);
    bind.focus     = float(double(target.focusRenderExtent.width) / eyeWidth);

    // Vertex: view 1 maps the crop [g-f, g+f] around the gaze g onto clip [-1, 1]
    // (view 0 is identity, its viewport does the scaling):
    //   pos.xy' = pos.xy / f - (g / f) * w
    // Fragment: view 0 SV_Position is viewport-local at periphery scale, so
    // multiply by its inverse; view 1's is local to the focus viewport at
    // focusScale density, so divide by that and add the focus rect origin.
    // Both yield true screen pixels.
    const float f  = m_focusCrop;
    const float gx = m_latchX[eye];
    const float gy = m_latchY[eye];
    const float focusDensity = float(double(target.focusRenderExtent.width) / double(target.focusExtent.width));

    bind.push = {
      1.0f / f, 1.0f / f,                     // focusScale
      -gx / f,  -gy / f,                      // focusOffset
      m_fragRemap ? 1.0f / bind.periphery : 1.0f,   // fragScale[0]
      m_fragRemap ? 1.0f / focusDensity : 1.0f,     // fragScale[1]
      m_fragRemap ? float(m_latchFx[eye]) : 0.0f,   // fragOffset
      m_fragRemap ? float(m_latchFy[eye]) : 0.0f };

    return bind;
  }


  Rc<DxvkImageView> D3D11FoveationManager::remapAttachmentView(const Rc<DxvkImageView>& view) {
    if (!m_enabled || view == nullptr)
      return view;

    std::lock_guard lock(m_mutex);

    for (auto& pair : m_targets) {
      auto& target = pair.second;

      if (view->image() == target.sourceColor.ptr() && target.shadowColor != nullptr)
        return target.shadowColor->createView(LayeredKey(view->info(), 2u));

      if (view->image() == target.sourceDepth.ptr() && target.shadowDepth != nullptr)
        return target.shadowDepth->createView(LayeredKey(view->info(), 2u));
    }

    return view;
  }


  Rc<DxvkImageView> D3D11FoveationManager::remapShaderResourceView(const Rc<DxvkImageView>& view) {
    if (!m_enabled || view == nullptr)
      return view;

    std::lock_guard lock(m_mutex);

    for (auto& pair : m_targets) {
      auto& target = pair.second;

      // Same view key on the shadow selects layer 0, the periphery. Colour
      // consumers derive coordinates from the image's own size, so the scaled
      // shadow is self-consistent for them.
      if (view->image() == target.sourceColor.ptr() && target.shadowColor != nullptr)
        return target.shadowColor->createView(view->info());

      // Depth consumers fetch at native coordinates derived from the
      // projection (screen-space reflections), so they get the full-size
      // reconstruction, refreshed on demand by takeSrvUpscale.
      if (view->image() == target.sourceDepth.ptr() && target.shadowDepth != nullptr) {
        ensureDepthUpscaled(target);

        return target.depthUpscaled != nullptr
          ? target.depthUpscaled->createView(view->info())
          : target.shadowDepth->createView(view->info());
      }
    }

    return view;
  }


  void D3D11FoveationManager::ensureDepthUpscaled(D3D11FoveationTarget& target) {
    if (target.depthUpscaled != nullptr)
      return;

    DxvkImageCreateInfo info = target.sourceDepth->info();
    info.numLayers = 1u;
    info.usage    |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.shared    = VK_FALSE;
    info.debugName = "lmu-depth-upscaled";

    try {
      target.depthUpscaled = m_device->GetDXVKDevice()->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      target.depthUpscaledView = target.depthUpscaled->createView(MakeLayerViewKey(
        target.depthUpscaled, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 0u));
      target.shadowDepthMsView = target.shadowDepth->createView(MakeLayerViewKey(
        target.shadowDepth, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 0u));

      Logger::info(str::format("LMU-FOVEATION: created lmu-depth-upscaled ",
        info.extent.width, "x", info.extent.height, " s=", uint32_t(info.sampleCount)));
    } catch (const DxvkError& e) {
      Logger::warn(str::format("LMU-FOVEATION: failed to create upscaled depth: ", e.message()));
      target.depthUpscaled = nullptr;
      target.depthUpscaledView = nullptr;
      target.shadowDepthMsView = nullptr;
    }
  }


  D3D11FoveatedSrvUpscale D3D11FoveationManager::takeSrvUpscale(const Rc<DxvkImageView>& view) {
    D3D11FoveatedSrvUpscale up;

    if (!m_enabled || view == nullptr)
      return up;

    std::lock_guard lock(m_mutex);

    for (auto& pair : m_targets) {
      auto& target = pair.second;

      if (view->image() != target.sourceColor.ptr() && view->image() != target.sourceDepth.ptr())
        continue;

      if (!target.srvDirty || target.depthUpscaled == nullptr)
        return up;

      up.active    = true;
      up.depthDst  = target.depthUpscaledView;
      up.depthSrc  = target.shadowDepthMsView;
      up.srcExtent = target.peripheryExtent;
      up.dstExtent = { target.sourceColor->info().extent.width, target.sourceColor->info().extent.height };
      target.srvDirty = false;
      return up;
    }

    return up;
  }


  Rc<DxvkImage> D3D11FoveationManager::remapResolveSource(const Rc<DxvkImage>& source) {
    if (!m_enabled || source == nullptr)
      return source;

    std::lock_guard lock(m_mutex);

    auto entry = m_targets.find(source.ptr());

    return (entry != m_targets.end() && entry->second.shadowColor != nullptr)
      ? entry->second.shadowColor
      : source;
  }


  D3D11FoveatedResolve D3D11FoveationManager::prepareResolve(
    const Rc<DxvkImage>& srcEye, const Rc<DxvkImage>& dst) {
    D3D11FoveatedResolve op;

    if (!m_enabled || srcEye == nullptr || dst == nullptr)
      return op;

    std::lock_guard lock(m_mutex);

    auto entry = m_targets.find(srcEye.ptr());

    if (entry == m_targets.end() || entry->second.shadowColor == nullptr)
      return op;

    D3D11FoveationTarget& target = entry->second;
    const auto dstExtent = dst->info().extent;

    // Shadow-sized single-sample scratch in the resolved image's format.
    if (target.shadowResolved == nullptr || target.shadowResolved->info().format != dst->info().format) {
      DxvkImageCreateInfo info = { };
      info.type        = VK_IMAGE_TYPE_2D;
      info.format      = dst->info().format;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { target.shadowExtent.width, target.shadowExtent.height, 1u };
      info.numLayers   = 2u;
      info.mipLevels   = 1u;
      info.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.stages      = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                       | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      info.access      = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
                       | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      info.layout      = VK_IMAGE_LAYOUT_GENERAL;
      info.debugName   = "lmu-shadow-resolved";

      try {
        target.shadowResolved = m_device->GetDXVKDevice()->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        target.shadowResolvedView = target.shadowResolved->createView(MakeLayerViewKey(
          target.shadowResolved, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0u));
        target.shadowFocusView = target.shadowResolved->createView(MakeLayerViewKey(
          target.shadowResolved, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1u));
      } catch (const DxvkError& e) {
        Logger::warn(str::format("LMU-FOVEATION: failed to create resolve scratch: ", e.message()));
        target.shadowResolved = nullptr;
        return op;
      }
    }

    if (target.resolveDst != dst.ptr()) {
      target.resolveDst = dst.ptr();
      target.resolveDstView = dst->createView(MakeLayerViewKey(
        dst, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0u));
    }

    op.active             = true;
    op.msaa               = target.shadowColor;
    op.resolved           = target.shadowResolved;
    op.resolvedView       = target.shadowResolvedView;
    op.focusView          = target.shadowFocusView;
    op.dstView            = target.resolveDstView;
    op.dstExtent          = { dstExtent.width, dstExtent.height };
    op.layerCount         = 2u;
    op.peripherySrcExtent = target.peripheryExtent;
    op.focusSrcExtent     = target.focusRenderExtent;

    // Focus rectangle: the latched origin and the native-sized crop, clipped
    // to the image (the composite maps by position within the full rect).
    uint32_t eye = m_eyeIndex & 1u;
    int32_t fx = m_latchFx[eye], fy = m_latchFy[eye];
    int32_t fw = int32_t(target.focusExtent.width), fh = int32_t(target.focusExtent.height);

    int32_t vx0 = std::max(fx, 0), vy0 = std::max(fy, 0);
    int32_t vx1 = std::min(fx + fw, int32_t(dstExtent.width));
    int32_t vy1 = std::min(fy + fh, int32_t(dstExtent.height));

    if (vx1 > vx0 && vy1 > vy0) {
      op.focusFullOffset = { fx, fy };
      op.focusFullExtent = target.focusExtent;
      op.focusOffset     = { vx0, vy0 };
      op.focusExtent     = { uint32_t(vx1 - vx0), uint32_t(vy1 - vy0) };
      op.feather         = m_feather;
      op.debugBorder     = m_debug;
    } else {
      op.focusView = nullptr;
    }

    // This eye is done; the next bind latches the other eye.
    m_eyeIndex++;
    return op;
  }


  void D3D11FoveationManager::openGaze() {
    m_gazeOpenTried = true;

    HANDLE file = CreateFileA("Z:\\dev\\shm\\lmu_gaze", GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE) {
      Logger::warn("LMU-FOVEATION: gaze enabled but Z:\\dev\\shm\\lmu_gaze not found; using fixed focus");
      return;
    }

    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, sizeof(D3D11GazeShm), nullptr);

    if (!mapping) {
      CloseHandle(file);
      Logger::warn("LMU-FOVEATION: gaze CreateFileMapping failed; using fixed focus");
      return;
    }

    void* ptr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(D3D11GazeShm));

    if (!ptr) {
      CloseHandle(mapping);
      CloseHandle(file);
      Logger::warn("LMU-FOVEATION: gaze MapViewOfFile failed; using fixed focus");
      return;
    }

    m_gazeFile    = file;
    m_gazeMapping = mapping;
    m_gazePtr     = reinterpret_cast<D3D11GazeShm*>(ptr);
    Logger::info("LMU-FOVEATION: gaze shared memory mapped");
  }


  bool D3D11FoveationManager::readGaze(float& lx, float& ly, float& rx, float& ry) {
    if (!m_gazeOpenTried)
      openGaze();

    if (m_gazePtr) {
      // Seqlock: retry while a write is in progress or the value moved.
      for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        uint32_t s0 = m_gazePtr->sequence;

        if (s0 & 1u)
          continue;

        uint32_t valid = m_gazePtr->valid;
        float a = m_gazePtr->leftX,  b = m_gazePtr->leftY;
        float c = m_gazePtr->rightX, d = m_gazePtr->rightY;

        if (m_gazePtr->sequence != s0)
          continue;

        if (valid) {
          m_gazeCache[0] = a; m_gazeCache[1] = b;
          m_gazeCache[2] = c; m_gazeCache[3] = d;
          m_gazeHave  = true;
          m_gazeStale = 0u;
          lx = a; ly = b; rx = c; ry = d;
          return true;
        }

        break;
      }
    }

    // Hold the last good gaze through a brief loss (a blink, or another
    // writer on the shared file) so the focus does not snap to the fixed
    // centre; fall back only after it has been gone for a while.
    if (m_gazeHave && ++m_gazeStale <= kGazeStaleMax) {
      lx = m_gazeCache[0]; ly = m_gazeCache[1];
      rx = m_gazeCache[2]; ry = m_gazeCache[3];
      return true;
    }

    m_gazeHave = false;
    return false;
  }

}
