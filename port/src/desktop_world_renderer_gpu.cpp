#include "desktop_world_renderer.hpp"
#include "desktop_world_gpu_sdl.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace phantomcrash {
namespace {

using gpu_sdl::GpuDraw;
using gpu_sdl::SdlGpuModel;
using gpu_sdl::SdlGpuResources;

Mat4 without_translation(Mat4 view) {
  view.at(3, 0) = 0.0F;
  view.at(3, 1) = 0.0F;
  view.at(3, 2) = 0.0F;
  return view;
}

void push_frame(SDL_GPUCommandBuffer *const cmd, const WorldFrame &frame, const Mat4 &view) {
  const StageLighting &light = frame.lighting;
  world_constants::FrameConstants fc{};
  fc.view_projection = view * frame.projection;
  fc.eye = frame.eye;
  fc.fog_start = light.fog_start;
  fc.ambient = light.ambient;
  fc.fog_end = light.fog_end;
  fc.sun_direction = light.sun_direction;
  fc.fog_enabled = light.fog ? 1.0F : 0.0F;
  fc.sun_colour = light.sun_colour;
  fc.shadow_length = frame.shadow_length;
  fc.fog_colour = light.fog_colour;
  SDL_PushGPUVertexUniformData(cmd, 0, &fc, sizeof(fc));
  SDL_PushGPUFragmentUniformData(cmd, 0, &fc, sizeof(fc));
}

} // namespace

struct WorldRenderer::Impl {
  SDL_Renderer *renderer_{};
  const DataRoots &roots_;
  SDL_GPUDevice *gpu_{};
  PostProcessOptions post_process_;
  SDL_GPUShader *vert_shader_{};
  SDL_GPUShader *frag_shader_{};
  std::unique_ptr<SdlGpuResources> resources_;
  SDL_GPUTextureFormat color_format_{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
  SDL_GPUTextureFormat depth_format_{SDL_GPU_TEXTUREFORMAT_D24_UNORM};
  std::vector<std::unique_ptr<SdlGpuModel>> models_;
  std::map<std::string, int> model_ids_;
  std::vector<SDL_GPUTexture *> textures_;
  std::map<std::string, int> texture_ids_;
  std::map<Uint32, SDL_GPUGraphicsPipeline *> pipelines_;
  SDL_Texture *output_{};
  SDL_GPUTexture *output_gpu_{};
  SDL_GPUTexture *depth_{};
  int width_{}, height_{};

  Impl(SDL_Renderer *renderer, const DataRoots &roots) : renderer_(renderer), roots_(roots) {
    gpu_ = SDL_GetGPURendererDevice(renderer_);
    if (!gpu_) {
      SDL_Log("The 3D GPU world backend is inactive for %s renderer", SDL_GetRendererName(renderer_));
      return;
    }
    try {
      depth_format_ = SDL_GPUTextureSupportsFormat(
                          gpu_, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D,
                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                          ? SDL_GPU_TEXTUREFORMAT_D24_UNORM
                          : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
      vert_shader_ = gpu_sdl::create_shader(gpu_, true);
      frag_shader_ = gpu_sdl::create_shader(gpu_, false);
      resources_ = std::make_unique<SdlGpuResources>(gpu_, roots_);
    } catch (const std::exception &err) {
      SDL_Log("The 3D world is unavailable: %s", err.what());
      cleanup();
      gpu_ = nullptr;
    }
  }

  ~Impl() {
    cleanup();
  }

  void cleanup() {
    if (output_) {
      SDL_DestroyTexture(output_);
      output_ = nullptr;
      output_gpu_ = nullptr;
    }
    if (!gpu_) return;
    if (depth_) {
      SDL_ReleaseGPUTexture(gpu_, depth_);
      depth_ = nullptr;
    }
    for (auto &[key, pipe] : pipelines_) SDL_ReleaseGPUGraphicsPipeline(gpu_, pipe);
    pipelines_.clear();
    for (auto &m : models_) {
      if (m->vertices) SDL_ReleaseGPUBuffer(gpu_, m->vertices);
      if (m->indices) SDL_ReleaseGPUBuffer(gpu_, m->indices);
    }
    models_.clear();
    textures_.clear();
    resources_.reset();
    if (vert_shader_) {
      SDL_ReleaseGPUShader(gpu_, vert_shader_);
      vert_shader_ = nullptr;
    }
    if (frag_shader_) {
      SDL_ReleaseGPUShader(gpu_, frag_shader_);
      frag_shader_ = nullptr;
    }
  }

  SDL_GPUGraphicsPipeline *pipeline(int blend_mode, bool double_sided, bool reflected,
                                    bool depth_test, bool depth_write) {
    const Uint32 key = static_cast<Uint32>((blend_mode & 3) | ((double_sided ? 1 : 0) << 2) |
                                           ((reflected ? 1 : 0) << 3) | ((depth_test ? 1 : 0) << 4) |
                                           ((depth_write ? 1 : 0) << 5));
    if (const auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    SDL_GPUGraphicsPipeline *pipe = gpu_sdl::create_pipeline(
        gpu_, vert_shader_, frag_shader_, color_format_, depth_format_, blend_mode, double_sided,
        reflected, depth_test, depth_write);
    pipelines_[key] = pipe;
    return pipe;
  }

  void ensure_targets(const int width, const int height) {
    if (output_ && width == width_ && height == height_) return;
    if (output_) {
      SDL_DestroyTexture(output_);
      output_ = nullptr;
      output_gpu_ = nullptr;
    }
    if (depth_) {
      SDL_ReleaseGPUTexture(gpu_, depth_);
      depth_ = nullptr;
    }
    width_ = width;
    height_ = height;
    output_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                                width, height);
    if (!output_) {
      throw std::runtime_error(std::string("Unable to create world texture: ") + SDL_GetError());
    }
    output_gpu_ = static_cast<SDL_GPUTexture *>(SDL_GetPointerProperty(
        SDL_GetTextureProperties(output_), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    if (!output_gpu_) throw std::runtime_error("World texture has no SDL_GPUTexture backing");
    SDL_GPUTextureCreateInfo dinfo{
        SDL_GPU_TEXTURETYPE_2D, depth_format_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
        static_cast<Uint32>(width), static_cast<Uint32>(height), 1, 1, SDL_GPU_SAMPLECOUNT_1, 0};
    depth_ = SDL_CreateGPUTexture(gpu_, &dinfo);
    if (!depth_) {
      throw std::runtime_error(std::string("Unable to create world depth target: ") +
                               SDL_GetError());
    }
  }

  void draw_one(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const GpuDraw &d,
                const StageLighting &lighting, bool sky, bool overlay) {
    if (!d.model || !d.model->vertices || !d.model->indices) return;
    const SdlGpuModel &model = *d.model;
    const ModelMaterial &mat = model.data.materials[static_cast<std::size_t>(d.draw->material)];
    const bool painted = d.paint && mat.base_texture >= 0 &&
                         (mat.part_paint || d.item->paint_any ||
                          model.paintable[static_cast<std::size_t>(mat.base_texture)]);
    const int second = mat.reflect_texture >= 0 ? mat.reflect_texture : mat.normal_texture;
    SDL_GPUTexture *t0 =
        painted ? d.paint
                : (mat.base_texture >= 0 ? model.textures[static_cast<std::size_t>(mat.base_texture)]
                                         : nullptr);
    SDL_GPUTexture *t1 = second >= 0 ? model.textures[static_cast<std::size_t>(second)] : nullptr;
    SDL_GPUSampler *s0 =
        mat.base_texture >= 0 ? model.samplers[static_cast<std::size_t>(mat.base_texture)]
                              : resources_->default_sampler;
    SDL_GPUSampler *s1 = second >= 0 ? model.samplers[static_cast<std::size_t>(second)]
                                     : resources_->default_sampler;
    const auto dc =
        gpu_sdl::build_draw_constants(d, lighting, sky, overlay, t0 != nullptr, t1 != nullptr);
    const int blend_mode =
        (mat.additive || d.item->additive) ? 2 : (d.blend || d.alpha < 1.0F ? 1 : 0);
    const bool depth_write =
        !sky && (blend_mode == 0 || (mat.blend_depth_write && !mat.additive && d.alpha == 1.0F));
    SDL_GPUGraphicsPipeline *pipe = pipeline(
        blend_mode, mat.double_sided || d.item->double_sided, d.item->reflected, !sky, depth_write);
    if (!pipe) return;
    SDL_BindGPUGraphicsPipeline(pass, pipe);
    SDL_PushGPUVertexUniformData(cmd, 1, &dc, sizeof(dc));
    SDL_PushGPUFragmentUniformData(cmd, 1, &dc, sizeof(dc));
    const std::array<SDL_GPUTextureSamplerBinding, 2> bindings{{
        {t0 ? t0 : resources_->white_tex, s0 ? s0 : resources_->default_sampler},
        {t1 ? t1 : resources_->white_tex, s1 ? s1 : resources_->default_sampler},
    }};
    SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), 2);
    const SDL_GPUBufferBinding vbind{model.vertices, 0};
    const SDL_GPUBufferBinding ibind{model.indices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
    SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(pass, d.draw->index_count, 1, d.draw->first_index, 0, 0);
  }
};

WorldRenderer::WorldRenderer(SDL_Renderer *renderer, const DataRoots &roots)
    : impl_(std::make_unique<Impl>(renderer, roots)) {}

WorldRenderer::~WorldRenderer() = default;

bool WorldRenderer::available() const noexcept {
  return impl_ && impl_->gpu_ != nullptr;
}

void WorldRenderer::set_post_process(const PostProcessOptions &options) {
  if (impl_) impl_->post_process_ = options;
}

int WorldRenderer::model(const std::string &asset_path) {
  if (!available()) return -1;
  if (const auto it = impl_->model_ids_.find(asset_path); it != impl_->model_ids_.end()) {
    return it->second;
  }
  int id = -1;
  try {
    auto gm = std::make_unique<SdlGpuModel>();
    gm->data = load_model(impl_->roots_.asset(asset_path), asset_path);
    if (!gm->data.vertices.empty() && !gm->data.indices.empty()) {
      gm->vertices = impl_->resources_->upload_buffer(
          gm->data.vertices.data(), gm->data.vertices.size() * sizeof(ModelVertex),
          SDL_GPU_BUFFERUSAGE_VERTEX);
      gm->indices = impl_->resources_->upload_buffer(
          gm->data.indices.data(), gm->data.indices.size() * sizeof(std::uint32_t),
          SDL_GPU_BUFFERUSAGE_INDEX);
      for (const ModelTexture &t : gm->data.textures) {
        gm->textures.push_back(impl_->resources_->load_image(t.image));
        gm->samplers.push_back(impl_->resources_->get_sampler(t.wrap_u, t.wrap_v));
        gm->paintable.push_back(t.image == gm->data.textures.front().image);
      }
      id = static_cast<int>(impl_->models_.size());
      impl_->models_.push_back(std::move(gm));
    }
  } catch (const std::exception &err) {
    SDL_Log("%s", err.what());
  }
  impl_->model_ids_[asset_path] = id;
  return id;
}

void WorldRenderer::prepare_shadow(int) {}

const ModelData *WorldRenderer::model_data(const int model) const {
  if (!impl_ || model < 0 || model >= static_cast<int>(impl_->models_.size()) ||
      !impl_->models_[static_cast<std::size_t>(model)]) {
    return nullptr;
  }
  return &impl_->models_[static_cast<std::size_t>(model)]->data;
}

int WorldRenderer::texture(const std::string &asset_path) {
  if (!available() || asset_path.empty()) return -1;
  if (const auto it = impl_->texture_ids_.find(asset_path); it != impl_->texture_ids_.end()) {
    return it->second;
  }
  int id = -1;
  if (SDL_GPUTexture *tex = impl_->resources_->load_image(asset_path)) {
    id = static_cast<int>(impl_->textures_.size());
    impl_->textures_.push_back(tex);
  }
  impl_->texture_ids_[asset_path] = id;
  return id;
}

void WorldRenderer::draw(const WorldFrame &frame, const WorldFrame *) {
  if (!available() || frame.width <= 0 || frame.height <= 0) return;
  try {
    impl_->ensure_targets(frame.width, frame.height);
  } catch (const std::exception &err) {
    SDL_Log("%s", err.what());
    return;
  }
  SDL_FlushRenderer(impl_->renderer_);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(impl_->gpu_);
  if (!cmd) return;
  std::vector<GpuDraw> draws, skies, opaques, blends;
  for (const WorldItem &item : frame.items) {
    gpu_sdl::collect_item_draws(item, impl_->models_, impl_->textures_, frame.eye, draws);
  }
  for (const GpuDraw &d : draws) {
    (d.sky_node ? skies : ((d.blend || d.alpha < 1.0F) ? blends : opaques)).push_back(d);
  }
  std::sort(opaques.begin(), opaques.end(),
            [](const GpuDraw &a, const GpuDraw &b) { return a.distance < b.distance; });
  std::sort(blends.begin(), blends.end(),
            [](const GpuDraw &a, const GpuDraw &b) { return a.distance > b.distance; });
  SDL_GPUColorTargetInfo cinfo{};
  cinfo.texture = impl_->output_gpu_;
  cinfo.clear_color = frame.transparent
                          ? SDL_FColor{0.0F, 0.0F, 0.0F, 0.0F}
                          : SDL_FColor{frame.lighting.clear_colour.x, frame.lighting.clear_colour.y,
                                       frame.lighting.clear_colour.z, 1.0F};
  cinfo.load_op = SDL_GPU_LOADOP_CLEAR;
  cinfo.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPUDepthStencilTargetInfo dinfo{};
  dinfo.texture = impl_->depth_;
  dinfo.clear_depth = 1.0F;
  dinfo.load_op = SDL_GPU_LOADOP_CLEAR;
  dinfo.store_op = SDL_GPU_STOREOP_DONT_CARE;
  dinfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
  dinfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
  const SDL_GPUViewport vp{0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height),
                           0.0F, 1.0F};
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &cinfo, 1, &dinfo);
  if (!pass) {
    SDL_CancelGPUCommandBuffer(cmd);
    return;
  }
  SDL_SetGPUViewport(pass, &vp);
  if (!skies.empty()) {
    push_frame(cmd, frame, without_translation(frame.view));
    for (const GpuDraw &d : skies) impl_->draw_one(cmd, pass, d, frame.lighting, true, false);
  }
  push_frame(cmd, frame, frame.view);
  for (const GpuDraw &d : opaques) impl_->draw_one(cmd, pass, d, frame.lighting, false, false);
  for (const GpuDraw &d : blends) impl_->draw_one(cmd, pass, d, frame.lighting, false, false);
  SDL_EndGPURenderPass(pass);
  if (!frame.overlay.empty()) {
    cinfo.load_op = SDL_GPU_LOADOP_LOAD;
    SDL_GPURenderPass *opass = SDL_BeginGPURenderPass(cmd, &cinfo, 1, &dinfo);
    if (opass) {
      SDL_SetGPUViewport(opass, &vp);
      push_frame(cmd, frame, Mat4{});
      std::vector<GpuDraw> odraws;
      for (const WorldItem &item : frame.overlay) {
        gpu_sdl::collect_item_draws(item, impl_->models_, impl_->textures_, {}, odraws);
      }
      for (const GpuDraw &d : odraws) impl_->draw_one(cmd, opass, d, frame.lighting, false, true);
      SDL_EndGPURenderPass(opass);
    }
  }
  SDL_SubmitGPUCommandBuffer(cmd);
  const SDL_FRect whole{0.0F, 0.0F, static_cast<float>(frame.width), static_cast<float>(frame.height)};
  SDL_SetTextureBlendMode(impl_->output_, frame.transparent ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
  SDL_RenderTexture(impl_->renderer_, impl_->output_, nullptr, &whole);
}

} // namespace phantomcrash
