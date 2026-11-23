#pragma once

namespace phantomcrash {

inline constexpr char kWorldShaderMsl[] = R"msl(
#include <metal_stdlib>
using namespace metal;

struct FrameUniforms {
  float4x4 view_projection;
  float3 eye;
  float fog_start;
  float3 ambient;
  float fog_end;
  float3 sun_direction;
  float fog_enabled;
  float3 sun_colour;
  float shadow_length;
  float3 fog_colour;
  float frame_pad1;
};

struct DrawUniforms {
  float4x4 world;
  float4 base_colour;
  float alpha_cutoff;
  float unlit;
  float has_base;
  float has_normal;
  float alpha_test;
  float fogged;
  float item_alpha;
  float flash;
  float2 uv_offset;
  float optical;
  float local_light;
  float4 optical_sphere;
  float4 optical_shape;
  float4 uv_transform;
  float4 rainbow;
  float4 material_light;
  float4 material_modes;
  float4 source_directions[2];
  float4 source_colours[2];
  float4 reflect_modes;
  float4 label_colour;
};

struct VertexIn {
  float3 position [[attribute(0)]];
  float3 normal [[attribute(1)]];
  float4 tangent [[attribute(2)]];
  float4 colour [[attribute(3)]];
  float2 uv [[attribute(4)]];
};

struct VertexOut {
  float4 position [[position]];
  float3 world_pos;
  float3 normal;
  float4 tangent;
  float4 colour;
  float2 uv;
  float fog_depth;
};

vertex VertexOut vs_main(
    VertexIn in [[stage_in]],
    constant FrameUniforms &frame_u [[buffer(0)]],
    constant DrawUniforms &draw_u [[buffer(1)]]) {
  VertexOut out;
  float4 placed = draw_u.world * float4(in.position, 1.0);
  float4 clip = frame_u.view_projection * placed;
  out.position = clip;
  out.fog_depth = clip.z;
  out.world_pos = placed.xyz;

  float3x3 axes = float3x3(draw_u.world[0].xyz, draw_u.world[1].xyz, draw_u.world[2].xyz);
  float3x3 cofactor = float3x3(
      cross(axes[1], axes[2]),
      cross(axes[2], axes[0]),
      cross(axes[0], axes[1]));
  float handedness = dot(axes[0], cofactor[0]) < 0.0 ? -1.0 : 1.0;
  out.normal = (cofactor * in.normal) * handedness;
  out.tangent = float4(axes * in.tangent.xyz, in.tangent.w * handedness);
  out.colour = in.colour;
  out.uv = in.uv * draw_u.uv_transform.xy + draw_u.uv_transform.zw + draw_u.uv_offset;
  if (draw_u.rainbow.z > 0.5) {
    out.uv = float2(in.uv.x, in.uv.y * 0.25 + draw_u.rainbow.y);
  }
  return out;
}

fragment float4 ps_main(
    VertexOut in [[stage_in]],
    bool front_facing [[front_facing]],
    texture2d<float> base_texture [[texture(0)]],
    texture2d<float> normal_texture [[texture(1)]],
    sampler base_sampler [[sampler(0)]],
    sampler normal_sampler [[sampler(1)]],
    constant FrameUniforms &frame_u [[buffer(0)]],
    constant DrawUniforms &draw_u [[buffer(1)]]) {
  bool labelled = draw_u.label_colour.w > 0.5 && draw_u.has_base > 0.5;
  float3 tint = draw_u.base_colour.rgb;
  if (labelled) {
    tint = mix(draw_u.label_colour.rgb, tint, base_texture.sample(base_sampler, in.uv).a);
  }
  float4 colour = float4(in.colour.rgb * tint, in.colour.a * draw_u.base_colour.a);
  float3 highlight = float3(0.0);
  float3 n = normalize(in.normal) * (front_facing ? 1.0 : -1.0);

  if ((draw_u.unlit < 0.5 || draw_u.local_light > 0.5) && draw_u.rainbow.z < 0.5) {
    if (draw_u.has_normal > 0.5 && draw_u.reflect_modes.x < 0.5) {
      float3 t = in.tangent.xyz - n * dot(n, in.tangent.xyz);
      t = normalize(t + 1e-6);
      float3 b = cross(n, t) * in.tangent.w;
      float3 m = normal_texture.sample(normal_sampler, in.uv).xyz * 2.0 - 1.0;
      n = normalize(t * m.x + b * m.y + n * m.z);
    }
  }

  if (draw_u.unlit < 0.5 && draw_u.rainbow.z < 0.5) {
    float diffuse = saturate(dot(n, -frame_u.sun_direction));
    float3 source_light = float3(0.0);
    for (int i = 0; i < 2; ++i) {
      source_light +=
          draw_u.source_colours[i].rgb * saturate(dot(n, draw_u.source_directions[i].xyz));
    }
    colour.rgb *= saturate(
        frame_u.ambient * draw_u.material_light.rgb + frame_u.sun_colour * diffuse + source_light);
    if (draw_u.material_modes.x > 0.5 && diffuse > 0.0) {
      float3 toward_eye = normalize(frame_u.eye - in.world_pos);
      float3 halfway = normalize(toward_eye - frame_u.sun_direction);
      float spec = pow(saturate(dot(n, halfway)), max(draw_u.material_light.w, 1e-4));
      highlight = frame_u.sun_colour * spec;
    }
  }

  if (draw_u.reflect_modes.y > 0.0) {
    float3 toward_eye = normalize(frame_u.eye - in.world_pos);
    float rim = pow(1.0 - saturate(dot(n, toward_eye)), 2.0);
    float glint = pow(saturate(dot(n, normalize(toward_eye - frame_u.sun_direction))), 24.0);
    highlight += draw_u.reflect_modes.y * draw_u.base_colour.rgb * (rim * 0.6 + glint * frame_u.sun_colour);
  }

  if (draw_u.has_base > 0.5) {
    float4 texel = base_texture.sample(base_sampler, in.uv);
    if (draw_u.reflect_modes.x > 0.5) {
      float3 shine = normal_texture.sample(normal_sampler, n.xz).rgb;
      texel.rgb = saturate(texel.rgb + shine);
    }
    if (draw_u.rainbow.z > 0.5) {
      colour.a *= texel.a;
    } else {
      colour.rgb = draw_u.material_modes.y > 0.5 ? saturate(colour.rgb + texel.rgb)
                                                 : colour.rgb * texel.rgb;
      if (draw_u.material_modes.z > 0.5 && !labelled) {
        colour.a = draw_u.material_modes.w > 0.5 ? texel.a * draw_u.base_colour.a
                                                 : colour.a * texel.a;
      }
    }
  }

  if (draw_u.alpha_test > 0.5 && colour.a < draw_u.alpha_cutoff) {
    discard_fragment();
  }

  colour.rgb += highlight;
  if (frame_u.fog_enabled > 0.5 && draw_u.fogged > 0.5) {
    float amount = saturate(
        (in.fog_depth - frame_u.fog_start) / max(frame_u.fog_end - frame_u.fog_start, 1e-3));
    colour.rgb = draw_u.fogged > 1.5 ? colour.rgb * (1.0 - amount)
                                     : mix(colour.rgb, frame_u.fog_colour, amount);
  }
  colour.rgb = mix(colour.rgb, float3(1.0), draw_u.flash * 0.6);
  colour.a *= draw_u.item_alpha;
  return colour;
}
)msl";

} // namespace phantomcrash
