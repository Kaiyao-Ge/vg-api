/*
 * Public-ABI F3--F5 legacy raster sample (not an F6 SceneRoot sample):
 *   ./vg-offscreen-triangle-ppm /tmp/offscreen-triangle.ppm
 *
 * It intentionally remains on the v1.6 source-facet path as a compatibility
 * regression.  tests/api/vg_f6_scene_root.c is the v1.7 SceneRoot example.
 */
#include <vg/vg.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_WIDTH 128u
#define IMAGE_HEIGHT 128u
#define TRY(call) do { \
  if ((call) != VG_SUCCESS) { \
    fprintf(stderr, "%s: %s\n", #call, vgGetLastDiagnostic()); \
    return 1; \
  } \
} while (0)

static void canonical_view(VgCanonicalViewDesc* desc, uint64_t allocation,
                           uint32_t generation, uint32_t format,
                           uint32_t width, uint32_t height) {
  memset(desc, 0, sizeof(*desc));
  desc->header.type = VG_STRUCTURE_CANONICAL_VIEW_DESC;
  desc->header.size = sizeof(*desc);
  desc->allocation = allocation;
  desc->allocation_generation = generation;
  desc->format = format;
  desc->dimension = VG_VIEW_DIMENSION_TEXTURE_2D;
  desc->width = width;
  desc->height = height;
  desc->array_layers = 1;
  desc->mip_levels = 1;
  desc->swizzle_red = VG_SWIZZLE_RED;
  desc->swizzle_green = VG_SWIZZLE_GREEN;
  desc->swizzle_blue = VG_SWIZZLE_BLUE;
  desc->swizzle_alpha = VG_SWIZZLE_ALPHA;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s OUTPUT.ppm\n", argv[0]);
    return 1;
  }
  const char* output_path = argv[1];
  VgApi api = {0};
  api.size = sizeof(api);
  TRY(vgGetApi(VG_API_VERSION_1_6, &api));

  VgRuntimeDesc runtime_desc = VG_INIT_STRUCT(VgRuntimeDesc, VG_STRUCTURE_RUNTIME_DESC);
  VgRuntime runtime = 0;
  TRY(api.createRuntime(&runtime_desc, &runtime));
  uint32_t adapter_count = 0;
  TRY(api.enumerateAdapters(runtime, &adapter_count, 0));
  if (adapter_count == 0 || adapter_count > 8) {
    fprintf(stderr, "no usable VG adapter\n");
    return 1;
  }
  VgAdapterInfo adapters[8];
  memset(adapters, 0, sizeof(adapters));
  for (uint32_t i = 0; i < adapter_count; ++i) {
    adapters[i].header.type = VG_STRUCTURE_ADAPTER_INFO;
    adapters[i].header.size = sizeof(VgAdapterInfo);
  }
  TRY(api.enumerateAdapters(runtime, &adapter_count, adapters));
  uint32_t selected = 0;
  for (uint32_t i = 0; i < adapter_count; ++i) {
    if (adapters[i].backend_kind == VG_BACKEND_METAL) {
      selected = i;
      break;
    }
  }

  VgAdapter adapter = 0;
  VgDevice device = 0;
  VgAddressDomain domain = 0;
  VgArena arena = 0;
  VgAllocation source = 0, target = 0, vertices = 0;
  VgCodeObject code = 0;
  VgNode node = 0;
  VgTaskGraphBuilder builder = 0;
  VgTaskGraph graph = 0;
  VgExecutionEnvelope envelope = 0;
  VgSubmission submission = 0;
  int result = 1;

  VgDeviceDesc device_desc = VG_INIT_STRUCT(VgDeviceDesc, VG_STRUCTURE_DEVICE_DESC);
  VgAddressDomainDesc domain_desc = VG_INIT_STRUCT(VgAddressDomainDesc, VG_STRUCTURE_ADDRESS_DOMAIN_DESC);
  VgArenaDesc arena_desc = VG_INIT_STRUCT(VgArenaDesc, VG_STRUCTURE_ARENA_DESC);
  domain_desc.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
  TRY(api.openAdapter(runtime, adapters[selected].stable_uuid, &adapter));
  TRY(api.createDevice(adapter, &device_desc, &device));
  TRY(api.createAddressDomain(device, &domain_desc, &domain));
  arena_desc.domain = domain;
  TRY(api.createArena(device, &arena_desc, &arena));
  TRY(api.arenaAllocate(arena, 2u * 2u * 4u, &source));
  TRY(api.arenaAllocate(arena, IMAGE_WIDTH * IMAGE_HEIGHT * 4u, &target));
  TRY(api.arenaAllocate(arena, 3u * 5u * sizeof(float), &vertices));

  uint64_t source_id, target_id, vertex_id;
  uint32_t source_generation, target_generation, vertex_generation;
  TRY(api.getAllocationRef(source, &source_id, &source_generation));
  TRY(api.getAllocationRef(target, &target_id, &target_generation));
  TRY(api.getAllocationRef(vertices, &vertex_id, &vertex_generation));
  const uint8_t checker[] = {
    255,  64,  32, 255, 255, 220,  32, 255,
     32, 100, 255, 255,  32, 255, 160, 255,
  };
  /* packed {x, y, z, u, v}, the public vg.msl.raster/v1 vertex contract. */
  const float triangle[] = {
    -0.86f, -0.78f, 0.50f, 0.0f, 1.0f,
     0.86f, -0.78f, 0.50f, 1.0f, 1.0f,
     0.00f,  0.86f, 0.50f, 0.5f, 0.0f,
  };
  TRY(api.writeAllocation(arena, source, 0, checker, sizeof(checker)));
  TRY(api.writeAllocation(arena, vertices, 0, triangle, sizeof(triangle)));

  VgCanonicalViewDesc view;
  VgFacetRef source_facet, target_facet, vertex_facet;
  canonical_view(&view, source_id, source_generation, VG_PIXEL_FORMAT_RGBA8_UNORM, 2, 2);
  TRY(api.acquireFacet(device, arena, &view, VG_FACET_KIND_SAMPLE, &source_facet));
  canonical_view(&view, target_id, target_generation, VG_PIXEL_FORMAT_RGBA8_UNORM, IMAGE_WIDTH, IMAGE_HEIGHT);
  TRY(api.acquireFacet(device, arena, &view, VG_FACET_KIND_ATTACHMENT, &target_facet));
  canonical_view(&view, vertex_id, vertex_generation, VG_PIXEL_FORMAT_RGBA8_UNORM, 15, 1);
  TRY(api.acquireFacet(device, arena, &view, VG_FACET_KIND_ADDRESS, &vertex_facet));

  char ir[512];
  int ir_size = snprintf(ir, sizeof(ir),
    "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.example.offscreen-triangle/v1\",\"instructions\":[{\"op\":\"load\",\"allocation\":%llu,\"generation\":%u,\"offset\":0,\"size\":4}],\"effects\":[{\"allocation\":%llu,\"offset\":0,\"size\":4,\"access\":\"read\",\"representation_epoch\":0}]}",
    (unsigned long long)source_id, source_generation, (unsigned long long)source_id);
  if (ir_size < 0 || (size_t)ir_size >= sizeof(ir)) {
    fprintf(stderr, "IR serialization failed\n");
    goto cleanup;
  }
  VgCodeObjectDesc code_desc = VG_INIT_STRUCT(VgCodeObjectDesc, VG_STRUCTURE_CODE_OBJECT_DESC);
  code_desc.bytes = ir;
  code_desc.byte_size = (uint64_t)ir_size;
  code_desc.format_tag = "vg.ir/v1";
  TRY(api.loadCodeObject(device, &code_desc, &code));
  VgNodeDesc node_desc = VG_INIT_STRUCT(VgNodeDesc, VG_STRUCTURE_NODE_DESC);
  node_desc.entry_name = "offscreen_triangle";
  TRY(api.createNode(code, &node_desc, &node));
  VgNodeRef node_ref;
  TRY(api.getNodeRef(node, &node_ref));
  VgTaskGraphBuilderDesc builder_desc = VG_INIT_STRUCT(VgTaskGraphBuilderDesc, VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC);
  builder_desc.code_object = code;
  TRY(api.createTaskGraphBuilder(device, &builder_desc, &builder));

  VgTaskRecordV2 task;
  memset(&task, 0, sizeof(task));
  task.node = node_ref;
  task.root_generation = 1;
  task.shape.x = task.shape.y = task.shape.z = 1;
  task.kind = VG_TASK_KIND_RASTER;
  task.topology = VG_TOPOLOGY_TRIANGLE_LIST;
  task.raster_facets.source = source_facet;
  task.raster_facets.target = target_facet;
  task.vertex_buffer_ref = vertex_facet;
  /* With no index buffer, VG derives the vertex count from this allocation's
   * packed five-float vertex stride: 60 bytes / 20 = three vertices. */
  task.raster_filter = VG_FILTER_BILINEAR;
  task.raster_wrap = VG_WRAP_CLAMP;
  task.raster_tint[0] = task.raster_tint[1] = task.raster_tint[2] = task.raster_tint[3] = 1.0f;
  task.depth_compare_op = VG_DEPTH_COMPARE_ALWAYS;
  VgTaskId task_id;
  TRY(api.taskGraphAppendV2(builder, &task, 1, &task_id));
  VgSealDesc seal_desc = VG_INIT_STRUCT(VgSealDesc, VG_STRUCTURE_SEAL_DESC);
  TRY(api.sealTaskGraph(builder, &seal_desc, &graph));
  VgExecutionEnvelopeDesc envelope_desc = VG_INIT_STRUCT(VgExecutionEnvelopeDesc, VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  envelope_desc.arena = arena;
  TRY(api.createExecutionEnvelope(device, &envelope_desc, &envelope));
  VgSubmitDesc submit_desc = VG_INIT_STRUCT(VgSubmitDesc, VG_STRUCTURE_SUBMIT_DESC);
  submit_desc.graph = graph;
  submit_desc.envelope = envelope;
  TRY(api.submit(device, &submit_desc, &submission));

  uint8_t pixels[IMAGE_WIDTH * IMAGE_HEIGHT * 4u];
  TRY(api.readAllocation(arena, target, 0, pixels, sizeof(pixels)));
  FILE* output = fopen(output_path, "wb");
  if (!output) {
    perror(output_path);
    goto cleanup;
  }
  fprintf(output, "P6\n%u %u\n255\n", IMAGE_WIDTH, IMAGE_HEIGHT);
  for (uint32_t i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT; ++i) {
    if (fwrite(&pixels[i * 4u], 1, 3, output) != 3) {
      perror("writing PPM");
      fclose(output);
      goto cleanup;
    }
  }
  if (fclose(output) != 0) {
    perror("closing PPM");
    goto cleanup;
  }
  fprintf(stdout, "wrote %s (%ux%u) through the VG public C ABI\n", output_path, IMAGE_WIDTH, IMAGE_HEIGHT);
  result = 0;

cleanup:
  if (submission) api.destroySubmission(submission);
  if (envelope) api.destroyExecutionEnvelope(envelope);
  if (graph) api.destroyTaskGraph(graph);
  if (builder) api.destroyTaskGraphBuilder(builder);
  if (node) api.destroyNode(node);
  if (code) api.destroyCodeObject(code);
  if (arena) api.destroyArena(arena);
  if (domain) api.destroyAddressDomain(domain);
  if (device) api.destroyDevice(device);
  if (adapter) api.closeAdapter(adapter);
  if (runtime) api.destroyRuntime(runtime);
  return result;
}
