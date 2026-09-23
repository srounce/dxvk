#version 460

// LMU foveation: composite the focus layer over the already-upscaled periphery
// with a feathered edge. Drawn over the VISIBLE part of the focus rectangle;
// the sample coordinate and the feather ramp are computed from the fragment's
// position within the FULL (unclipped) focus rectangle, so when the focus is
// near a screen edge the on-screen part still lands exactly where it belongs
// and the off-screen part is simply not drawn.

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_texture;

layout(location = 0) out vec4 o_color;

layout(push_constant, scalar)
uniform push_block {
  uint  p_sampler;
  float p_feather;   // ramp width as a fraction of the rectangle (0..0.5)
  vec2  p_origin;    // full focus rect origin, framebuffer pixels
  vec2  p_size;      // full focus rect size, framebuffer pixels
  vec2  p_srcScale;  // fraction of the source layer the focus view rendered into
};

void main() {
  vec2 uv = (gl_FragCoord.xy - p_origin) / p_size;
  // The focus view occupies only the top-left p_srcScale of its layer, at
  // native density, so the rect coordinate maps onto that sub-rectangle 1:1.
  vec2 tuv = clamp(uv, 0.0, 1.0) * p_srcScale;
  vec3 c = texture(sampler2D(s_texture, s_samplers[p_sampler]), tuv).rgb;

  float f = max(p_feather, 1.0e-4);
  float ax = smoothstep(0.0, f, uv.x) * smoothstep(0.0, f, 1.0 - uv.x);
  float ay = smoothstep(0.0, f, uv.y) * smoothstep(0.0, f, 1.0 - uv.y);

  o_color = vec4(c, ax * ay);
}
