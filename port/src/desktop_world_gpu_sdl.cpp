#include "desktop_world_gpu_sdl.hpp"
#include "desktop_world_renderer.hpp"
#include "desktop_world_shaders_msl.hpp"
#include "desktop_world_shaders_spv.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace phantomcrash::gpu_sdl {
namespace {

SDL_GPUSamplerAddressMode address_mode(const TextureWrap wrap) {
  switch (wrap) {
  case TextureWrap::clamp:
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  case TextureWrap::mirror:
    return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
  default:
    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  }
}

} // namespace

SDL_GPUShader *create_shader(SDL_GPUDevice *const gpu, const bool vertex) {
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(gpu);
  SDL_GPUShaderCreateInfo info{};
  info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = vertex ? 0U : 2U;
  info.num_uniform_buffers = 2U;
  if (formats & SDL_GPU_SHADERFORMAT_MSL) {
    info.code = reinterpret_cast<const Uint8 *>(kWorldShaderMsl);
    info.code_size = std::strlen(kWorldShaderMsl);
    info.entrypoint = vertex ? "vs_main" : "ps_main";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
  } else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
    info.code = reinterpret_cast<const Uint8 *>(vertex ? kWorldVertSpv : kWorldFragSpv);
    info.code_size = vertex ? sizeof(kWorldVertSpv) : sizeof(kWorldFragSpv);
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  } else {
    throw std::runtime_error("No supported SDL_GPU shader format (MSL/SPIR-V)");
  }
  SDL_GPUShader *shader = SDL_CreateGPUShader(gpu, &info);
  if (!shader) {
    throw std::runtime_error(std::string("SDL_CreateGPUShader failed: ") + SDL_GetError());
  }
  return shader;
}

SdlGpuResources::SdlGpuResources(SDL_GPUDevice *const device, const DataRoots &data_roots)
    : gpu(device), roots(data_roots) {
  default_sampler = get_sampler(TextureWrap::repeat, TextureWrap::repeat);
  const std::uint32_t white = 0xFFFFFFFFU;
  white_tex = upload_rgba_texture(&white, 1, 1);
  if (!white_tex || !default_sampler) {
    throw std::runtime_error("Unable to initialize fallback SDL_GPU resources");
  }
}

SdlGpuResources::~SdlGpuResources() {
  if (!gpu) {
    return;
  }
  for (auto &[path, tex] : images) {
    if (tex) {
      SDL_ReleaseGPUTexture(gpu, tex);
    }
  }
  for (auto &[k, samp] : samplers) {
    if (samp) {
      SDL_ReleaseGPUSampler(gpu, samp);
    }
  }
  if (white_tex) {
    SDL_ReleaseGPUTexture(gpu, white_tex);
  }
}

SDL_GPUBuffer *SdlGpuResources::upload_buffer(const void *const data, const std::size_t bytes,
                                              const SDL_GPUBufferUsageFlags usage) {
  if (!data || bytes == 0) {
    return nullptr;
  }
  SDL_GPUBufferCreateInfo binfo{};
  binfo.usage = usage;
  binfo.size = static_cast<Uint32>(bytes);
  SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(gpu, &binfo);
  SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, binfo.size, 0};
  SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu, &tinfo);
  if (!buf || !xfer) {
    if (buf) SDL_ReleaseGPUBuffer(gpu, buf);
    if (xfer) SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    throw std::runtime_error(std::string("Unable to allocate GPU buffer: ") + SDL_GetError());
  }
  void *mapped = SDL_MapGPUTransferBuffer(gpu, xfer, false);
  if (!mapped) {
    SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    SDL_ReleaseGPUBuffer(gpu, buf);
    throw std::runtime_error(std::string("Unable to map GPU transfer buffer: ") + SDL_GetError());
  }
  std::memcpy(mapped, data, bytes);
  SDL_UnmapGPUTransferBuffer(gpu, xfer);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
  if (!cmd) {
    SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    SDL_ReleaseGPUBuffer(gpu, buf);
    throw std::runtime_error(std::string("Unable to acquire GPU command buffer: ") + SDL_GetError());
  }
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTransferBufferLocation src{xfer, 0};
  SDL_GPUBufferRegion dst{buf, 0, binfo.size};
  SDL_UploadToGPUBuffer(copy, &src, &dst, false);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(gpu, xfer);
  return buf;
}

SDL_GPUTexture *SdlGpuResources::upload_rgba_texture(const void *const pixels, const int w,
                                                     const int h) {
  if (!pixels || w <= 0 || h <= 0) {
    return nullptr;
  }
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.width = static_cast<Uint32>(w);
  info.height = static_cast<Uint32>(h);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  SDL_GPUTexture *tex = SDL_CreateGPUTexture(gpu, &info);
  const Uint32 bytes = static_cast<Uint32>(w) * static_cast<Uint32>(h) * 4;
  SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
  SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu, &tinfo);
  if (!tex || !xfer) {
    if (tex) SDL_ReleaseGPUTexture(gpu, tex);
    if (xfer) SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    return nullptr;
  }
  void *mapped = SDL_MapGPUTransferBuffer(gpu, xfer, false);
  if (!mapped) {
    SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    SDL_ReleaseGPUTexture(gpu, tex);
    return nullptr;
  }
  std::memcpy(mapped, pixels, bytes);
  SDL_UnmapGPUTransferBuffer(gpu, xfer);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
  if (!cmd) {
    SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    SDL_ReleaseGPUTexture(gpu, tex);
    return nullptr;
  }
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src{xfer, 0, static_cast<Uint32>(w), static_cast<Uint32>(h)};
  SDL_GPUTextureRegion dst{tex, 0, 0, 0, 0, 0, info.width, info.height, 1};
  SDL_UploadToGPUTexture(copy, &src, &dst, false);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(gpu, xfer);
  return tex;
}

SDL_GPUTexture *SdlGpuResources::load_image(const std::string &asset_path) {
  if (asset_path.empty()) {
    return nullptr;
  }
  if (const auto it = images.find(asset_path); it != images.end()) {
    return it->second;
  }
  SDL_GPUTexture *tex = nullptr;
  try {
    const auto file = roots.asset(asset_path);
    if (SDL_Surface *raw = IMG_Load(file.string().c_str())) {
      SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(raw);
      if (rgba) {
        tex = upload_rgba_texture(rgba->pixels, rgba->w, rgba->h);
        SDL_DestroySurface(rgba);
      }
    }
  } catch (...) {
    tex = nullptr;
  }
  images[asset_path] = tex;
  return tex;
}

SDL_GPUSampler *SdlGpuResources::get_sampler(const TextureWrap u, const TextureWrap v) {
  const auto key = std::make_pair(u, v);
  if (const auto it = samplers.find(key); it != samplers.end()) {
    return it->second;
  }
  SDL_GPUSamplerCreateInfo s{};
  s.min_filter = SDL_GPU_FILTER_LINEAR;
  s.mag_filter = SDL_GPU_FILTER_LINEAR;
  s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  s.address_mode_u = address_mode(u);
  s.address_mode_v = address_mode(v);
  s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  SDL_GPUSampler *samp = SDL_CreateGPUSampler(gpu, &s);
  samplers[key] = samp;
  return samp;
}

SDL_GPUGraphicsPipeline *create_pipeline(SDL_GPUDevice *const gpu, SDL_GPUShader *const vert,
                                         SDL_GPUShader *const frag,
                                         const SDL_GPUTextureFormat color_fmt,
                                         const SDL_GPUTextureFormat depth_fmt,
                                         const int blend_mode, const bool double_sided,
                                         const bool reflected, const bool depth_test,
                                         const bool depth_write) {
  const std::array<SDL_GPUVertexBufferDescription, 1> vbufs{{{
      0, static_cast<Uint32>(sizeof(ModelVertex)), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}}};
  const std::array<SDL_GPUVertexAttribute, 5> attrs{{
      {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
      {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
      {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 24},
      {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 40},
      {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 44},
  }};
  SDL_GPUColorTargetDescription color{};
  color.format = color_fmt;
  auto &bs = color.blend_state;
  bs.enable_color_write_mask = false;
  bs.enable_blend = (blend_mode != 0);
  if (blend_mode == 1) {
    bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
    bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  } else if (blend_mode == 2) {
    bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
    bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  }
  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = vert;
  info.fragment_shader = frag;
  info.vertex_input_state = {vbufs.data(), 1, attrs.data(), static_cast<Uint32>(attrs.size())};
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = double_sided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
  info.rasterizer_state.front_face =
      reflected ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
  info.rasterizer_state.enable_depth_clip = true;
  info.depth_stencil_state.compare_op =
      depth_test ? SDL_GPU_COMPAREOP_LESS_OR_EQUAL : SDL_GPU_COMPAREOP_ALWAYS;
  info.depth_stencil_state.enable_depth_test = depth_test;
  info.depth_stencil_state.enable_depth_write = depth_write;
  info.target_info.color_target_descriptions = &color;
  info.target_info.num_color_targets = 1;
  info.target_info.depth_stencil_format = depth_fmt;
  info.target_info.has_depth_stencil_target = true;
  SDL_GPUGraphicsPipeline *pipe = SDL_CreateGPUGraphicsPipeline(gpu, &info);
  if (!pipe) {
    throw std::runtime_error(std::string("SDL_CreateGPUGraphicsPipeline failed: ") + SDL_GetError());
  }
  return pipe;
}

void collect_item_draws(const WorldItem &item,
                        const std::vector<std::unique_ptr<SdlGpuModel>> &models,
                        const std::vector<SDL_GPUTexture *> &textures, const Vec3 eye,
                        std::vector<GpuDraw> &out) {
  if (item.model < 0 || item.model >= static_cast<int>(models.size()) || !models[item.model]) {
    return;
  }
  const SdlGpuModel &model = *models[static_cast<std::size_t>(item.model)];
  const bool animated = item.node_worlds.size() == model.data.nodes.size();
  SDL_GPUTexture *paint =
      (item.paint >= 0 && item.paint < static_cast<int>(textures.size()))
          ? textures[static_cast<std::size_t>(item.paint)]
          : nullptr;
  const Mat4 stretch = scaling({item.scale_x, 1.0F, 1.0F});
  for (const ModelDraw &draw : model.data.draws) {
    if (item.node >= 0 && draw.node != item.node) continue;
    const auto node_index = static_cast<std::size_t>(draw.node);
    const ModelNode &node = model.data.nodes[node_index];
    if (std::ranges::find(item.shadow_nodes, node.name) != item.shadow_nodes.end()) continue;
    Mat4 world = animated ? item.node_worlds[node_index] : stretch * node.world * item.world;
    auto uv_offset = item.uv_offset;
    const bool sky = std::ranges::find(item.sky_nodes, node.name) != item.sky_nodes.end();
    if (sky) {
      if (const auto m = std::ranges::find(item.sky_motion, node.name, &SkyMotion::node);
          m != item.sky_motion.end()) {
        const float ang =
            static_cast<float>(std::fmod(item.sky_seconds * m->rotation_per_second, 2.0 * pi));
        world = world * rotation_y(ang);
        for (std::size_t a = 0; a < 2; ++a) {
          uv_offset[a] += static_cast<float>(std::fmod(item.sky_seconds * m->uv_per_second[a], 1.0));
        }
      }
    }
    const float dist = length(transform_point(draw.centre, world) - eye);
    const auto &mat = model.data.materials[static_cast<std::size_t>(draw.material)];
    const bool blend = mat.alpha == AlphaMode::blend || mat.additive;
    SDL_GPUTexture *atlas =
        mat.part_paint
            ? ((item.part_paint >= 0 && item.part_paint < static_cast<int>(textures.size()))
                   ? textures[static_cast<std::size_t>(item.part_paint)]
                   : nullptr)
            : paint;
    out.push_back({&model, &draw, world, dist, atlas, item.alpha, item.flash, &item, sky, blend,
                   uv_offset});
  }
}

world_constants::DrawConstants build_draw_constants(const GpuDraw &d, const StageLighting &lighting,
                                                    const bool sky, const bool overlay,
                                                    const bool has_base, const bool has_normal) {
  const ModelMaterial &mat =
      d.model->data.materials[static_cast<std::size_t>(d.draw->material)];
  world_constants::DrawConstants dc{};
  dc.world = d.world;
  dc.base_colour = d.item->colour.value_or(mat.base_colour);
  dc.uv_offset = d.uv_offset;
  dc.alpha_cutoff = mat.alpha_cutoff;
  dc.unlit = mat.unlit ? 1.0F : 0.0F;
  if (!sky && !overlay && !mat.unlit && !d.item->ground_shadow && !d.item->rainbow) {
    const auto src = sample_source_lights(lighting.source_lights, transform_point({}, d.item->world));
    for (std::size_t i = 0; i < src.size(); ++i) {
      dc.source_directions[i] = {src[i].toward.x, src[i].toward.y, src[i].toward.z, 0};
      dc.source_colours[i] = {src[i].colour.x, src[i].colour.y, src[i].colour.z, 0};
    }
  }
  dc.has_base = has_base ? 1.0F : 0.0F;
  dc.has_normal = has_normal ? 1.0F : 0.0F;
  dc.reflect_modes[0] = (mat.reflect_texture >= 0 && has_normal) ? 1.0F : 0.0F;
  dc.reflect_modes[1] = d.item->sheen;
  if (const auto &lbl = d.item->label_colour) {
    dc.label_colour = {(*lbl)[0], (*lbl)[1], (*lbl)[2], 1.0F};
  }
  dc.alpha_test = (mat.alpha == AlphaMode::mask ||
                   (mat.alpha == AlphaMode::blend && mat.blend_depth_write && !mat.additive))
                      ? 1.0F
                      : 0.0F;
  dc.fogged = (!sky && !overlay && d.item->fogged)
                  ? ((mat.additive || d.item->additive) ? 2.0F : 1.0F)
                  : 0.0F;
  dc.item_alpha = d.alpha;
  dc.flash = d.flash;
  if (d.item->rainbow) {
    dc.rainbow = {(*d.item->rainbow)[0], (*d.item->rainbow)[1], 1.0F, 0.0F};
  }
  dc.material_light = {mat.ambient_colour.x, mat.ambient_colour.y, mat.ambient_colour.z,
                       mat.specular_power};
  dc.material_modes = {mat.specular ? 1.0F : 0.0F, mat.texture_add ? 1.0F : 0.0F,
                       mat.texture_alpha ? 1.0F : 0.0F, mat.original_material ? 1.0F : 0.0F};
  return dc;
}

} // namespace phantomcrash::gpu_sdl
