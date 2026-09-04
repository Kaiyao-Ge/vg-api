#include "backends/metal/metal_device_internal.h"
#include "backends/metal/metal_shader_sources.h"
#include "compiler/shader_sources.h"
#include "ir/sha256.h"
#include <cstring>
#include <utility>

namespace vg::metal {

bool DeviceHal::Impl::ensure_pipeline(const MslModule& compiled_msl, std::string* error,
                    const std::string& function_name) {
  const std::string_view ir_hash = compiled_msl.ir_hash;
  const std::string_view msl_source = compiled_msl.source;
  if (pipeline != nil && cached_ir_hash == ir_hash) return true;
  pipeline = nil;
  library = nil;
  cached_ir_hash.clear();

  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  id<MTLLibrary> new_library = [device newLibraryWithSource:ns_utf8(msl_source)
                                                      options:options
                                                        error:&compile_error];
  if (new_library == nil) {
    if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                              : "unknown MSL compile error";
    return false;
  }
  id<MTLFunction> function = [new_library newFunctionWithName:[NSString stringWithUTF8String:function_name.c_str()]];
  if (function == nil) {
    if (error) *error = "MSL library missing " + function_name + " entry point";
    return false;
  }
  NSError* pipeline_error = nil;
  id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&pipeline_error];
  if (new_pipeline == nil) {
    if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                               : "unknown pipeline creation error";
    return false;
  }
  library = new_library;
  pipeline = new_pipeline;
  cached_ir_hash = ir_hash;
  return true;
}

bool DeviceHal::Impl::ensure_node_pipeline(const MslModule& compiled_msl,
                          id<MTLComputePipelineState>* out_pipeline, std::string* error,
                          const std::string& function_name,
                          bool* cache_hit) {
  const std::string_view ir_hash = compiled_msl.ir_hash;
  const std::string_view msl_source = compiled_msl.source;
  const std::string cache_key = std::string(ir_hash) + "\n" + function_name;
  auto it = node_pipelines.find(cache_key);
  if (it != node_pipelines.end()) {
    if (cache_hit != nullptr) *cache_hit = true;
    *out_pipeline = it->second.second;
    return true;
  }
  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  id<MTLLibrary> new_library = [device newLibraryWithSource:ns_utf8(msl_source)
                                                      options:options
                                                        error:&compile_error];
  if (new_library == nil) {
    if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                              : "unknown per-Node MSL compile error";
    return false;
  }
  id<MTLFunction> function =
      [new_library newFunctionWithName:[NSString stringWithUTF8String:function_name.c_str()]];
  if (function == nil) {
    if (error) *error = "per-Node MSL library missing " + function_name + " entry point";
    return false;
  }
  NSError* pipeline_error = nil;
  id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&pipeline_error];
  if (new_pipeline == nil) {
    if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                               : "unknown per-Node pipeline creation error";
    return false;
  }
  node_pipelines.emplace(cache_key, std::make_pair(new_library, new_pipeline));
  if (cache_hit != nullptr) *cache_hit = false;
  *out_pipeline = new_pipeline;
  return true;
}

id<MTLSamplerState> DeviceHal::Impl::ensure_sampler_state(core::FilterMode filter, core::WrapMode wrap, LodClamp lod) {
  std::array<uint32_t, 4> key{static_cast<uint32_t>(filter), static_cast<uint32_t>(wrap), 0, 0};
  std::memcpy(&key[2], &lod.min, sizeof(float));
  std::memcpy(&key[3], &lod.max, sizeof(float));
  auto it = sampler_cache.find(key);
  if (it != sampler_cache.end()) return it->second;
  MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
  const bool nearest = filter == core::FilterMode::Nearest;
  const MTLSamplerMinMagFilter mtl_filter =
      nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
  descriptor.minFilter = mtl_filter;
  descriptor.magFilter = mtl_filter;
  descriptor.mipFilter = nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
  descriptor.lodMinClamp = lod.min;
  descriptor.lodMaxClamp = lod.max;
  const MTLSamplerAddressMode mtl_wrap =
      wrap == core::WrapMode::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  descriptor.sAddressMode = mtl_wrap;
  descriptor.tAddressMode = mtl_wrap;
  descriptor.rAddressMode = mtl_wrap;
  descriptor.normalizedCoordinates = YES;
  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
  if (sampler != nil) sampler_cache.emplace(key, sampler);
  return sampler;
}

bool DeviceHal::Impl::ensure_task_ring_pipeline(std::string* error) {
  if (task_ring_pipeline != nil) return true;
  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  const std::string source = compiler::task_ring_metal_source();
  id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                      options:options
                                                        error:&compile_error];
  if (new_library == nil) {
    if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                              : "unknown task ring MSL compile error";
    return false;
  }
  id<MTLFunction> function = [new_library newFunctionWithName:@"vg_task_publish"];
  if (function == nil) {
    if (error) *error = "task ring MSL library missing vg_task_publish entry point";
    return false;
  }
  NSError* pipeline_error = nil;
  id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&pipeline_error];
  if (new_pipeline == nil) {
    if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                               : "unknown task ring pipeline creation error";
    return false;
  }
  task_ring_library = new_library;
  task_ring_pipeline = new_pipeline;
  return true;
}

bool DeviceHal::Impl::ensure_cull_compact_pipeline(std::string* error) {
  if (cull_compact_pipeline != nil) return true;
  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  const std::string source = compiler::cull_compact_metal_source();
  id<MTLLibrary> new_library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                      options:options
                                                        error:&compile_error];
  if (new_library == nil) {
    if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                              : "unknown cull/compact MSL compile error";
    return false;
  }
  id<MTLFunction> function = [new_library newFunctionWithName:@"vg_cull_compact"];
  if (function == nil) {
    if (error) *error = "cull/compact MSL library missing vg_cull_compact entry point";
    return false;
  }
  NSError* pipeline_error = nil;
  id<MTLComputePipelineState> new_pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&pipeline_error];
  if (new_pipeline == nil) {
    if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                               : "unknown cull/compact pipeline creation error";
    return false;
  }
  cull_compact_library = new_library;
  cull_compact_pipeline = new_pipeline;
  return true;
}

std::string DeviceHal::Impl::target_identity() const {
  return std::string([[device name] UTF8String]) + "|" +
         [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String] + "|MSL";
}

compiler::PipelineKey DeviceHal::Impl::make_pipeline_key(const ShaderEntry& shader,
                                        std::vector<std::pair<std::string, uint64_t>> constants,
                                        std::vector<uint32_t> attachment_formats,
                                        uint32_t sample_count) const {
  compiler::PipelineKey key;
  key.code_object_hash = ir::sha256_hex(shader.source);
  key.entry = std::string(shader.entry);
  key.function_constants = std::move(constants);
  key.attachment_formats = std::move(attachment_formats);
  key.sample_count = sample_count;
  key.target_identity = target_identity();
  return key;
}

id<MTLLibrary> DeviceHal::Impl::ensure_library(const LibraryText& text, std::string* error) {
  auto it = library_by_hash.find(std::string(text.hash));
  if (it != library_by_hash.end()) return it->second;
  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  id<MTLLibrary> library_object =
      [device newLibraryWithSource:ns_utf8(text.source)
                           options:options
                             error:&compile_error];
  if (library_object == nil) {
    if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                              : "unknown MSL compile error";
    return nil;
  }
  library_by_hash.emplace(std::string(text.hash), library_object);
  return library_object;
}

id<MTLFunction> DeviceHal::Impl::ensure_function(id<MTLLibrary> library_object, const compiler::PipelineKey& key,
                                std::string* error) {
  NSString* name = [NSString stringWithUTF8String:key.entry.c_str()];
  MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
  for (const auto& constant : key.function_constants) {
    // Every function constant this backend specializes on is the MSL `bool`
    // of 06 §6.4; a wider constant would need its own type here rather than
    // being coerced into this one.
    const bool value = constant.second != 0;
    [values setConstantValue:&value
                        type:MTLDataTypeBool
                     atIndex:compiler::kFacetCheckedProfileFunctionConstant];
  }
  NSError* function_error = nil;
  id<MTLFunction> function = [library_object newFunctionWithName:name
                                                  constantValues:values
                                                           error:&function_error];
  if (function == nil && error)
    *error = function_error != nil ? [[function_error localizedDescription] UTF8String]
                                   : "MSL specialization of " + key.entry + " failed";
  return function;
}

bool DeviceHal::Impl::acquire_compute_pipeline(compiler::PipelineClassificationCache& cache,
                              std::unordered_map<uint64_t, id<MTLComputePipelineState>>& objects,
                              const std::string& source, const compiler::PipelineKey& key,
                              const std::string& trigger, id<MTLComputePipelineState>* out,
                              compiler::SpecializationReport* report, std::string* error) {
  const uint64_t digest = key.hash();
  compiler::SpecializationReport local;
  id<MTLComputePipelineState> created = nil;
  const bool ok = cache.acquire(
      key, trigger,
      [&](uint64_t* binary_size, std::string* create_error) {
        id<MTLLibrary> library_object = ensure_library({source, key.code_object_hash}, create_error);
        if (library_object == nil) return false;
        id<MTLFunction> function = ensure_function(library_object, key, create_error);
        if (function == nil) return false;
        NSError* pipeline_error = nil;
        created = [device newComputePipelineStateWithFunction:function error:&pipeline_error];
        if (created == nil) {
          if (create_error)
            *create_error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                  : "unknown compute pipeline creation error";
          return false;
        }
        // Metal exposes no compiled binary size for a pipeline built from
        // source, and 10 §12 forbids writing an unobservable cost as a real
        // number, so it stays 0 rather than becoming a guess.
        *binary_size = 0;
        return true;
      },
      &local, error);
  if (!ok) return false;
  if (created != nil) objects[digest] = created;
  auto it = objects.find(digest);
  if (it == objects.end()) {
    if (error) *error = "pipeline cache reported a hit for a Metal object this device never created";
    return false;
  }
  if (out) *out = it->second;
  if (report) *report = local;
  return true;
}

bool DeviceHal::Impl::acquire_render_pipeline(compiler::PipelineClassificationCache& cache,
                             std::unordered_map<uint64_t, id<MTLRenderPipelineState>>& objects,
                             const std::string& source, const compiler::PipelineKey& key,
                             const std::string& vertex_entry, MTLPixelFormat color_format,
                             MTLPixelFormat depth_format,
                             const std::string& trigger, id<MTLRenderPipelineState>* out,
                             compiler::SpecializationReport* report, std::string* error) {
  const uint64_t digest = key.hash();
  compiler::SpecializationReport local;
  id<MTLRenderPipelineState> created = nil;
  const bool ok = cache.acquire(
      key, trigger,
      [&](uint64_t* binary_size, std::string* create_error) {
        id<MTLLibrary> library_object = ensure_library({source, key.code_object_hash}, create_error);
        if (library_object == nil) return false;
        compiler::PipelineKey vertex_key = key;
        vertex_key.entry = vertex_entry;
        id<MTLFunction> vertex_function = ensure_function(library_object, vertex_key, create_error);
        if (vertex_function == nil) return false;
        id<MTLFunction> fragment_function = ensure_function(library_object, key, create_error);
        if (fragment_function == nil) return false;
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex_function;
        descriptor.fragmentFunction = fragment_function;
        descriptor.colorAttachments[0].pixelFormat = color_format;
        descriptor.depthAttachmentPixelFormat = depth_format;
        descriptor.rasterSampleCount = key.sample_count;
        // No MTLVertexDescriptor on purpose: the vertex stage indexes a
        // `device const VgRasterVertex*` at buffer(0) by [[vertex_id]].
        // Pointer-indexed root data is this project's addressing philosophy
        // (04 §8, 06 §5) and it keeps vertex layout out of the key (06 §7).
        NSError* pipeline_error = nil;
        created = [device newRenderPipelineStateWithDescriptor:descriptor error:&pipeline_error];
        if (created == nil) {
          if (create_error)
            *create_error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                  : "unknown render pipeline creation error";
          return false;
        }
        *binary_size = 0;
        return true;
      },
      &local, error);
  if (!ok) return false;
  if (created != nil) objects[digest] = created;
  auto it = objects.find(digest);
  if (it == objects.end()) {
    if (error) *error = "pipeline cache reported a hit for a Metal object this device never created";
    return false;
  }
  if (out) *out = it->second;
  if (report) *report = local;
  return true;
}

bool DeviceHal::Impl::ensure_depth_stencil_state(const compiler::PipelineKey& key, bool test_enable,
                                bool write_enable, core::DepthCompareOp compare_op,
                                id<MTLDepthStencilState>* out, bool* cache_hit,
                                std::string* error) {
  const uint64_t digest = key.hash();
  auto found = depth_stencil_by_key.find(digest);
  if (found != depth_stencil_by_key.end()) {
    if (out) *out = found->second;
    if (cache_hit) *cache_hit = true;
    return true;
  }
  MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
  // Metal ignores compare/write only if there is no depth attachment.  We
  // bind one on every F4 pass, and keep disabled testing semantically exact:
  // Always plus writes disabled means the attachment is not modified.
  descriptor.depthCompareFunction = test_enable ? to_mtl_compare_function(compare_op)
                                                 : MTLCompareFunctionAlways;
  descriptor.depthWriteEnabled = write_enable;
  id<MTLDepthStencilState> created = [device newDepthStencilStateWithDescriptor:descriptor];
  if (created == nil) {
    if (error) *error = "Metal depth-stencil state creation failed";
    return false;
  }
  depth_stencil_by_key.emplace(digest, created);
  if (out) *out = created;
  if (cache_hit) *cache_hit = false;
  return true;
}

bool DeviceHal::Impl::ensure_sample_facet_pipeline(bool array_dimension, bool checked,
                                  id<MTLComputePipelineState>* out, std::string* error) {
  const std::string source = array_dimension ? compiler::sample_facet_array_metal_source()
                                             : compiler::sample_facet_metal_source();
  const std::string entry = array_dimension ? "vg_sample_facet_array" : "vg_sample_facet";
  std::vector<std::pair<std::string, uint64_t>> constants;
  if (checked) constants.emplace_back("vg_checked_profile", 1);
  const compiler::PipelineKey key = make_pipeline_key({source, entry}, constants, {}, 1);
  return acquire_compute_pipeline(pipeline_cache, compute_pipeline_by_key, source, key,
                                  checked ? "checked-profile facet generation guard (06 §6.4)"
                                          : "fast-native SampleFacet kernel",
                                  out, nullptr, error);
}

bool DeviceHal::Impl::ensure_raster_pipeline(core::PixelFormat format, core::PixelFormat depth_format, bool has_depth,
                            uint32_t sample_count, bool depth_test_enable,
                            bool depth_write_enable, core::DepthCompareOp depth_compare_op,
                            id<MTLRenderPipelineState>* out, id<MTLDepthStencilState>* depth_out,
                            bool* depth_cache_hit, std::string* error,
                            const ir::UserRasterShaderContract* user_shader) {
  const std::string source =
      user_shader != nullptr ? user_shader->source : compiler::raster_facet_metal_source();
  const std::string vertex_entry = user_shader != nullptr ? user_shader->vertex_entry : "vg_raster_vertex";
  const std::string fragment_entry = user_shader != nullptr ? user_shader->fragment_entry : "vg_raster_fragment";
  const compiler::PipelineKey key = make_pipeline_key(
      {source, fragment_entry}, {},
      has_depth ? std::vector<uint32_t>{static_cast<uint32_t>(format), static_cast<uint32_t>(depth_format)}
                : std::vector<uint32_t>{static_cast<uint32_t>(format)}, sample_count);
  // These are immutable MTLDepthStencilState choices.  They intentionally
  // participate in the classified key even though the state object is
  // distinct from MTLRenderPipelineState.
  compiler::PipelineKey keyed = key;
  keyed.raster_state = {{"depth_test_enable", depth_test_enable ? 1u : 0u},
                        {"depth_write_enable", depth_write_enable ? 1u : 0u},
                        {"depth_compare_op", static_cast<uint64_t>(depth_compare_op)}};
  if (!acquire_render_pipeline(pipeline_cache, render_pipeline_by_key, source, keyed, vertex_entry,
                               to_mtl_pixel_format(format),
                               has_depth ? to_mtl_pixel_format(depth_format) : MTLPixelFormatInvalid,
                               user_shader != nullptr
                                   ? "restricted-import user MSL raster shader (ADR-043 Decision #4)"
                                   : "F4 depth raster attachment store",
                               out, nullptr, error))
    return false;
  return !has_depth || ensure_depth_stencil_state(keyed, depth_test_enable, depth_write_enable, depth_compare_op,
                                                  depth_out, depth_cache_hit, error);
}

bool DeviceHal::Impl::ensure_storage_facet_pipelines(std::string* error) {
  if (storage_facet_pipeline != nil && storage_array_facet_pipeline != nil &&
      storage_buffer_facet_pipeline != nil)
    return true;
  NSError* compile_error = nil;
  MTLCompileOptions* options = [MTLCompileOptions new];
  const char* source = storage_facet_metal_source();
  id<MTLLibrary> new_library = [device newLibraryWithSource:@(source)
                                                      options:options
                                                        error:&compile_error];
  if (new_library == nil) {
    if (error) *error = compile_error != nil ? [[compile_error localizedDescription] UTF8String]
                                              : "unknown storage facet MSL compile error";
    return false;
  }
  id<MTLComputePipelineState> new_pipelines[3] = {nil, nil, nil};
  const char* entry_points[3] = {"vg_storage_facet_write", "vg_storage_facet_write_array",
                                 "vg_storage_facet_write_buffer"};
  for (int i = 0; i < 3; ++i) {
    id<MTLFunction> function = [new_library newFunctionWithName:@(entry_points[i])];
    if (function == nil) {
      if (error) *error = std::string("storage facet MSL library missing ") + entry_points[i] + " entry point";
      return false;
    }
    NSError* pipeline_error = nil;
    new_pipelines[i] = [device newComputePipelineStateWithFunction:function error:&pipeline_error];
    if (new_pipelines[i] == nil) {
      if (error) *error = pipeline_error != nil ? [[pipeline_error localizedDescription] UTF8String]
                                                 : "unknown storage facet pipeline creation error";
      return false;
    }
  }
  storage_facet_library = new_library;
  storage_facet_pipeline = new_pipelines[0];
  storage_array_facet_pipeline = new_pipelines[1];
  storage_buffer_facet_pipeline = new_pipelines[2];
  return true;
}

}  // namespace vg::metal
