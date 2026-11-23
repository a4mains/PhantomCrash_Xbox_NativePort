#pragma once

#include "desktop_world_constants.hpp"
#include "phantomcrash/data_roots.hpp"
#include "phantomcrash/training_ground.hpp"
#include "phantomcrash/world_model.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace phantomcrash {
struct WorldItem;
}

namespace phantomcrash::gpu_sdl {

struct SdlGpuModel {
  ModelData data;
  SDL_GPUBuffer *vertices{};
  SDL_GPUBuffer *indices{};
  std::vector<SDL_GPUTexture *> textures;
  std::vector<SDL_GPUSampler *> samplers;
  std::vector<bool> paintable;
};

struct GpuDraw {
  const SdlGpuModel *model{};
  const ModelDraw *draw{};
  Mat4 world;
  float distance{};
  SDL_GPUTexture *paint{};
  float alpha{1.0F};
  float flash{};
  const WorldItem *item{};
  bool sky_node{};
  bool blend{};
  std::array<float, 2> uv_offset{};
};

struct SdlGpuResources {
  SDL_GPUDevice *gpu{};
  const DataRoots &roots;
  SDL_GPUTexture *white_tex{};
  SDL_GPUSampler *default_sampler{};
  std::map<std::string, SDL_GPUTexture *> images;
  std::map<std::pair<TextureWrap, TextureWrap>, SDL_GPUSampler *> samplers;

  SdlGpuResources(SDL_GPUDevice *device, const DataRoots &data_roots);
  ~SdlGpuResources();
  SdlGpuResources(const SdlGpuResources &) = delete;
  SdlGpuResources &operator=(const SdlGpuResources &) = delete;

  SDL_GPUBuffer *upload_buffer(const void *data, std::size_t bytes, SDL_GPUBufferUsageFlags usage);
  SDL_GPUTexture *upload_rgba_texture(const void *pixels, int w, int h);
  SDL_GPUTexture *load_image(const std::string &asset_path);
  SDL_GPUSampler *get_sampler(TextureWrap u, TextureWrap v);
};

[[nodiscard]] SDL_GPUShader *create_shader(SDL_GPUDevice *gpu, bool vertex);
[[nodiscard]] SDL_GPUGraphicsPipeline *create_pipeline(
    SDL_GPUDevice *gpu, SDL_GPUShader *vert, SDL_GPUShader *frag, SDL_GPUTextureFormat color_fmt,
    SDL_GPUTextureFormat depth_fmt, int blend_mode, bool double_sided, bool reflected,
    bool depth_test, bool depth_write);
void collect_item_draws(const WorldItem &item,
                        const std::vector<std::unique_ptr<SdlGpuModel>> &models,
                        const std::vector<SDL_GPUTexture *> &textures, Vec3 eye,
                        std::vector<GpuDraw> &out);
world_constants::DrawConstants build_draw_constants(const GpuDraw &d, const StageLighting &lighting,
                                                    bool sky, bool overlay, bool has_base,
                                                    bool has_normal);

} // namespace phantomcrash::gpu_sdl
