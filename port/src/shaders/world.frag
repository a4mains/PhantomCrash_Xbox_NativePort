#version 450

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec4 in_colour;
layout(location = 4) in vec2 in_uv;
layout(location = 5) in float in_fog_depth;

layout(set = 2, binding = 0) uniform sampler2D base_texture;
layout(set = 2, binding = 1) uniform sampler2D normal_texture;

layout(set = 3, binding = 0) uniform FrameUniforms {
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

layout(set = 3, binding = 1) uniform DrawUniforms {
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

layout(location = 0) out vec4 out_colour;

void main() {
  bool labelled = draw_u.label_colour.w > 0.5 && draw_u.has_base > 0.5;
  vec3 tint = draw_u.base_colour.rgb;
  if (labelled) {
    tint = mix(draw_u.label_colour.rgb, tint, texture(base_texture, in_uv).a);
  }
  vec4 colour = vec4(in_colour.rgb * tint, in_colour.a * draw_u.base_colour.a);
  vec3 highlight = vec3(0.0);
  vec3 n = normalize(in_normal) * (gl_FrontFacing ? 1.0 : -1.0);

  if ((draw_u.unlit < 0.5 || draw_u.local_light > 0.5) && draw_u.rainbow.z < 0.5) {
    if (draw_u.has_normal > 0.5 && draw_u.reflect_modes.x < 0.5) {
      vec3 t = in_tangent.xyz - n * dot(n, in_tangent.xyz);
      t = normalize(t + 1e-6);
      vec3 b = cross(n, t) * in_tangent.w;
      vec3 m = texture(normal_texture, in_uv).xyz * 2.0 - 1.0;
      n = normalize(t * m.x + b * m.y + n * m.z);
    }
  }

  if (draw_u.unlit < 0.5 && draw_u.rainbow.z < 0.5) {
    float diffuse = clamp(dot(n, -frame_u.sun_direction), 0.0, 1.0);
    vec3 source_light = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
      source_light +=
          draw_u.source_colours[i].rgb * clamp(dot(n, draw_u.source_directions[i].xyz), 0.0, 1.0);
    }
    colour.rgb *= clamp(
        frame_u.ambient * draw_u.material_light.rgb + frame_u.sun_colour * diffuse + source_light,
        0.0, 1.0);
    if (draw_u.material_modes.x > 0.5 && diffuse > 0.0) {
      vec3 toward_eye = normalize(frame_u.eye - in_world_pos);
      vec3 halfway = normalize(toward_eye - frame_u.sun_direction);
      float spec = pow(clamp(dot(n, halfway), 0.0, 1.0), max(draw_u.material_light.w, 1e-4));
      highlight = frame_u.sun_colour * spec;
    }
  }

  if (draw_u.reflect_modes.y > 0.0) {
    vec3 toward_eye = normalize(frame_u.eye - in_world_pos);
    float rim = pow(1.0 - clamp(dot(n, toward_eye), 0.0, 1.0), 2.0);
    float glint = pow(clamp(dot(n, normalize(toward_eye - frame_u.sun_direction)), 0.0, 1.0), 24.0);
    highlight += draw_u.reflect_modes.y * draw_u.base_colour.rgb * (rim * 0.6 + glint * frame_u.sun_colour);
  }

  if (draw_u.has_base > 0.5) {
    vec4 texel = texture(base_texture, in_uv);
    if (draw_u.reflect_modes.x > 0.5) {
      vec3 shine = texture(normal_texture, n.xz).rgb;
      texel.rgb = clamp(texel.rgb + shine, 0.0, 1.0);
    }
    if (draw_u.rainbow.z > 0.5) {
      colour.a *= texel.a;
    } else {
      colour.rgb = draw_u.material_modes.y > 0.5 ? clamp(colour.rgb + texel.rgb, 0.0, 1.0)
                                                 : colour.rgb * texel.rgb;
      if (draw_u.material_modes.z > 0.5 && !labelled) {
        colour.a = draw_u.material_modes.w > 0.5 ? texel.a * draw_u.base_colour.a
                                                 : colour.a * texel.a;
      }
    }
  }

  if (draw_u.alpha_test > 0.5 && colour.a < draw_u.alpha_cutoff) {
    discard;
  }

  colour.rgb += highlight;
  if (frame_u.fog_enabled > 0.5 && draw_u.fogged > 0.5) {
    float amount = clamp(
        (in_fog_depth - frame_u.fog_start) / max(frame_u.fog_end - frame_u.fog_start, 1e-3),
        0.0, 1.0);
    colour.rgb = draw_u.fogged > 1.5 ? colour.rgb * (1.0 - amount)
                                     : mix(colour.rgb, frame_u.fog_colour, amount);
  }
  colour.rgb = mix(colour.rgb, vec3(1.0), draw_u.flash * 0.6);
  colour.a *= draw_u.item_alpha;
  out_colour = colour;
}
