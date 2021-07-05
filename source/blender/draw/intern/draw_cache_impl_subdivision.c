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
#include "BKE_subdiv_foreach.h"
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
#include "draw_cache_extract_mesh_private.h"
#include "draw_cache_impl.h"
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
extern char datatoc_common_subdiv_buffer_lines_comp_glsl[];
extern char datatoc_common_subdiv_buffer_lnor_comp_glsl[];
extern char datatoc_common_subdiv_buffer_edge_fac_comp_glsl[];
extern char datatoc_common_subdiv_buffer_points_comp_glsl[];
extern char datatoc_common_subdiv_patch_handles_comp_glsl[];

enum {
  SHADER_BUFFER_BUILD,
  SHADER_BUFFER_BUILD_SMOOTH,
  SHADER_BUFFER_LINES,
  SHADER_BUFFER_EDGE_FAC,
  SHADER_BUFFER_LNOR,
  SHADER_BUFFER_POINTS,
  SHADER_BUFFER_PATCH_COORDS,

  NUM_SHADERS,
};

static GPUShader *g_subdiv_shaders[NUM_SHADERS];

static const char *get_shader_code(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_BUILD:
    case SHADER_BUFFER_BUILD_SMOOTH: {
      return datatoc_common_subdiv_buffer_build_comp_glsl;
    }
    case SHADER_BUFFER_LINES: {
      return datatoc_common_subdiv_buffer_lines_comp_glsl;
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return datatoc_common_subdiv_buffer_edge_fac_comp_glsl;
    }
    case SHADER_BUFFER_LNOR: {
      return datatoc_common_subdiv_buffer_lnor_comp_glsl;
    }
    case SHADER_BUFFER_POINTS: {
      return datatoc_common_subdiv_buffer_points_comp_glsl;
    }
    case SHADER_BUFFER_PATCH_COORDS: {
      return datatoc_common_subdiv_patch_handles_comp_glsl;
    }
  }
  return NULL;
}

static const char *get_shader_name(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_BUILD:
    case SHADER_BUFFER_BUILD_SMOOTH: {
      return "subdiv buffer build";
    }
    case SHADER_BUFFER_LINES: {
      return "subdiv lines build";
    }
    case SHADER_BUFFER_LNOR: {
      return "subdiv lnor build";
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return "subdiv edge fac build";
    }
    case SHADER_BUFFER_POINTS: {
      return "subdiv points build";
    }
    case SHADER_BUFFER_PATCH_COORDS: {
      return "subdiv points coords";
    }
  }
  return NULL;
}

static GPUShader *get_subdiv_shader(int shader_type, const char *defines)
{
  if (g_subdiv_shaders[shader_type] == NULL) {
    const char *compute_code = get_shader_code(shader_type);
    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, datatoc_common_subdiv_lib_glsl, defines, get_shader_name(shader_type));
  }
  return g_subdiv_shaders[shader_type];
}

/* Vertex format used for rendering the result; corresponds to the VertexBufferData struct above.
 */
static GPUVertFormat *get_render_format(void)
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
static GPUVertFormat *get_work_vertex_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* We use 4 components for the vectors to account for padding in the compute shaders, where
     * vec3 is promoted to vec4. */
    GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format used for the OpenSubdiv_PatchCoord.
 */
static GPUVertFormat *get_blender_patch_coords_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "patch_index", GPU_COMP_U32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "u", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    GPU_vertformat_attr_add(&format, "v", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format used for the PatchTable::PatchHandle.
 */
static GPUVertFormat *get_patch_handle_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "vertex_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "array_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "patch_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }
  return &format;
}

/* Vertex format used for the quadtree nodes of the PatchMap.
 */
static GPUVertFormat *get_quadtree_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "child", GPU_COMP_U32, 4, GPU_FETCH_INT);
  }
  return &format;
}

/* Vertex format used for the patch coords buffer; corresponds to the PatchCoord struct from OSD.
 */
static GPUVertFormat *get_patch_coords_format(void)
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

static GPUVertFormat *get_edge_fac_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "wd", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  }
  return &format;
}

static GPUVertFormat *get_lnor_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    GPU_vertformat_alias_add(&format, "lnor");
  }
  return &format;
}

static GPUVertFormat *get_edit_data_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_U16, 4, GPU_FETCH_INT);
    GPU_vertformat_alias_add(&format, "flag");
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
  buffer_interface->buffer_offset = 0;
}

// --------------------------------------------------------

static void initialize_uv_buffer(GPUVertBuf *uvs,
                                 struct MeshBatchCache *cache,
                                 CustomData *cd_ldata,
                                 uint v_len,
                                 uint *r_uv_layers)
{
  GPUVertFormat format = {0};

  if (!mesh_extract_uv_format_init(&format, cache, cd_ldata, MR_EXTRACT_MESH, r_uv_layers)) {
    // TODO(kevindietrich): handle this more gracefully.
    v_len = 1;
  }

  GPU_vertbuf_init_build_on_device(uvs, &format, v_len);
}

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
 * \{ */

struct GPUVertBuf *build_origindex_buffer(int *vert_origindex, uint num_loops)
{
  GPUVertBuf *buffer = GPU_vertbuf_calloc();

  static GPUVertFormat format;
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "vindex", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }

  GPU_vertbuf_init_with_format_ex(buffer, &format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(buffer, num_loops);

  int *vbo_data = (int *)GPU_vertbuf_get_data(buffer);

  for (int i = 0; i < num_loops; ++i) {
    vbo_data[i] = vert_origindex[i];
  }

  return buffer;
}

struct GPUVertBuf *build_flags_buffer(int *vert_origindex, uint num_loops)
{
  GPUVertBuf *buffer = GPU_vertbuf_calloc();

  static GPUVertFormat format;
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "flags", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }

  GPU_vertbuf_init_with_format_ex(buffer, &format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(buffer, num_loops);

  int *vbo_data = (int *)GPU_vertbuf_get_data(buffer);

  for (int i = 0; i < num_loops; ++i) {
    vbo_data[i] = vert_origindex[i];
  }

  return buffer;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
 * \{ */

typedef struct DRWSubdivCache {
  /* Coordinates used to evaluate patches, for UVs, positions, and normals. */
  GPUVertBuf *patch_coords;

  /* Resolution used to generate the patch coordinates. */
  int resolution;

  /* Number of coordinantes. */
  uint num_patch_coords;

  /* Maps to original element in the coarse mesh, only for edit mode. */
  GPUVertBuf *verts_orig_index;
  GPUVertBuf *faces_orig_index;
  GPUVertBuf *edges_orig_index;

  /* General flags (selection, edge render). */
  GPUVertBuf *flags;
  EditLoopData *edit_data;
} DRWSubdivCache;

static void draw_free_edit_mode_cache(DRWSubdivCache *cache)
{
  if (cache->verts_orig_index) {
    GPU_vertbuf_discard(cache->verts_orig_index);
    cache->verts_orig_index = NULL;
  }
  if (cache->edges_orig_index) {
    GPU_vertbuf_discard(cache->edges_orig_index);
    cache->edges_orig_index = NULL;
  }
  if (cache->faces_orig_index) {
    GPU_vertbuf_discard(cache->faces_orig_index);
    cache->faces_orig_index = NULL;
  }
}

static void draw_subdiv_cache_free(DRWSubdivCache *cache)
{
  if (cache->patch_coords) {
    GPU_vertbuf_discard(cache->patch_coords);
    cache->patch_coords = NULL;
  }
  if (cache->flags) {
    GPU_vertbuf_discard(cache->flags);
    cache->flags = NULL;
  }
  draw_free_edit_mode_cache(cache);
  cache->resolution = 0;
  cache->num_patch_coords = 0;
}

static void free_draw_cache_from_subdiv_cb(void *ptr)
{
  DRWSubdivCache *cache = (DRWSubdivCache *)(ptr);
  draw_subdiv_cache_free(cache);
  MEM_freeN(cache);
}

static DRWSubdivCache *ensure_draw_cache(Subdiv *subdiv)
{
  DRWSubdivCache *draw_cache = subdiv->draw_cache;
  if (draw_cache == NULL) {
    fprintf(stderr, "Creating a new cache !\n");
    draw_cache = MEM_callocN(sizeof(DRWSubdivCache), "DRWSubdivCache");
  }
  subdiv->draw_cache = draw_cache;
  subdiv->free_draw_cache = free_draw_cache_from_subdiv_cb;
  return draw_cache;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWCacheBuildingContext
 * \{ */

enum {
  SMOOTH_SHADING = (1 << 0),
};

typedef struct DRWCacheBuildingContext {
  const Mesh *coarse_mesh;
  const SubdivToMeshSettings *settings;

  OpenSubdiv_PatchCoord *patch_coords;
  /* Number of coordinantes. */
  uint num_patch_coords;

  int *vert_origindex;
  int *edge_origindex;
  int *face_origindex;
  int *flags;

  EditLoopData *edit_data;
} DRWCacheBuildingContext;

static bool patch_coords_topology_info(const SubdivForeachContext *foreach_context,
                                       const int UNUSED(num_vertices),
                                       const int UNUSED(num_edges),
                                       const int num_loops,
                                       const int UNUSED(num_polygons))
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->patch_coords = MEM_mallocN(num_loops * sizeof(OpenSubdiv_PatchCoord),
                                  "OpenSubdiv PatchCoord");

  ctx->vert_origindex = MEM_mallocN(num_loops * sizeof(int), "verts orig index");
  ctx->edge_origindex = MEM_mallocN(num_loops * sizeof(int), "edges orig index");
  ctx->face_origindex = MEM_mallocN(num_loops * sizeof(int), "polys orig index");
  ctx->flags = MEM_mallocN(num_loops * sizeof(int), "flags index");

  // ctx->edit_data = MEM_mallocN(num_loops * sizeof(EditLoopData), "EditLoopData");

  // num polys
  // ctx->fdots_pos = MEM_mallocN(num_loops * sizeof(float) * 3, "polys orig index");
  // ctx->fdots_nor = MEM_mallocN(num_loops * sizeof(float) * 3, "polys orig index");

  ctx->num_patch_coords = num_loops;
  return true;
}

static int get_loop_corner_index(float u, float v)
{
  if (u == 0.0f && v == 0.0f) {
    return 0;
  }

  if (u == 0.0f && v == 1.0f) {
    return 1;
  }

  if (u == 1.0f && v == 1.0f) {
    return 2;
  }

  if (u == 1.0f && v == 0.0f) {
    return 3;
  }

  return -1;
}

static bool is_on_outer_edge(float u, float v)
{
  return u == 0.0f || v == 0.0f || u == 1.0f || v == 1.0f;
}

static void patch_coords_loop(const SubdivForeachContext *foreach_context,
                              void *UNUSED(tls_v),
                              const int ptex_face_index,
                              const float u,
                              const float v,
                              const int UNUSED(coarse_loop_index),
                              const int coarse_poly_index,
                              const int coarse_corner,
                              const int subdiv_loop_index,
                              const int UNUSED(subdiv_vertex_index),
                              const int UNUSED(subdiv_edge_index))
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  OpenSubdiv_PatchCoord patch_coord = {ptex_face_index, u, v};
  ctx->patch_coords[subdiv_loop_index] = patch_coord;

  /* Setup editmode data. */
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MPoly *mpoly = &coarse_mesh->mpoly[coarse_poly_index];
  const MLoop *mloops = coarse_mesh->mloop;
  int flags = 0;
  int vert_orig_index = -1;
  int edge_orig_index = -1;
  const int face_orig_index = coarse_poly_index;

  ushort v_flag = 0;
  ushort e_flag = 0;
  ushort crease = 0;
  ushort bweight = 0;

  if (mpoly->flag & ME_SMOOTH) {
    flags |= SMOOTH_SHADING;
  }

  const int corner_index = get_loop_corner_index(u, v);
  if (corner_index != -1) {
    const MLoop *mloop = &mloops[mpoly->loopstart + corner_index];
    vert_orig_index = mloop->v;
    edge_orig_index = mloop->e;

    if (coarse_mesh->edit_mesh) {
      BMesh *bm = coarse_mesh->edit_mesh->bm;
      BMVert *eve = BM_vert_at_index(bm, mloop->v);
      if (eve) {
        if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
          e_flag |= VFLAG_VERT_SELECTED;
        }
        if (eve == BM_mesh_active_vert_get(bm)) {
          e_flag |= VFLAG_VERT_ACTIVE;
        }
      }

      BMFace *efa = BM_face_at_index(bm, coarse_poly_index);
      if (efa) {
        if (efa == BM_mesh_active_face_get(bm, false, true)) {
          v_flag |= VFLAG_FACE_ACTIVE;
        }
        if (BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
          v_flag |= VFLAG_FACE_SELECTED;
        }
      }

      BMEdge *eed = BM_edge_at_index(bm, mloop->e);
      if (eed) {
        if (eed == BM_mesh_active_edge_get(bm)) {
          e_flag |= VFLAG_EDGE_ACTIVE;
        }
        if (/*!is_vertex_select_mode && */ BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
          e_flag |= VFLAG_EDGE_SELECTED;
        }
        if (/*is_vertex_select_mode &&*/ BM_elem_flag_test(eed->v1, BM_ELEM_SELECT) &&
            BM_elem_flag_test(eed->v2, BM_ELEM_SELECT)) {
          e_flag |= VFLAG_EDGE_SELECTED;
          e_flag |= VFLAG_VERT_SELECTED;
        }
        if (BM_elem_flag_test(eed, BM_ELEM_SEAM)) {
          e_flag |= VFLAG_EDGE_SEAM;
        }
        if (!BM_elem_flag_test(eed, BM_ELEM_SMOOTH)) {
          e_flag |= VFLAG_EDGE_SHARP;
        }
      }
    }
  }

  const bool is_outer = is_on_outer_edge(u, v);
  if (is_outer) {
    const MLoop *mloop = &mloops[mpoly->loopstart + coarse_corner];
    edge_orig_index = mloop->e;
  }
  else {
//    if (!ctx->settings->use_optimal_display) {
//    }
  }

  ctx->vert_origindex[subdiv_loop_index] = vert_orig_index;
  ctx->edge_origindex[subdiv_loop_index] = edge_orig_index;
  ctx->face_origindex[subdiv_loop_index] = face_orig_index;
  ctx->flags[subdiv_loop_index] = flags;
//  ctx->edit_data[subdiv_loop_index].v_flag = v_flag;
//  ctx->edit_data[subdiv_loop_index].e_flag = e_flag;
//  ctx->edit_data[subdiv_loop_index].crease = crease;
//  ctx->edit_data[subdiv_loop_index].bweight = bweight;
}

static void patch_coords_foreach_callbacks(SubdivForeachContext *foreach_context)
{
  memset(foreach_context, 0, sizeof(*foreach_context));
  foreach_context->topology_info = patch_coords_topology_info;
  foreach_context->loop = patch_coords_loop;
}

static void build_cached_data_from_subdiv(DRWCacheBuildingContext *cache_building_context,
                                          Subdiv *subdiv)
{
  SubdivForeachContext foreach_context;
  patch_coords_foreach_callbacks(&foreach_context);
  foreach_context.user_data = cache_building_context;

  BKE_subdiv_foreach_subdiv_geometry(subdiv,
                                     &foreach_context,
                                     cache_building_context->settings,
                                     cache_building_context->coarse_mesh);

//  for (int i = 0; i < cache_building_context->num_patch_coords; i++) {
//    fprintf(stderr, "flags : %d\n", cache_building_context->flags[i]);
//  }
#if 0
  int num_vertex_no_origindex = 0;
  for (int i = 0; i < cache_building_context->num_patch_coords; i += 2) {
    if (cache_building_context->edge_origindex[i] != -1) {
      if (cache_building_context->edge_origindex[i + 1] == -1) {
        cache_building_context->edge_origindex[i] = -1;
      }
    }

    if (cache_building_context->edge_origindex[i] != -1) {
      fprintf(stderr, "edge origindex : %d\n", cache_building_context->edge_origindex[i]);
    }
    else {
      ++num_vertex_no_origindex;
    }
  }
  fprintf(stderr, "%d edges with no origindex\n", num_vertex_no_origindex);
#endif
}

static void do_build_patch_coords(GPUVertBuf *blender_patch_coords,
                                  GPUVertBuf *patch_coords_buffer,
                                  GPUVertBuf *patch_map_handles,
                                  GPUVertBuf *patch_map_quadtree,
                                  int min_patch_face,
                                  int max_patch_face,
                                  int max_depth,
                                  int patches_are_triangular,
                                  uint num_patch_coords)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_PATCH_COORDS, NULL);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "min_patch_face", min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", max_depth);
  GPU_shader_uniform_1i(shader, "patches_are_triangular", patches_are_triangular);

  /* Inputs */
  GPU_vertbuf_bind_as_ssbo(patch_map_handles, 0);
  GPU_vertbuf_bind_as_ssbo(patch_map_quadtree, 1);
  GPU_vertbuf_bind_as_ssbo(blender_patch_coords, 2);

  /* Outputs */
  GPU_vertbuf_bind_as_ssbo(patch_coords_buffer, 3);

  // TODO(kevindietrich): don't execute for every patch coordinate.
  GPU_compute_dispatch(shader, num_patch_coords, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  GPU_shader_unbind();
}

static bool generate_required_cached_data(DRWSubdivCache *cache,
                                          Subdiv *subdiv,
                                          Mesh *mesh_eval,
                                          const Scene *scene,
                                          const SubsurfModifierData *smd,
                                          const bool is_final_render)
{
  const int level = get_render_subsurf_level(&scene->r, smd->levels, is_final_render);
  SubdivToMeshSettings to_mesh_settings;
  to_mesh_settings.resolution = (1 << level) + 1;
  to_mesh_settings.use_optimal_display = false;

  if (cache->resolution != to_mesh_settings.resolution) {
    fprintf(stderr, "Resolution changed rebuilding cache !\n");
    /* Resolution chaged, we need to rebuild. */
    draw_subdiv_cache_free(cache);
  }

  if (cache->patch_coords != NULL) {
    fprintf(stderr, "Cache does not need to be rebuilt !\n");
    /* No need to rebuild anything. */
    return true;
  }

  DRWCacheBuildingContext cache_building_context;
  cache_building_context.coarse_mesh = mesh_eval;
  cache_building_context.settings = &to_mesh_settings;

  build_cached_data_from_subdiv(&cache_building_context, subdiv);
  if (cache_building_context.num_patch_coords == 0) {
    return false;
  }

#if 0
  OpenSubdiv_BufferInterface patch_coords_buffer_interface;
  GPUVertBuf *patch_coords_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_coords_buffer, get_patch_coords_format(), GPU_USAGE_STATIC);
  opensubdiv_gpu_buffer_init(&patch_coords_buffer_interface, patch_coords_buffer);

  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;
  evaluator->buildPatchCoordsBuffer(
      evaluator, cache_building_context.patch_coords, cache_building_context.num_patch_coords, &patch_coords_buffer_interface);
#else
  /*
    - cache patch coords
    - build patch coords buffer
    - build PatchHandle array on the GPU
    - move PatchMap evaluation to the GPU
   */

  GPUVertBuf *patch_coords_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_build_on_device(
      patch_coords_buffer, get_patch_coords_format(), cache_building_context.num_patch_coords);

  GPUVertBuf *blender_patch_coords = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      blender_patch_coords, get_blender_patch_coords_format(), GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(blender_patch_coords, cache_building_context.num_patch_coords);
  memcpy(GPU_vertbuf_get_data(blender_patch_coords),
         cache_building_context.patch_coords,
         cache_building_context.num_patch_coords * sizeof(OpenSubdiv_PatchCoord));

  /* Build buffers for the PatchMap. */
  GPUVertBuf *patch_map_handles = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(patch_map_handles, get_patch_handle_format(), GPU_USAGE_STATIC);

  GPUVertBuf *patch_map_quadtree = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(patch_map_quadtree, get_quadtree_format(), GPU_USAGE_STATIC);

  OpenSubdiv_BufferInterface patch_map_handles_interface;
  opensubdiv_gpu_buffer_init(&patch_map_handles_interface, patch_map_handles);

  OpenSubdiv_BufferInterface patch_map_quad_tree_interface;
  opensubdiv_gpu_buffer_init(&patch_map_quad_tree_interface, patch_map_quadtree);

  int min_patch_face = 0;
  int max_patch_face = 0;
  int max_depth = 0;
  int patches_are_triangular = 0;

  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;
  evaluator->getPatchMap(evaluator,
                         &patch_map_handles_interface,
                         &patch_map_quad_tree_interface,
                         &min_patch_face,
                         &max_patch_face,
                         &max_depth,
                         &patches_are_triangular);

  do_build_patch_coords(blender_patch_coords,
                        patch_coords_buffer,
                        patch_map_handles,
                        patch_map_quadtree,
                        min_patch_face,
                        max_patch_face,
                        max_depth,
                        patches_are_triangular,
                        cache_building_context.num_patch_coords);

#endif
  cache->resolution = to_mesh_settings.resolution;
  cache->num_patch_coords = cache_building_context.num_patch_coords;
  cache->patch_coords = patch_coords_buffer;
  cache->verts_orig_index = build_origindex_buffer(cache_building_context.vert_origindex,
                                                   cache_building_context.num_patch_coords);
  cache->edges_orig_index = build_origindex_buffer(cache_building_context.edge_origindex,
                                                   cache_building_context.num_patch_coords);
  // cache->edit_data = cache_building_context.edit_data;
  cache->flags = build_flags_buffer(cache_building_context.flags,
                                    cache_building_context.num_patch_coords);

  /* Cleanup. */
  MEM_freeN(cache_building_context.patch_coords);
  MEM_freeN(cache_building_context.vert_origindex);
  MEM_freeN(cache_building_context.edge_origindex);
  MEM_freeN(cache_building_context.face_origindex);
  MEM_freeN(cache_building_context.flags);

  return true;
}

/** \} */

// --------------------------------------------------------

typedef struct DRWSubdivBuffers {
  uint number_of_loops;
  uint number_of_quads;
  uint number_of_triangles;

  GPUVertBuf *patch_coords;
  GPUVertBuf *work_vertices;

  GPUVertBuf *work_dPdu;
  GPUVertBuf *work_dPdv;

  GPUVertBuf *face_origindex;
  GPUVertBuf *vert_origindex;
  GPUVertBuf *edge_origindex;
  GPUVertBuf *flags;

  EditLoopData *edit_data;
} DRWSubdivBuffers;

static void initialize_buffers(DRWSubdivBuffers *buffers,
                               const DRWSubdivCache *draw_cache,
                               const bool do_smooth_normals)
{
  const uint number_of_loops = draw_cache->num_patch_coords;
  buffers->number_of_loops = number_of_loops;
  buffers->number_of_quads = number_of_loops / 4;
  buffers->number_of_triangles = tris_count_from_number_of_loops(number_of_loops);

  buffers->work_vertices = GPU_vertbuf_calloc();
  GPU_vertbuf_init_build_on_device(
      buffers->work_vertices, get_work_vertex_format(), number_of_loops);

  if (do_smooth_normals) {
    buffers->work_dPdu = GPU_vertbuf_calloc();
    GPU_vertbuf_init_build_on_device(
        buffers->work_dPdu, get_work_vertex_format(), number_of_loops);

    buffers->work_dPdv = GPU_vertbuf_calloc();
    GPU_vertbuf_init_build_on_device(
        buffers->work_dPdv, get_work_vertex_format(), number_of_loops);
  }
  else {
    buffers->work_dPdu = NULL;
    buffers->work_dPdv = NULL;
  }

  buffers->patch_coords = draw_cache->patch_coords;
  buffers->edge_origindex = draw_cache->edges_orig_index;
  buffers->vert_origindex = draw_cache->verts_orig_index;
  buffers->face_origindex = draw_cache->faces_orig_index;
  buffers->flags = draw_cache->flags;

  // buffers->edit_data = draw_cache->edit_data;
}

static void free_buffers(DRWSubdivBuffers *buffers)
{
  // GPU_vertbuf_discard(buffers->patch_coords);
  GPU_vertbuf_discard(buffers->work_vertices);

  if (buffers->work_dPdu) {
    GPU_vertbuf_discard(buffers->work_dPdu);
  }
  if (buffers->work_dPdv) {
    GPU_vertbuf_discard(buffers->work_dPdv);
  }
}

// --------------------------------------------------------

static void do_build_draw_buffer(DRWSubdivBuffers *buffers,
                                 GPUVertBuf *subdiv_pos_nor,
                                 GPUIndexBuf *subdiv_tris)
{
  const bool do_smooth_normals = buffers->work_dPdu != NULL && buffers->work_dPdv != NULL;

  const char *defines = NULL;
  if (do_smooth_normals) {
    defines = "#define SMOOTH_NORMALS\n";
  }

  GPUShader *shader = get_subdiv_shader(
      do_smooth_normals ? SHADER_BUFFER_BUILD_SMOOTH : SHADER_BUFFER_BUILD, defines);
  GPU_shader_bind(shader);

  int binding_point = 0;

  /* Inputs */
  GPU_vertbuf_bind_as_ssbo(buffers->work_vertices, binding_point++);
  GPU_vertbuf_bind_as_ssbo(buffers->vert_origindex, binding_point++);
  GPU_vertbuf_bind_as_ssbo(buffers->flags, binding_point++);

  if (do_smooth_normals) {
    GPU_vertbuf_bind_as_ssbo(buffers->work_dPdu, binding_point++);
    GPU_vertbuf_bind_as_ssbo(buffers->work_dPdv, binding_point++);
  }

  /* Outputs */
  GPU_vertbuf_bind_as_ssbo(subdiv_pos_nor, binding_point++);
  GPU_indexbuf_bind_as_ssbo(subdiv_tris, binding_point++);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_lines_buffer(DRWSubdivBuffers *buffers, GPUIndexBuf *lines_indices)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LINES, NULL);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(buffers->edge_origindex, 0);
  GPU_indexbuf_bind_as_ssbo(lines_indices, 1);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_edge_fac_buffer(DRWSubdivBuffers *buffers,
                                     GPUVertBuf *pos_nor,
                                     GPUVertBuf *edge_fac)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_EDGE_FAC, NULL);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(pos_nor, 0);
  GPU_vertbuf_bind_as_ssbo(edge_fac, 1);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_lnor_buffer(DRWSubdivBuffers *buffers, GPUVertBuf *pos_nor, GPUVertBuf *lnor)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LNOR, NULL);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(pos_nor, 0);
  GPU_vertbuf_bind_as_ssbo(lnor, 1);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_points_buffer(DRWSubdivBuffers *buffers, GPUIndexBuf *points)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_POINTS, NULL);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(buffers->vert_origindex, 0);
  GPU_indexbuf_bind_as_ssbo(points, 1);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

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

  if (buffers->work_dPdu && buffers->work_dPdv) {
    OpenSubdiv_BufferInterface dPdu_interface;
    opensubdiv_gpu_buffer_init(&dPdu_interface, buffers->work_dPdu);

    OpenSubdiv_BufferInterface dPdv_interface;
    opensubdiv_gpu_buffer_init(&dPdv_interface, buffers->work_dPdv);

    evaluator->evaluatePatchesLimitFromBuffer(
        evaluator, &patch_coord_interface, &vertex_interface, &dPdu_interface, &dPdv_interface);
  }
  else {
    evaluator->evaluatePatchesLimitFromBuffer(
        evaluator, &patch_coord_interface, &vertex_interface, NULL, NULL);
  }
}

static void do_eval_face_varying_limits(Subdiv *subdiv,
                                        DRWSubdivBuffers *buffers,
                                        GPUVertBuf *face_varying,
                                        uint uv_layers)
{
  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;

  OpenSubdiv_BufferInterface patch_coord_interface;
  opensubdiv_gpu_buffer_init(&patch_coord_interface, buffers->patch_coords);

  OpenSubdiv_BufferInterface face_varying_interface;
  opensubdiv_gpu_buffer_init(&face_varying_interface, face_varying);

  /* Index of the UV layer in the compact buffer. Used UV layers are stored in a single buffer. */
  int buffer_layer_index = 0;

  for (int i = 0; i < MAX_MTFACE; i++) {
    if (uv_layers & (1 << i)) {
      const int offset = (int)buffers->number_of_loops * buffer_layer_index++;
      /* Multiply by 2 since UVs are 2D vectors. */
      face_varying_interface.buffer_offset = offset * 2;

      evaluator->evaluateFaceVaryingFromBuffer(
          evaluator, i, &patch_coord_interface, &face_varying_interface);
    }
  }
}

// --------------------------------------------------------

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
/* -------------------------------------------------------------------- */

static bool has_smooth_shading(const Mesh *mesh)
{
  for (int i = 0; i < mesh->totpoly; i++) {
    const MPoly *mpoly = &mesh->mpoly[i];

    if ((mpoly->flag & ME_SMOOTH) != 0) {
      return true;
    }
  }

  return false;
}

#if 0
EXTRACT_ADD_REQUESTED(VBO, vbo, pos_nor); // done
EXTRACT_ADD_REQUESTED(VBO, vbo, lnor); // done
EXTRACT_ADD_REQUESTED(VBO, vbo, uv); // done
EXTRACT_ADD_REQUESTED(VBO, vbo, tan);
EXTRACT_ADD_REQUESTED(VBO, vbo, vcol);
EXTRACT_ADD_REQUESTED(VBO, vbo, sculpt_data); // no need
EXTRACT_ADD_REQUESTED(VBO, vbo, orco);
EXTRACT_ADD_REQUESTED(VBO, vbo, edge_fac); // done
EXTRACT_ADD_REQUESTED(VBO, vbo, weights);
EXTRACT_ADD_REQUESTED(VBO, vbo, edit_data);
EXTRACT_ADD_REQUESTED(VBO, vbo, edituv_data); // no need
EXTRACT_ADD_REQUESTED(VBO, vbo, edituv_stretch_area); // no need
EXTRACT_ADD_REQUESTED(VBO, vbo, edituv_stretch_angle); // no need
EXTRACT_ADD_REQUESTED(VBO, vbo, mesh_analysis);
EXTRACT_ADD_REQUESTED(VBO, vbo, fdots_pos);
EXTRACT_ADD_REQUESTED(VBO, vbo, fdots_nor);
EXTRACT_ADD_REQUESTED(VBO, vbo, fdots_uv);
EXTRACT_ADD_REQUESTED(VBO, vbo, fdots_edituv_data);
EXTRACT_ADD_REQUESTED(VBO, vbo, poly_idx);
EXTRACT_ADD_REQUESTED(VBO, vbo, edge_idx);
EXTRACT_ADD_REQUESTED(VBO, vbo, vert_idx);
EXTRACT_ADD_REQUESTED(VBO, vbo, fdot_idx); // leave to mesh_extract
EXTRACT_ADD_REQUESTED(VBO, vbo, skin_roots);

EXTRACT_ADD_REQUESTED(IBO, ibo, tris); // done
if (DRW_ibo_requested(mbc->ibo.lines)) { // done
  const MeshExtract *extractor;
  if (mbc->ibo.lines_loose != nullptr) {
    /* Update #lines_loose ibo. */
    extractor = &extract_lines_with_lines_loose;
  }
  else {
    extractor = &extract_lines;
  }
  extractors.append(extractor);
}
else if (DRW_ibo_requested(mbc->ibo.lines_loose)) {
  /* Note: #ibo.lines must have been created first. */
  const MeshExtract *extractor = &extract_lines_loose_only;
  extractors.append(extractor);
}
EXTRACT_ADD_REQUESTED(IBO, ibo, points);
EXTRACT_ADD_REQUESTED(IBO, ibo, fdots);
EXTRACT_ADD_REQUESTED(IBO, ibo, lines_paint_mask);
EXTRACT_ADD_REQUESTED(IBO, ibo, lines_adjacency);
EXTRACT_ADD_REQUESTED(IBO, ibo, edituv_tris);
EXTRACT_ADD_REQUESTED(IBO, ibo, edituv_lines);
EXTRACT_ADD_REQUESTED(IBO, ibo, edituv_points);
EXTRACT_ADD_REQUESTED(IBO, ibo, edituv_fdots);
#endif

static bool draw_subdiv_create_requested_buffers(const Scene *scene,
                                                 Object *ob,
                                                 Mesh *mesh,
                                                 struct MeshBatchCache *batch_cache,
                                                 MeshBufferCache *mbc)
{
  SubsurfModifierData *smd = get_subsurf_modifier(ob);
  BLI_assert(smd);

  const bool is_final_render = DRW_state_is_scene_render();

  SubdivSettings settings;
  BKE_subdiv_settings_init_from_modifier(&settings, smd, is_final_render);

  if (settings.level == 0) {
    return false;
  }

  Mesh *mesh_eval = mesh;
  if (mesh->edit_mesh) {
    mesh_eval = mesh->edit_mesh->mesh_eval_final;
  }

  BKE_modifier_subsurf_ensure_runtime(smd);

  Subdiv *subdiv = BKE_modifier_subsurf_subdiv_descriptor_ensure(smd, &settings, mesh_eval, true);
  if (!subdiv) {
    return false;
  }

  if (!BKE_subdiv_eval_begin_from_mesh(
          subdiv, mesh_eval, NULL, OPENSUBDIV_EVALUATOR_GLSL_COMPUTE)) {
    return false;
  }

  DRWSubdivCache *draw_cache = ensure_draw_cache(subdiv);
  if (!generate_required_cached_data(draw_cache, subdiv, mesh_eval, scene, smd, is_final_render)) {
    return false;
  }

  const bool do_smooth = has_smooth_shading(mesh_eval);
  DRWSubdivBuffers subdiv_buffers = {0};
  initialize_buffers(&subdiv_buffers, draw_cache, do_smooth);

  fprintf(stderr, "HANDLING requests\n");

  if (DRW_vbo_requested(mbc->vbo.pos_nor) || DRW_ibo_requested(mbc->ibo.tris)) {
    fprintf(stderr, "POS NOR requested\n");
    fprintf(stderr, "TRIS requested\n");
    do_eval_patches_limits(subdiv, &subdiv_buffers);

    /* Initialize the output buffers */

    /* Initialise the vertex buffer, it was already allocated. */
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.pos_nor, get_render_format(), subdiv_buffers.number_of_loops);

    /* Initialise the index buffer, it was already allocated, it will be filled on the device. */
    GPU_indexbuf_init_build_on_device(mbc->ibo.tris, subdiv_buffers.number_of_triangles * 3);

    do_build_draw_buffer(&subdiv_buffers, mbc->vbo.pos_nor, mbc->ibo.tris);

    if (mbc->tris_per_mat) {
      fprintf(stderr, "TRIS PER MAT requested\n");
      for (int i = 0; i < batch_cache->mat_len; i++) {
        if (mbc->tris_per_mat[i] == NULL) {
          mbc->tris_per_mat[i] = GPU_indexbuf_calloc();
        }

        // TODO(kevindietrich): per triangle materials
        const int start = 0;
        const int len = subdiv_buffers.number_of_triangles * 3;
        GPU_indexbuf_create_subrange_in_place(mbc->tris_per_mat[i], mbc->ibo.tris, start, len);
      }
    }

    fprintf(stderr, "-- POS NOR points : %d\n", GPU_vertbuf_get_vertex_len(mbc->vbo.pos_nor));
  }

  if (DRW_vbo_requested(mbc->vbo.lnor)) {
    fprintf(stderr, "LNOR requested\n");
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.lnor, get_lnor_format(), subdiv_buffers.number_of_loops);
    do_build_lnor_buffer(&subdiv_buffers, mbc->vbo.pos_nor, mbc->vbo.lnor);

    fprintf(stderr, "-- LNOR points : %d\n", GPU_vertbuf_get_vertex_len(mbc->vbo.lnor));
  }

  if (DRW_ibo_requested(mbc->ibo.lines)) {
    fprintf(stderr, "LINES requested\n");
    GPU_indexbuf_init_build_on_device(mbc->ibo.lines, subdiv_buffers.number_of_loops * 2);
    do_build_lines_buffer(&subdiv_buffers, mbc->ibo.lines);
  }

  if (DRW_vbo_requested(mbc->vbo.edge_fac)) {
    fprintf(stderr, "EDGE FAC requested\n");
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.edge_fac, get_edge_fac_format(), subdiv_buffers.number_of_loops);
    do_build_edge_fac_buffer(&subdiv_buffers, mbc->vbo.pos_nor, mbc->vbo.edge_fac);

    fprintf(stderr, "-- EDGE FAC points : %d\n", GPU_vertbuf_get_vertex_len(mbc->vbo.edge_fac));
  }

  if (DRW_vbo_requested(mbc->vbo.vert_idx)) {
    fprintf(stderr, "VERT IDX requested\n");
  }

  if (DRW_vbo_requested(mbc->vbo.edge_idx)) {
    fprintf(stderr, "EDGE IDX requested\n");
  }

  if (DRW_vbo_requested(mbc->vbo.poly_idx)) {
    fprintf(stderr, "POLY IDX requested\n");
  }

  if (DRW_ibo_requested(mbc->ibo.points)) {
    fprintf(stderr, "POINTS requested\n");
    GPU_indexbuf_init_build_on_device(mbc->ibo.points, subdiv_buffers.number_of_loops);
    do_build_points_buffer(&subdiv_buffers, mbc->ibo.points);
  }

  if (DRW_ibo_requested(mbc->ibo.edituv_tris)) {
    fprintf(stderr, "edituv_tris requested\n");
  }

  if (DRW_ibo_requested(mbc->ibo.edituv_lines)) {
    fprintf(stderr, "edituv_lines requested\n");
  }

  if (DRW_ibo_requested(mbc->ibo.edituv_points)) {
    fprintf(stderr, "edituv_points requested\n");
  }

  if (DRW_ibo_requested(mbc->ibo.edituv_fdots)) {
    fprintf(stderr, "edituv_fdots requested\n");
  }

  if (DRW_ibo_requested(mbc->ibo.lines_adjacency)) {
    fprintf(stderr, "lines_adjacency requested\n");
  }

//  if (DRW_vbo_requested(mbc->vbo.edit_data)) {
//    fprintf(stderr, "edit_data requested\n");
//    //GPU_vertbuf_init_build_on_device(mbc->vbo.edit_data, get_edit_data_format(), subdiv_buffers.number_of_loops);
//    GPU_vertbuf_init_with_format(mbc->vbo.edit_data, get_edit_data_format());
//    GPU_vertbuf_data_alloc(mbc->vbo.edit_data, subdiv_buffers.number_of_loops);

//    memcpy(GPU_vertbuf_get_data(mbc->vbo.edit_data), subdiv_buffers.edit_data, sizeof(EditLoopData) * subdiv_buffers.number_of_loops);

//    MEM_freeN(subdiv_buffers.edit_data);
//  }

  if (DRW_vbo_requested(mbc->vbo.uv)) {
    fprintf(stderr, "UV requested\n");
    uint uv_layers;
    initialize_uv_buffer(
        mbc->vbo.uv, batch_cache, &mesh_eval->ldata, subdiv_buffers.number_of_loops, &uv_layers);

    if (uv_layers != 0) {
      do_eval_face_varying_limits(subdiv, &subdiv_buffers, mbc->vbo.uv, uv_layers);
    }

    fprintf(stderr, "-- UV points : %d\n", GPU_vertbuf_get_vertex_len(mbc->vbo.uv));
  }

  free_buffers(&subdiv_buffers);
  return true;
}

void DRW_create_subdivision(const Scene *scene,
                            Object *ob,
                            Mesh *mesh,
                            struct MeshBatchCache *batch_cache,
                            MeshBufferCache *mbc)
{
  if (!draw_subdiv_create_requested_buffers(scene, ob, mesh, batch_cache, mbc)) {
    // TODO(kevindietrich): fill the regular mesh buffer cache.
    // We should still initialize the buffers.
    // TODO(kevindietrich): Error: Not freed memory blocks: 2, total unfreed memory 0.000000 MB
    GPU_vertbuf_init_build_on_device(mbc->vbo.pos_nor, get_render_format(), 0);
    GPU_indexbuf_init_build_on_device(mbc->ibo.tris, 0);
  }
}

void DRW_subdiv_free(void)
{
  for (int i = 0; i < NUM_SHADERS; ++i) {
    GPU_shader_free(g_subdiv_shaders[i]);
  }
}
