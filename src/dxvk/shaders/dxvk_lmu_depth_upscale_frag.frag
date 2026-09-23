#version 460

// LMU foveation: upscale the periphery's scaled MSAA depth into a full-size
// MSAA depth image, sample by sample, with bilinear reconstruction.
//
// The game's post chain and its screen-space reflections read the eye depth
// with texelFetch at native integer coordinates. With the eye depth shadowed
// at a reduced scale, texel (x, y) of the shadow is the depth of screen
// (x / scale, y / scale). This pass produces a depth image whose texel (x, y)
// is the depth of screen (x, y).
//
// Reconstruction must be bilinear, not nearest. Nearest gives depth that is
// piecewise constant in 1/scale blocks; a screen-space ray marched across a
// grazing surface (road, grass) then dips under the flat depth inside each
// block and registers false hits, or misses and falls back to the reflection
// probe, with a fixed distance/angle boundary that shows as a seam. Depth
// (z/w) is affine in screen space over a plane, so bilinear reconstruction is
// exact on flat surfaces, which is where reflections matter.

#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform texture2DMS s_depth;

layout(push_constant, scalar)
uniform push_block {
  vec2 p_scale;    // shadow pixels per native pixel, per axis
  vec2 p_src_max;  // last valid shadow texel of the periphery sub-rect
};

void main() {
  // Texel-centre aligned bilinear: the source sample for native pixel centre
  // (x + 0.5) lies at shadow (x + 0.5) * scale, between texel centres.
  vec2 c  = gl_FragCoord.xy * p_scale - 0.5;
  vec2 i0 = floor(c);
  vec2 f  = c - i0;

  ivec2 t00 = ivec2(clamp(i0,               vec2(0.0), p_src_max));
  ivec2 t11 = ivec2(clamp(i0 + vec2(1.0),   vec2(0.0), p_src_max));
  ivec2 t10 = ivec2(t11.x, t00.y);
  ivec2 t01 = ivec2(t00.x, t11.y);

  // gl_SampleID forces per-sample invocation, so each destination sample takes
  // the matching source sample.
  float d00 = texelFetch(s_depth, t00, gl_SampleID).r;
  float d10 = texelFetch(s_depth, t10, gl_SampleID).r;
  float d01 = texelFetch(s_depth, t01, gl_SampleID).r;
  float d11 = texelFetch(s_depth, t11, gl_SampleID).r;

  gl_FragDepth = mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);
}
