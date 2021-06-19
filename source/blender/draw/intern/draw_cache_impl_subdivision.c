/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2021, Blender Foundation.
 */

#include "draw_subdivision.h"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_editmesh.h"
#include "BKE_modifier.h"
#include "BKE_scene.h"
#include "BKE_subdiv.h"
#include "BKE_subdiv_eval.h"
#include "BKE_subdiv_mesh.h"

#include "BLI_string.h"

#include "DRW_render.h"

#include "GPU_capabilities.h"
#include "GPU_compute.h"
#include "GPU_index_buffer.h"
#include "GPU_state.h"
#include "GPU_vertex_buffer.h"

#include "opensubdiv_capi.h"
#include "opensubdiv_capi_type.h"
#include "opensubdiv_converter_capi.h"
#include "opensubdiv_evaluator_capi.h"
#include "opensubdiv_topology_refiner_capi.h"

#include "draw_cache_extract.h"
#include "draw_cache_inline.h"

/* To-dos:
 * - improve OpenSubdiv_BufferInterface
 * - comments
 * - better structure for this file
 *
 * - revise patch_coords cache, try to do on GPU, or fix buffers
 * - fix edit mode rendering (face dot, snap to cage, etc.)
 * - (long term) add support for vertex colors
 *
 * - holes: deduplicate operator code in edit_mesh_tools.
 */

extern char datatoc_common_subdiv_lib_glsl[];
extern char datatoc_common_subdiv_buffer_build_comp_glsl[];

enum {
  SHADER_BUFFER_BUILD,

  NUM_SHADERS,
};

static GPUShader *g_subdiv_shaders[NUM_SHADERS];

static const char *get_shader_code(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_BUILD: {
      return datatoc_common_subdiv_buffer_build_comp_glsl;
    }
  }
  return NULL;
}

static const char *get_shader_name(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_BUILD: {
      return "subdiv buffer build";
    }
  }
  return NULL;
}

static GPUShader *get_subdiv_shader(int shader_type)
{
  if (g_subdiv_shaders[shader_type] == NULL) {
    const char *compute_code = get_shader_code(shader_type);
    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, datatoc_common_subdiv_lib_glsl, NULL, get_shader_name(shader_type));
  }
  return g_subdiv_shaders[shader_type];
}

/* Vertex format used for rendering the result; corresponds to the VertexBufferData struct above.
 */
static GPUVertFormat *get_render_format()
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* WARNING Adjust #VertexBufferData struct in common_subdiv_lib accordingly.
     * We use 4 components for the vectors to account for padding in the compute shaders, where
     * vec3 is promoted to vec4. */
    GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    GPU_vertformat_alias_add(&format, "vnor");
  }
  return &format;
}

/* Vertex format for the OpenSubdiv vertex buffer. */
static GPUVertFormat *get_work_vertex_format()
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* We use 4 components for the vectors to account for padding in the compute shaders, where
     * vec3 is promoted to vec4. */
    GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format used for the patch coords buffer; corresponds to the PatchCoord struct from OSD.
 */
static GPUVertFormat *get_patch_coords_format()
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "vertex_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "array_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "patch_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "u", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    GPU_vertformat_attr_add(&format, "v", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  }
  return &format;
}

// --------------------------------------------------------

static uint tris_count_from_number_of_loops(const uint number_of_loops)
{
  const uint32_t number_of_quads = number_of_loops / 4;
  return number_of_quads * 2;
}

// --------------------------------------------------------

static uint vertbuf_bind_gpu(OpenSubdiv_BufferInterface *buffer)
{
  GPUVertBuf *verts = (GPUVertBuf *)(buffer->data);
  GPU_vertbuf_use(verts);
  return GPU_vertbuf_get_device_ptr(verts);
}

static void *vertbuf_alloc(OpenSubdiv_BufferInterface *interface, const uint len)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  GPU_vertbuf_data_alloc(verts, len);
  return GPU_vertbuf_get_data(verts);
}

static int vertbuf_num_vertices(OpenSubdiv_BufferInterface *interface)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  return GPU_vertbuf_get_vertex_len(verts);
}

static void opensubdiv_gpu_buffer_init(OpenSubdiv_BufferInterface *buffer_interface,
                                       GPUVertBuf *vertbuf)
{
  buffer_interface->data = vertbuf;
  buffer_interface->bind = vertbuf_bind_gpu;
  buffer_interface->alloc = vertbuf_alloc;
  buffer_interface->num_vertices = vertbuf_num_vertices;
}

// --------------------------------------------------------

// TODO(kevindietrich) : duplicate of extract_uv_init
static void initialize_uv_buffer(GPUVertBuf *uvs, CustomData *cd_ldata, int i, uint v_len)
{
  GPUVertFormat format = {0};
  GPU_vertformat_deinterleave(&format);

  char attr_name[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
  const char *layer_name = CustomData_get_layer_name(cd_ldata, CD_MLOOPUV, i);

  GPU_vertformat_safe_attr_name(layer_name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);
  /* UV layer name. */
  BLI_snprintf(attr_name, sizeof(attr_name), "u%s", attr_safe_name);
  GPU_vertformat_attr_add(&format, attr_name, GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  /* Auto layer name. */
  BLI_snprintf(attr_name, sizeof(attr_name), "a%s", attr_safe_name);
  GPU_vertformat_alias_add(&format, attr_name);
  /* Active render layer name. */
  if (i == CustomData_get_render_layer(cd_ldata, CD_MLOOPUV)) {
    GPU_vertformat_alias_add(&format, "u");
  }
  /* Active display layer name. */
  if (i == CustomData_get_active_layer(cd_ldata, CD_MLOOPUV)) {
    GPU_vertformat_alias_add(&format, "au");
    /* Alias to `pos` for edit uvs. */
    GPU_vertformat_alias_add(&format, "pos");
  }
  /* Stencil mask uv layer name. */
  if (i == CustomData_get_stencil_layer(cd_ldata, CD_MLOOPUV)) {
    GPU_vertformat_alias_add(&format, "mu");
  }

  if (v_len == 0) {
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    /* VBO will not be used, only allocate minimum of memory. */
    v_len = 1;
  }

  GPU_vertbuf_init_build_on_device(uvs, &format, v_len);
}

// --------------------------------------------------------

typedef struct DRWSubdivBuffers {
  uint number_of_loops;
  uint number_of_quads;
  uint number_of_triangles;

  GPUVertBuf *patch_coords;
  GPUVertBuf *work_vertices;
} DRWSubdivBuffers;

static void initialize_buffers(Subdiv *subdiv,
                               DRWSubdivBuffers *buffers,
                               const OpenSubdiv_PatchCoord *patch_coords,
                               const uint num_patch_coords,
                               const bool do_patch_coords)
{
  const uint number_of_loops = num_patch_coords;
  buffers->number_of_loops = number_of_loops;
  buffers->number_of_quads = number_of_loops / 4;
  buffers->number_of_triangles = tris_count_from_number_of_loops(number_of_loops);

  /* Used to evaluate vertices from our the patches. */
  buffers->work_vertices = GPU_vertbuf_calloc();
  GPU_vertbuf_init_build_on_device(
      buffers->work_vertices, get_work_vertex_format(), number_of_loops);

  /* Create the patch coords buffer. */
  if (do_patch_coords) {
    if (subdiv->patch_coords_draw_cache) {
      GPU_vertbuf_discard((GPUVertBuf *)(subdiv->patch_coords_draw_cache));
      subdiv->patch_coords_draw_cache = NULL;
    }

    OpenSubdiv_BufferInterface patch_coords_buffer_interface;
    GPUVertBuf *patch_coords_buffer = GPU_vertbuf_calloc();
    GPU_vertbuf_init_with_format_ex(
        patch_coords_buffer, get_patch_coords_format(), GPU_USAGE_STATIC);
    opensubdiv_gpu_buffer_init(&patch_coords_buffer_interface, patch_coords_buffer);
    buffers->patch_coords = patch_coords_buffer;

    OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;
    evaluator->buildPatchCoordsBuffer(
        evaluator, patch_coords, num_patch_coords, &patch_coords_buffer_interface);

    subdiv->patch_coords_draw_cache = patch_coords_buffer;
  }
  else {
    BLI_assert(subdiv->patch_coords_draw_cache);
    buffers->patch_coords = (GPUVertBuf *)(subdiv->patch_coords_draw_cache);
  }
}

static void free_buffers(DRWSubdivBuffers *buffers)
{
  // GPU_vertbuf_discard(buffers->patch_coords);
  GPU_vertbuf_discard(buffers->work_vertices);
}

// --------------------------------------------------------

static void do_build_draw_buffer(DRWSubdivBuffers *buffers,
                                 GPUVertBuf *subdiv_pos_nor,
                                 GPUIndexBuf *subdiv_tris)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_BUILD);
  GPU_shader_bind(shader);

  /* Inputs */
  GPU_vertbuf_bind_as_ssbo(buffers->work_vertices, 0);

  /* Outputs */
  GPU_vertbuf_bind_as_ssbo(subdiv_pos_nor, 1);
  GPU_indexbuf_bind_as_ssbo(subdiv_tris, 2);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_eval_patches_limits(Subdiv *subdiv, DRWSubdivBuffers *buffers)
{
  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;

  OpenSubdiv_BufferInterface patch_coord_interface;
  opensubdiv_gpu_buffer_init(&patch_coord_interface, buffers->patch_coords);

  OpenSubdiv_BufferInterface vertex_interface;
  opensubdiv_gpu_buffer_init(&vertex_interface, buffers->work_vertices);

  evaluator->evaluatePatchesLimitFromBuffer(evaluator, &patch_coord_interface, &vertex_interface);
}

static void do_eval_face_varying_limits(Subdiv *subdiv,
                                        DRWSubdivBuffers *buffers,
                                        GPUVertBuf *face_varying)
{
  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;

  OpenSubdiv_BufferInterface patch_coord_interface;
  opensubdiv_gpu_buffer_init(&patch_coord_interface, buffers->patch_coords);

  OpenSubdiv_BufferInterface face_varying_interface;
  opensubdiv_gpu_buffer_init(&face_varying_interface, face_varying);

  evaluator->evaluateFaceVaryingFromBuffer(
      evaluator, 0, &patch_coord_interface, &face_varying_interface);
}

// --------------------------------------------------------

// TODO(kevindietrich): move this to draw_cache_impl_mesh
static bool check_requests(MeshBufferCache *mbc)
{
  return DRW_vbo_requested(mbc->vbo.subdiv_pos) || DRW_ibo_requested(mbc->ibo.subdiv_tris) ||
         DRW_vbo_requested(mbc->vbo.subdiv_uv) || DRW_vbo_requested(mbc->vbo.subdiv_vcol);
}

static SubsurfModifierData *get_subsurf_modifier(Object *ob)
{
  ModifierData *md = (ModifierData *)(ob->modifiers.last);

  if (md == NULL) {
    return NULL;
  }

  if (md->type != eModifierType_Subsurf) {
    return NULL;
  }

  return (SubsurfModifierData *)(md);
}

static void free_patch_coords_buffer(void *ptr)
{
  GPUVertBuf *patch_coords_buffer = (GPUVertBuf *)(ptr);
  if (patch_coords_buffer) {
    GPU_vertbuf_discard(patch_coords_buffer);
  }
}

void DRW_create_subdivision(const Scene *scene, Object *ob, Mesh *mesh, MeshBufferCache *mbc)
{
#if 0
  std::cerr << "-- Positions requested: " << DRW_vbo_requested(mbc->vbo.subdiv_pos) << '\n';
  std::cerr << "-- Triangles requested: " << DRW_ibo_requested(mbc->ibo.subdiv_tris) << '\n';
  std::cerr << "-- UV layer requested: " << DRW_vbo_requested(mbc->vbo.subdiv_uv) << '\n';
  std::cerr << "-- Vertex colors requested: " << DRW_vbo_requested(mbc->vbo.subdiv_vcol) << '\n';
#endif

  if (!check_requests(mbc)) {
    return;
  }
  // std::cerr << __func__ << '\n';

  SubsurfModifierData *smd = get_subsurf_modifier(ob);
  BLI_assert(smd);

  const bool is_final_render = DRW_state_is_scene_render();

  SubdivSettings settings;
  BKE_subdiv_settings_init_from_modifier(&settings, smd, is_final_render);

  if (settings.level == 0) {
    // TODO(kevindietrich): fill the regular mesh buffer cache.
    return;
  }

  Mesh *mesh_eval = mesh;
  if (mesh->edit_mesh) {
    mesh_eval = mesh->edit_mesh->mesh_eval_final;
  }

  Subdiv *subdiv = BKE_modifier_subsurf_subdiv_descriptor_ensure(smd, &settings, mesh_eval, true);
  if (!subdiv) {
    return;
  }

  if (!BKE_subdiv_eval_begin_from_mesh(subdiv, mesh_eval, NULL, OPENSUBDIV_EVALUATOR_GLSL_COMPUTE)) {
    return;
  }

  const int level = get_render_subsurf_level(&scene->r, smd->levels, is_final_render);
  SubdivToMeshSettings to_mesh_settings;
  to_mesh_settings.resolution = (1 << level) + 1;
  to_mesh_settings.use_optimal_display = false;

  if (subdiv->patch_coords != NULL && subdiv->patch_resolution != to_mesh_settings.resolution) {
    MEM_freeN(subdiv->patch_coords);
    subdiv->patch_coords = NULL;
  }

  subdiv->free_draw_cache = free_patch_coords_buffer;

  const bool do_patch_coords = subdiv->patch_coords == NULL;
  if (do_patch_coords) {
    OpenSubdiv_PatchCoord *patch_coords = BKE_subdiv_build_patch_coords_array(
        mesh_eval, &to_mesh_settings, subdiv, &subdiv->num_patch_coords);
    if (subdiv->num_patch_coords == 0) {
      // We should still initialize the buffers.
      // TODO(kevindietrich): Error: Not freed memory blocks: 2, total unfreed memory 0.000000 MB
      GPU_vertbuf_init_build_on_device(mbc->vbo.subdiv_pos, get_render_format(), 0);
      GPU_indexbuf_init_build_on_device(mbc->ibo.subdiv_tris, 0);
      return;
    }

    subdiv->patch_resolution = to_mesh_settings.resolution;
    subdiv->patch_coords = patch_coords;
  }

  DRWSubdivBuffers subdiv_buffers = {0};
  initialize_buffers(
      subdiv, &subdiv_buffers, subdiv->patch_coords, subdiv->num_patch_coords, do_patch_coords);

  if (DRW_vbo_requested(mbc->vbo.subdiv_pos) || DRW_ibo_requested(mbc->ibo.subdiv_tris)) {
    do_eval_patches_limits(subdiv, &subdiv_buffers);

    /* Initialize the output buffers */

    /* Initialise the vertex buffer, it was already allocated. */
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.subdiv_pos, get_render_format(), subdiv_buffers.number_of_loops);

    /* Initialise the index buffer, it was already allocated, it will be filled on the device. */
    GPU_indexbuf_init_build_on_device(mbc->ibo.subdiv_tris,
                                      subdiv_buffers.number_of_triangles * 3);

    do_build_draw_buffer(&subdiv_buffers, mbc->vbo.subdiv_pos, mbc->ibo.subdiv_tris);
  }

  if (DRW_vbo_requested(mbc->vbo.subdiv_uv)) {
    initialize_uv_buffer(mbc->vbo.subdiv_uv, &mesh_eval->ldata, 0, subdiv_buffers.number_of_loops);
    do_eval_face_varying_limits(subdiv, &subdiv_buffers, mbc->vbo.subdiv_uv);
  }

  free_buffers(&subdiv_buffers);
}

void DRW_subdiv_free(void)
{
  for (int i = 0; i < NUM_SHADERS; ++i) {
    GPU_shader_free(g_subdiv_shaders[i]);
  }
}
