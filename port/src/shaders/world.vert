#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec4 in_colour;
layout(location = 4) in vec2 in_uv;

layout(set = 1, binding = 0) uniform FrameUniforms {
  mat4 view_projection;
  vec3 eye;
  float fog_start;
  vec3 ambient;
  float fog_end;
  vec3 sun_direction;
  float fog_enabled;
  vec3 sun_colour;
  float shadow_length;
  vec3 fog_colour;
  float frame_pad1;
} frame_u;

layout(set = 1, binding = 1) uniform DrawUniforms {
  mat4 world;
  vec4 base_colour;
  float alpha_cutoff;
  float unlit;
  float has_base;
  float has_normal;
  float alpha_test;
  float fogged;
  float item_alpha;
  float flash;
  vec2 uv_offset;
  float optical;
  float local_light;
  vec4 optical_sphere;
  vec4 optical_shape;
  vec4 uv_transform;
  vec4 rainbow;
  vec4 material_light;
  vec4 material_modes;
  vec4 source_directions[2];
  vec4 source_colours[2];
  vec4 reflect_modes;
  vec4 label_colour;
} draw_u;

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec4 out_tangent;
layout(location = 3) out vec4 out_colour;
layout(location = 4) out vec2 out_uv;
layout(location = 5) out float out_fog_depth;

void main() {
  vec4 placed = draw_u.world * vec4(in_position, 1.0);
  vec4 clip = frame_u.view_projection * placed;
  gl_Position = clip;
  out_fog_depth = clip.z;
  out_world_pos = placed.xyz;

  mat3 axes = mat3(draw_u.world[0].xyz, draw_u.world[1].xyz, draw_u.world[2].xyz);
  mat3 cofactor = mat3(
      cross(axes[1], axes[2]),
      cross(axes[2], axes[0]),
      cross(axes[0], axes[1]));
  float handedness = dot(axes[0], cofactor[0]) < 0.0 ? -1.0 : 1.0;
  out_normal = (cofactor * in_normal) * handedness;
  out_tangent = vec4(axes * in_tangent.xyz, in_tangent.w * handedness);
  out_colour = in_colour;
  out_uv = in_uv * draw_u.uv_transform.xy + draw_u.uv_transform.zw + draw_u.uv_offset;
  if (draw_u.rainbow.z > 0.5) {
    out_uv = vec2(in_uv.x, in_uv.y * 0.25 + draw_u.rainbow.y);
  }
}
