/* F7 checkpoint A: a real C translation unit using no private headers. */
#include <vg/vg.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TRY(x) do { if ((x) != VG_SUCCESS) { fprintf(stderr, "%s: %s\n", #x, vgGetLastDiagnostic()); return 1; } } while (0)

static void view(VgCanonicalViewDesc* d, uint64_t id, uint32_t gen, uint32_t format, uint32_t w, uint32_t h) {
  memset(d, 0, sizeof(*d));
  d->header.type = VG_STRUCTURE_CANONICAL_VIEW_DESC; d->header.size = sizeof(*d);
  d->allocation = id; d->allocation_generation = gen; d->format = format;
  d->dimension = VG_VIEW_DIMENSION_TEXTURE_2D; d->width = w; d->height = h;
  d->array_layers = 1; d->mip_levels = 1;
  d->swizzle_red = VG_SWIZZLE_RED; d->swizzle_green = VG_SWIZZLE_GREEN;
  d->swizzle_blue = VG_SWIZZLE_BLUE; d->swizzle_alpha = VG_SWIZZLE_ALPHA;
}

int main(void) {
  VgApi api = {0}; api.size = sizeof(api); TRY(vgGetApi(VG_API_VERSION_1_6, &api));
  VgRuntimeDesc rd = VG_INIT_STRUCT(VgRuntimeDesc, VG_STRUCTURE_RUNTIME_DESC);
  VgRuntime runtime = 0; TRY(api.createRuntime(&rd, &runtime));
  uint32_t count = 0; TRY(api.enumerateAdapters(runtime, &count, 0));
  VgAdapterInfo infos[8]; memset(infos, 0, sizeof(infos)); if (count > 8) count = 8;
  for (uint32_t i = 0; i < count; ++i) { infos[i].header.type = VG_STRUCTURE_ADAPTER_INFO; infos[i].header.size = sizeof(VgAdapterInfo); }
  TRY(api.enumerateAdapters(runtime, &count, infos));
  uint32_t pick = 0; for (uint32_t i = 0; i < count; ++i) if (infos[i].backend_kind == VG_BACKEND_METAL) { pick = i; break; }
  VgAdapter adapter = 0; TRY(api.openAdapter(runtime, infos[pick].stable_uuid, &adapter));
  VgDeviceDesc dd = VG_INIT_STRUCT(VgDeviceDesc, VG_STRUCTURE_DEVICE_DESC); VgDevice device = 0; TRY(api.createDevice(adapter, &dd, &device));
  VgAddressDomainDesc ad = VG_INIT_STRUCT(VgAddressDomainDesc, VG_STRUCTURE_ADDRESS_DOMAIN_DESC); ad.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
  VgAddressDomain domain = 0; TRY(api.createAddressDomain(device, &ad, &domain));
  VgArenaDesc ar = VG_INIT_STRUCT(VgArenaDesc, VG_STRUCTURE_ARENA_DESC); ar.domain = domain; VgArena arena = 0; TRY(api.createArena(device, &ar, &arena));

  VgAllocation source=0,target=0,depth=0,vertices=0,indices=0;
  TRY(api.arenaAllocate(arena, 64, &source)); TRY(api.arenaAllocate(arena, 64, &target)); TRY(api.arenaAllocate(arena, 64, &depth));
  TRY(api.arenaAllocate(arena, 4*5*sizeof(float), &vertices)); TRY(api.arenaAllocate(arena, 6*sizeof(uint16_t), &indices));
  uint64_t sid,tid,did,vid,iid; uint32_t sg,tg,dg,vg,ig;
  TRY(api.getAllocationRef(source,&sid,&sg)); TRY(api.getAllocationRef(target,&tid,&tg)); TRY(api.getAllocationRef(depth,&did,&dg));
  TRY(api.getAllocationRef(vertices,&vid,&vg)); TRY(api.getAllocationRef(indices,&iid,&ig));
  uint8_t pixels[64]; for (uint32_t i=0;i<64;i+=4) { pixels[i]=255; pixels[i+1]=0; pixels[i+2]=0; pixels[i+3]=255; }
  const float quad[] = {-1,-1,.5f,0,1, 1,-1,.5f,1,1, -1,1,.5f,0,0, 1,1,.5f,1,0};
  const uint16_t index[] = {0,1,2,2,1,3};
  TRY(api.writeAllocation(arena,source,0,pixels,sizeof(pixels))); TRY(api.writeAllocation(arena,vertices,0,quad,sizeof(quad))); TRY(api.writeAllocation(arena,indices,0,index,sizeof(index)));

  VgCanonicalViewDesc v; VgFacetRef sf,tf,df,vf,ifac;
  view(&v,sid,sg,VG_PIXEL_FORMAT_RGBA8_UNORM,4,4); TRY(api.acquireFacet(device,arena,&v,VG_FACET_KIND_SAMPLE,&sf));
  view(&v,tid,tg,VG_PIXEL_FORMAT_RGBA8_UNORM,4,4); TRY(api.acquireFacet(device,arena,&v,VG_FACET_KIND_ATTACHMENT,&tf));
  view(&v,did,dg,VG_PIXEL_FORMAT_DEPTH32_FLOAT,4,4); TRY(api.acquireFacet(device,arena,&v,VG_FACET_KIND_ATTACHMENT,&df));
  view(&v,vid,vg,VG_PIXEL_FORMAT_RGBA8_UNORM,20,1); TRY(api.acquireFacet(device,arena,&v,VG_FACET_KIND_ADDRESS,&vf));
  view(&v,iid,ig,VG_PIXEL_FORMAT_R16_UINT,6,1); TRY(api.acquireFacet(device,arena,&v,VG_FACET_KIND_ADDRESS,&ifac));

  char module[512];
  int module_size = snprintf(module, sizeof(module), "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.f7.checkpoint/v1\",\"instructions\":[{\"op\":\"load\",\"allocation\":%llu,\"generation\":%u,\"offset\":0,\"size\":4}],\"effects\":[{\"allocation\":%llu,\"offset\":0,\"size\":4,\"access\":\"read\",\"representation_epoch\":0}]}", (unsigned long long)sid, sg, (unsigned long long)sid);
  if (module_size < 0 || (size_t)module_size >= sizeof(module)) return 3;
  VgCodeObjectDesc cd = VG_INIT_STRUCT(VgCodeObjectDesc, VG_STRUCTURE_CODE_OBJECT_DESC); cd.bytes=module; cd.byte_size=(uint64_t)module_size; cd.format_tag="vg.ir/v1";
  VgCodeObject code=0; TRY(api.loadCodeObject(device,&cd,&code));
  VgNodeDesc nd = VG_INIT_STRUCT(VgNodeDesc, VG_STRUCTURE_NODE_DESC); nd.entry_name="checkpoint"; VgNode node=0; TRY(api.createNode(code,&nd,&node)); VgNodeRef nr; TRY(api.getNodeRef(node,&nr));
  VgTaskGraphBuilderDesc bd = VG_INIT_STRUCT(VgTaskGraphBuilderDesc, VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC); bd.code_object=code; VgTaskGraphBuilder builder=0; TRY(api.createTaskGraphBuilder(device,&bd,&builder));
  VgTaskRecordV2 task; memset(&task,0,sizeof(task)); task.node=nr; task.root_generation=1; task.shape.x=1; task.shape.y=1; task.shape.z=1;
  task.kind=VG_TASK_KIND_RASTER; task.topology=VG_TOPOLOGY_TRIANGLE_LIST; task.raster_facets.source=sf; task.raster_facets.target=tf; task.vertex_buffer_ref=vf; task.index_buffer_ref=ifac; task.index_count=6;
  task.raster_filter=VG_FILTER_NEAREST; task.raster_wrap=VG_WRAP_CLAMP; task.raster_tint[0]=task.raster_tint[1]=task.raster_tint[2]=task.raster_tint[3]=1;
  task.depth_attachment_ref=df; task.depth_test_enable=VG_TRUE; task.depth_write_enable=VG_TRUE; task.depth_compare_op=VG_DEPTH_COMPARE_LESS;
  VgTaskId id; TRY(api.taskGraphAppendV2(builder,&task,1,&id));
  VgSealDesc sd = VG_INIT_STRUCT(VgSealDesc, VG_STRUCTURE_SEAL_DESC); VgTaskGraph graph=0; TRY(api.sealTaskGraph(builder,&sd,&graph));
  VgExecutionEnvelopeDesc ed = VG_INIT_STRUCT(VgExecutionEnvelopeDesc, VG_STRUCTURE_EXECUTION_ENVELOPE_DESC); ed.arena=arena; VgExecutionEnvelope env=0; TRY(api.createExecutionEnvelope(device,&ed,&env));
  VgSubmitDesc sub = VG_INIT_STRUCT(VgSubmitDesc, VG_STRUCTURE_SUBMIT_DESC); sub.graph=graph; sub.envelope=env; VgSubmission submission=0; TRY(api.submit(device,&sub,&submission));
  uint8_t result[64]; TRY(api.readAllocation(arena,target,0,result,sizeof(result)));
  if (result[20] != 255 || result[21] != 0 || result[23] != 255) { fprintf(stderr,"checkpoint pixel mismatch\n"); return 2; }
  api.destroySubmission(submission); api.destroyExecutionEnvelope(env); api.destroyTaskGraph(graph); api.destroyTaskGraphBuilder(builder); api.destroyNode(node); api.destroyCodeObject(code); api.destroyArena(arena); api.destroyAddressDomain(domain); api.destroyDevice(device); api.closeAdapter(adapter); api.destroyRuntime(runtime);
  return 0;
}
