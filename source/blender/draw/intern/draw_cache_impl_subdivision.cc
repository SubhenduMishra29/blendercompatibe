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
#include "BKE_object.h"
#include "BKE_scene.h"
#include "BKE_subdiv.h"
#include "BKE_subdiv_eval.h"
#include "BKE_subdiv_foreach.h"
#include "BKE_subdiv_mesh.h"

#include "BLI_linklist.h"

#include "BLI_string.h"

#include "PIL_time.h"

#include "DRW_engine.h"
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
#include "draw_cache_impl.h"
#include "draw_cache_inline.h"
#include "mesh_extractors/extract_mesh.h"

extern "C" char datatoc_common_subdiv_custom_data_interp_comp_glsl[];
extern "C" char datatoc_common_subdiv_ibo_lines_comp_glsl[];
extern "C" char datatoc_common_subdiv_ibo_points_comp_glsl[];
extern "C" char datatoc_common_subdiv_ibo_tris_comp_glsl[];
extern "C" char datatoc_common_subdiv_lib_glsl[];
extern "C" char datatoc_common_subdiv_normals_accumulate_comp_glsl[];
extern "C" char datatoc_common_subdiv_normals_finalize_comp_glsl[];
extern "C" char datatoc_common_subdiv_patch_evaluation_comp_glsl[];
extern "C" char datatoc_common_subdiv_vbo_edge_fac_comp_glsl[];
extern "C" char datatoc_common_subdiv_vbo_lnor_comp_glsl[];

enum {
  SHADER_BUFFER_LINES,
  SHADER_BUFFER_LINES_LOOSE,
  SHADER_BUFFER_EDGE_FAC,
  SHADER_BUFFER_LNOR,
  SHADER_BUFFER_TRIS,
  SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS,
  SHADER_BUFFER_NORMALS_ACCUMULATE,
  SHADER_BUFFER_NORMALS_FINALIZE,
  SHADER_PATCH_EVALUATION,
  SHADER_PATCH_EVALUATION_LIMIT_NORMALS,
  SHADER_PATCH_EVALUATION_FVAR,
  SHADER_PATCH_EVALUATION_FACE_DOTS,
  SHADER_COMP_CUSTOM_DATA_INTERP_1D,
  SHADER_COMP_CUSTOM_DATA_INTERP_4D,

  NUM_SHADERS,
};

static GPUShader *g_subdiv_shaders[NUM_SHADERS];

static const char *get_shader_code(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_LINES:
    case SHADER_BUFFER_LINES_LOOSE: {
      return datatoc_common_subdiv_ibo_lines_comp_glsl;
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return datatoc_common_subdiv_vbo_edge_fac_comp_glsl;
    }
    case SHADER_BUFFER_LNOR: {
      return datatoc_common_subdiv_vbo_lnor_comp_glsl;
    }
    case SHADER_BUFFER_TRIS:
    case SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS: {
      return datatoc_common_subdiv_ibo_tris_comp_glsl;
    }
    case SHADER_BUFFER_NORMALS_ACCUMULATE: {
      return datatoc_common_subdiv_normals_accumulate_comp_glsl;
    }
    case SHADER_BUFFER_NORMALS_FINALIZE: {
      return datatoc_common_subdiv_normals_finalize_comp_glsl;
    }
    case SHADER_PATCH_EVALUATION:
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS:
    case SHADER_PATCH_EVALUATION_FVAR:
    case SHADER_PATCH_EVALUATION_FACE_DOTS: {
      return datatoc_common_subdiv_patch_evaluation_comp_glsl;
    }
    case SHADER_COMP_CUSTOM_DATA_INTERP_1D:
    case SHADER_COMP_CUSTOM_DATA_INTERP_4D: {
      return datatoc_common_subdiv_custom_data_interp_comp_glsl;
    }
  }
  return nullptr;
}

static const char *get_shader_name(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_LINES: {
      return "subdiv lines build";
    }
    case SHADER_BUFFER_LINES_LOOSE: {
      return "subdiv lines loose build";
    }
    case SHADER_BUFFER_LNOR: {
      return "subdiv lnor build";
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return "subdiv edge fac build";
    }
    case SHADER_BUFFER_TRIS:
    case SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS: {
      return "subdiv tris";
    }
    case SHADER_BUFFER_NORMALS_ACCUMULATE: {
      return "subdiv normals accumulate";
    }
    case SHADER_BUFFER_NORMALS_FINALIZE: {
      return "subdiv normals finalize";
    }
    case SHADER_PATCH_EVALUATION: {
      return "subdiv patch evaluation";
    }
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS: {
      return "subdiv patch evaluation limit normals";
    }
    case SHADER_PATCH_EVALUATION_FVAR: {
      return "subdiv patch evaluation face-varying";
    }
    case SHADER_PATCH_EVALUATION_FACE_DOTS: {
      return "subdiv patch evaluation face dots";
    }
    case SHADER_COMP_CUSTOM_DATA_INTERP_1D: {
      return "subdiv custom data interp 1D";
    }
    case SHADER_COMP_CUSTOM_DATA_INTERP_4D: {
      return "subdiv custom data interp 4D";
    }
  }
  return nullptr;
}

static GPUShader *get_patch_evaluation_shader(int shader_type)
{
  if (g_subdiv_shaders[shader_type] == nullptr) {
    const char *compute_code = get_shader_code(shader_type);

    const char *defines = nullptr;
    if (shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define LIMIT_NORMALS\n";
    }
    else if (shader_type == SHADER_PATCH_EVALUATION_FVAR) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define FVAR_EVALUATION\n";
    }
    else if (shader_type == SHADER_PATCH_EVALUATION_FACE_DOTS) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define FDOTS_EVALUATION\n";
    }
    else {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n";
    }

    /* Merge OpenSubdiv library code with our own library code. */
    const char *patch_basis_source = openSubdiv_getGLSLPatchBasisSource();
    const char *subdiv_lib_code = datatoc_common_subdiv_lib_glsl;
    char *library_code = static_cast<char *>(
        MEM_mallocN(strlen(patch_basis_source) + strlen(subdiv_lib_code) + 1,
                    "subdiv patch evaluation library code"));
    library_code[0] = '\0';
    strcat(library_code, patch_basis_source);
    strcat(library_code, subdiv_lib_code);

    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, library_code, defines, get_shader_name(shader_type));

    MEM_freeN(library_code);
  }

  return g_subdiv_shaders[shader_type];
}

static GPUShader *get_subdiv_shader(int shader_type, const char *defines)
{
  if (shader_type == SHADER_PATCH_EVALUATION ||
      shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS ||
      shader_type == SHADER_PATCH_EVALUATION_FVAR ||
      shader_type == SHADER_PATCH_EVALUATION_FACE_DOTS) {
    return get_patch_evaluation_shader(shader_type);
  }
  if (g_subdiv_shaders[shader_type] == nullptr) {
    const char *compute_code = get_shader_code(shader_type);
    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, datatoc_common_subdiv_lib_glsl, defines, get_shader_name(shader_type));
  }
  return g_subdiv_shaders[shader_type];
}

typedef struct CompressedPatchCoord {
  int ptex_face_index;
  /* UV coordinate encoded as u << 16 | v, where u and v are quantized on 16-bits. */
  unsigned int encoded_uv;
} CompressedPatchCoord;

MINLINE CompressedPatchCoord make_patch_coord(int ptex_face_index, float u, float v)
{
  CompressedPatchCoord patch_coord = {
      ptex_face_index,
      (static_cast<unsigned int>(u * 65535.0f) << 16) | static_cast<unsigned int>(v * 65535.0f),
  };
  return patch_coord;
}

/* Vertex format used for the #CompressedPatchCoord. */
static GPUVertFormat *get_blender_patch_coords_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* WARNING! Adjust #CompressedPatchCoord accordingly. */
    GPU_vertformat_attr_add(&format, "ptex_face_index", GPU_COMP_U32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "uv", GPU_COMP_U32, 1, GPU_FETCH_INT);
  }
  return &format;
}

static GPUVertFormat *get_origindex_format(void)
{
  static GPUVertFormat format;
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "color", GPU_COMP_U32, 1, GPU_FETCH_INT);
  }
  return &format;
}

// --------------------------------------------------------

static uint tris_count_from_number_of_loops(const uint number_of_loops)
{
  const uint32_t number_of_quads = number_of_loops / 4;
  return number_of_quads * 2;
}

/* -------------------------------------------------------------------- */
/** \name Utilities to build a GPUVertBuf from an origindex buffer).
 * \{ */

void draw_subdiv_init_origindex_buffer(GPUVertBuf *buffer,
                                       int *vert_origindex,
                                       uint num_loops,
                                       uint loose_len)
{
  GPU_vertbuf_init_with_format_ex(buffer, get_origindex_format(), GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(buffer, num_loops + loose_len);

  int *vbo_data = (int *)GPU_vertbuf_get_data(buffer);
  memcpy(vbo_data, vert_origindex, num_loops * sizeof(int));
}

GPUVertBuf *draw_subdiv_build_origindex_buffer(int *vert_origindex, uint num_loops)
{
  GPUVertBuf *buffer = GPU_vertbuf_calloc();
  draw_subdiv_init_origindex_buffer(buffer, vert_origindex, num_loops, 0);
  return buffer;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utilities for DRWPatchMap.
 * \{ */

static void draw_patch_map_build(DRWPatchMap *gpu_patch_map, Subdiv *subdiv)
{
  GPUVertBuf *patch_map_handles = nullptr;
  GPUVertBuf *patch_map_quadtree = nullptr;
  int min_patch_face = 0;
  int max_patch_face = 0;
  int max_depth = 0;
  int patches_are_triangular = 0;

  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;
  evaluator->getPatchMap(evaluator,
                         &patch_map_handles,
                         &patch_map_quadtree,
                         &min_patch_face,
                         &max_patch_face,
                         &max_depth,
                         &patches_are_triangular);

  gpu_patch_map->patch_map_handles = patch_map_handles;
  gpu_patch_map->patch_map_quadtree = patch_map_quadtree;
  gpu_patch_map->min_patch_face = min_patch_face;
  gpu_patch_map->max_patch_face = max_patch_face;
  gpu_patch_map->max_depth = max_depth;
  gpu_patch_map->patches_are_triangular = patches_are_triangular;
}

static void draw_patch_map_free(DRWPatchMap *gpu_patch_map)
{
  GPU_VERTBUF_DISCARD_SAFE(gpu_patch_map->patch_map_handles);
  GPU_VERTBUF_DISCARD_SAFE(gpu_patch_map->patch_map_quadtree);
  gpu_patch_map->min_patch_face = 0;
  gpu_patch_map->max_patch_face = 0;
  gpu_patch_map->max_depth = 0;
  gpu_patch_map->patches_are_triangular = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
 * \{ */

static void draw_subdiv_cache_free_material_data(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->polygon_mat_offset);
  MEM_SAFE_FREE(cache->mat_start);
  MEM_SAFE_FREE(cache->mat_end);
}

static void draw_subdiv_free_edit_mode_cache(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->verts_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->edges_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->fdots_patch_coords);
}

static void draw_subdiv_cache_free(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->patch_coords);
  GPU_VERTBUF_DISCARD_SAFE(cache->face_ptex_offset_buffer);
  GPU_VERTBUF_DISCARD_SAFE(cache->subdiv_polygon_offset_buffer);
  GPU_VERTBUF_DISCARD_SAFE(cache->extra_coarse_face_data);
  MEM_SAFE_FREE(cache->subdiv_loop_subdiv_vert_index);
  MEM_SAFE_FREE(cache->subdiv_loop_poly_index);
  MEM_SAFE_FREE(cache->point_indices);
  MEM_SAFE_FREE(cache->subdiv_polygon_offset);
  GPU_VERTBUF_DISCARD_SAFE(cache->subdiv_vertex_face_adjacency_offsets);
  GPU_VERTBUF_DISCARD_SAFE(cache->subdiv_vertex_face_adjacency);
  cache->resolution = 0;
  cache->num_subdiv_loops = 0;
  cache->num_coarse_poly = 0;
  cache->num_subdiv_quads = 0;
  draw_subdiv_free_edit_mode_cache(cache);
  draw_subdiv_cache_free_material_data(cache);
  draw_patch_map_free(&cache->gpu_patch_map);
  if (cache->ubo) {
    GPU_uniformbuf_free(cache->ubo);
    cache->ubo = nullptr;
  }
}

static void draw_subdiv_cache_update_extra_coarse_face_data(DRWSubdivCache *cache, Mesh *mesh)
{
  if (cache->extra_coarse_face_data == nullptr) {
    cache->extra_coarse_face_data = GPU_vertbuf_calloc();
    static GPUVertFormat format;
    if (format.attr_len == 0) {
      GPU_vertformat_attr_add(&format, "data", GPU_COMP_U32, 1, GPU_FETCH_INT);
    }
    GPU_vertbuf_init_with_format_ex(cache->extra_coarse_face_data, &format, GPU_USAGE_DYNAMIC);
    GPU_vertbuf_data_alloc(cache->extra_coarse_face_data, mesh->totpoly);
  }

  uint32_t *flags_data = (uint32_t *)(GPU_vertbuf_get_data(cache->extra_coarse_face_data));

  for (int i = 0; i < mesh->totpoly; i++) {
    uint32_t flag = 0;
    if ((mesh->mpoly[i].flag & ME_SMOOTH) != 0) {
      flag = 1;
    }
    flags_data[i] = (uint)(mesh->mpoly[i].loopstart) | (flag << 31);
  }

  /* Make sure updated data is reuploaded. */
  GPU_vertbuf_tag_dirty(cache->extra_coarse_face_data);
}

static void free_draw_cache_from_subdiv_cb(void *ptr)
{
  DRWSubdivCache *cache = (DRWSubdivCache *)(ptr);
  draw_subdiv_cache_free(cache);
  MEM_freeN(cache);
}

static DRWSubdivCache *ensure_draw_cache(Subdiv *subdiv)
{
  DRWSubdivCache *draw_cache = static_cast<DRWSubdivCache *>(subdiv->draw_cache);
  if (draw_cache == nullptr) {
    draw_cache = static_cast<DRWSubdivCache *>(
        MEM_callocN(sizeof(DRWSubdivCache), "DRWSubdivCache"));
  }
  subdiv->draw_cache = draw_cache;
  subdiv->free_draw_cache = free_draw_cache_from_subdiv_cb;
  return draw_cache;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Traverse the uniform subdivision grid over coarse faces and gather useful information for
 * building the draw buffers on the GPU. We primarily gather the patch coordinates for all
 * subdivision faces, as well as the original coarse indices for each subdivision element (vertex,
 * face, or edge) which directly maps to its coarse counterpart (note that all subdivision faces
 * map to a coarse face). This information will then be cached in #DRWSubdivCache for subsequent
 * reevaluations, as long as the topology does not change.
 * \{ */

typedef struct DRWCacheBuildingContext {
  const Mesh *coarse_mesh;
  const SubdivToMeshSettings *settings;

  DRWSubdivCache *cache;

  /* Pointers into DRWSubdivCache buffers for easier access during traversal. */
  CompressedPatchCoord *patch_coords;
  int *subdiv_loop_vert_index;
  int *subdiv_loop_subdiv_vert_index;
  int *subdiv_loop_edge_index;
  int *subdiv_loop_poly_index;
  int *point_indices;

  /* Temporary buffers used during traversal. */
  int *vert_origindex_map;
  int *edge_origindex_map;
} DRWCacheBuildingContext;

static bool draw_subdiv_topology_info_cb(const SubdivForeachContext *foreach_context,
                                         const int num_vertices,
                                         const int num_edges,
                                         const int num_loops,
                                         const int num_polygons,
                                         const int *subdiv_polygon_offset)
{
  if (num_loops == 0) {
    return false;
  }

  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  DRWSubdivCache *cache = ctx->cache;

  /* Set topology information. */
  cache->num_subdiv_edges = (uint)num_edges;
  cache->num_subdiv_loops = (uint)num_loops;
  cache->num_subdiv_vertis = (uint)num_vertices;
  cache->num_subdiv_quads = (uint)num_polygons;
  cache->subdiv_polygon_offset = static_cast<int *>(MEM_dupallocN(subdiv_polygon_offset));

  /* Initialize cache buffers, prefer dynamic usage so we can reuse memory on the host even after
   * it was sent to the device, since we may use the data while building other buffers on the CPU
   * side. */
  cache->patch_coords = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      cache->patch_coords, get_blender_patch_coords_format(), GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(cache->patch_coords, cache->num_subdiv_loops);

  cache->verts_orig_index = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      cache->verts_orig_index, get_origindex_format(), GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(cache->verts_orig_index, cache->num_subdiv_loops);

  cache->edges_orig_index = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      cache->edges_orig_index, get_origindex_format(), GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(cache->edges_orig_index, cache->num_subdiv_loops);

  cache->subdiv_loop_subdiv_vert_index = static_cast<int *>(
      MEM_mallocN(cache->num_subdiv_loops * sizeof(int), "subdiv_loop_subdiv_vert_index"));

  cache->subdiv_loop_poly_index = static_cast<int *>(
      MEM_mallocN(cache->num_subdiv_loops * sizeof(int), "subdiv_loop_poly_index"));

  cache->point_indices = static_cast<int *>(
      MEM_mallocN(cache->num_subdiv_vertis * sizeof(int), "point_indices"));
  for (int i = 0; i < num_vertices; i++) {
    cache->point_indices[i] = -1;
  }

  /* Initialize context pointers and temporary buffers. */
  ctx->patch_coords = (CompressedPatchCoord *)GPU_vertbuf_get_data(cache->patch_coords);
  ctx->subdiv_loop_vert_index = (int *)GPU_vertbuf_get_data(cache->verts_orig_index);
  ctx->subdiv_loop_edge_index = (int *)GPU_vertbuf_get_data(cache->edges_orig_index);
  ctx->subdiv_loop_subdiv_vert_index = cache->subdiv_loop_subdiv_vert_index;
  ctx->subdiv_loop_poly_index = cache->subdiv_loop_poly_index;
  ctx->point_indices = cache->point_indices;

  ctx->vert_origindex_map = static_cast<int *>(
      MEM_mallocN(cache->num_subdiv_vertis * sizeof(int), "subdiv_vert_origindex_map"));
  for (int i = 0; i < num_vertices; i++) {
    ctx->vert_origindex_map[i] = -1;
  }

  ctx->edge_origindex_map = static_cast<int *>(
      MEM_mallocN(cache->num_subdiv_edges * sizeof(int), "subdiv_edge_origindex_map"));
  for (int i = 0; i < num_edges; i++) {
    ctx->edge_origindex_map[i] = -1;
  }

  return true;
}

static void draw_subdiv_vertex_corner_cb(const SubdivForeachContext *foreach_context,
                                         void *UNUSED(tls),
                                         const int UNUSED(ptex_face_index),
                                         const float UNUSED(u),
                                         const float UNUSED(v),
                                         const int coarse_vertex_index,
                                         const int UNUSED(coarse_poly_index),
                                         const int UNUSED(coarse_corner),
                                         const int subdiv_vertex_index)
{
  BLI_assert(coarse_vertex_index != ORIGINDEX_NONE);
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->vert_origindex_map[subdiv_vertex_index] = coarse_vertex_index;
}

static void draw_subdiv_vertex_edge_cb(const SubdivForeachContext *UNUSED(foreach_context),
                                       void *UNUSED(tls_v),
                                       const int UNUSED(ptex_face_index),
                                       const float UNUSED(u),
                                       const float UNUSED(v),
                                       const int UNUSED(coarse_edge_index),
                                       const int UNUSED(coarse_poly_index),
                                       const int UNUSED(coarse_corner),
                                       const int UNUSED(subdiv_vertex_index))
{
  /* Required if SubdivForeachContext.vertex_corner is also set. */
}

static void draw_subdiv_edge_cb(const SubdivForeachContext *foreach_context,
                                void *UNUSED(tls),
                                const int coarse_edge_index,
                                const int subdiv_edge_index,
                                const int UNUSED(subdiv_v1),
                                const int UNUSED(subdiv_v2))
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->edge_origindex_map[subdiv_edge_index] = coarse_edge_index;
}

static void draw_subdiv_loop_cb(const SubdivForeachContext *foreach_context,
                                void *UNUSED(tls_v),
                                const int ptex_face_index,
                                const float u,
                                const float v,
                                const int UNUSED(coarse_loop_index),
                                const int coarse_poly_index,
                                const int UNUSED(coarse_corner),
                                const int subdiv_loop_index,
                                const int subdiv_vertex_index,
                                const int subdiv_edge_index)
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->patch_coords[subdiv_loop_index] = make_patch_coord(ptex_face_index, u, v);

  const int coarse_vertex_index = ctx->vert_origindex_map[subdiv_vertex_index];

  if (coarse_vertex_index != -1) {
    ctx->point_indices[coarse_vertex_index] = subdiv_loop_index;
  }

  ctx->subdiv_loop_subdiv_vert_index[subdiv_loop_index] = subdiv_vertex_index;
  /* For now index the subdiv_edge_index, it will be replaced by the actual coarse edge index
   * at the end of the traversal as some edges are only then traversed. */
  ctx->subdiv_loop_edge_index[subdiv_loop_index] = subdiv_edge_index;
  ctx->subdiv_loop_poly_index[subdiv_loop_index] = coarse_poly_index;
  ctx->subdiv_loop_vert_index[subdiv_loop_index] = coarse_vertex_index;
}

static void draw_subdiv_foreach_callbacks(SubdivForeachContext *foreach_context)
{
  memset(foreach_context, 0, sizeof(*foreach_context));
  foreach_context->topology_info = draw_subdiv_topology_info_cb;
  foreach_context->loop = draw_subdiv_loop_cb;
  foreach_context->edge = draw_subdiv_edge_cb;
  foreach_context->vertex_corner = draw_subdiv_vertex_corner_cb;
  foreach_context->vertex_edge = draw_subdiv_vertex_edge_cb;
}

static void do_subdiv_traversal(DRWCacheBuildingContext *cache_building_context, Subdiv *subdiv)
{
  SubdivForeachContext foreach_context;
  draw_subdiv_foreach_callbacks(&foreach_context);
  foreach_context.user_data = cache_building_context;

  BKE_subdiv_foreach_subdiv_geometry(subdiv,
                                     &foreach_context,
                                     cache_building_context->settings,
                                     cache_building_context->coarse_mesh);

  /* Now that traversal is done, we can set up the right original indices for the loop-to-edge map.
   */
  for (int i = 0; i < cache_building_context->cache->num_subdiv_loops; i++) {
    cache_building_context->subdiv_loop_edge_index[i] =
        cache_building_context
            ->edge_origindex_map[cache_building_context->subdiv_loop_edge_index[i]];
  }
}

static GPUVertBuf *gpu_vertbuf_create_from_format(GPUVertFormat *format, uint len)
{
  GPUVertBuf *verts = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format(verts, format);
  GPU_vertbuf_data_alloc(verts, len);
  return verts;
}

/* Build maps to hold enough information to tell which face is adjacent to which vertex; those will
 * be used for computing normals if limit surfaces are unavailable. */
static void build_vertex_face_adjacency_maps(DRWSubdivCache *cache)
{
  /* +1 so that we do not require a special case for the last vertex, this extra offset will
   * contain the total number of adjacent faces. */
  cache->subdiv_vertex_face_adjacency_offsets = gpu_vertbuf_create_from_format(
      get_origindex_format(), cache->num_subdiv_vertis + 1);

  int *vertex_offsets = (int *)GPU_vertbuf_get_data(cache->subdiv_vertex_face_adjacency_offsets);
  memset(vertex_offsets, 0, sizeof(int) * cache->num_subdiv_vertis + 1);

  for (int i = 0; i < cache->num_subdiv_loops; i++) {
    vertex_offsets[cache->subdiv_loop_subdiv_vert_index[i]]++;
  }

  int ofs = vertex_offsets[0];
  vertex_offsets[0] = 0;
  for (uint i = 1; i < cache->num_subdiv_vertis + 1; i++) {
    int tmp = vertex_offsets[i];
    vertex_offsets[i] = ofs;
    ofs += tmp;
  }

  cache->subdiv_vertex_face_adjacency = gpu_vertbuf_create_from_format(get_origindex_format(),
                                                                       cache->num_subdiv_loops);
  int *adjacent_faces = (int *)GPU_vertbuf_get_data(cache->subdiv_vertex_face_adjacency);
  int *tmp_set_faces = static_cast<int *>(
      MEM_callocN(sizeof(int) * cache->num_subdiv_vertis, "tmp subdiv vertex offset"));

  for (int i = 0; i < cache->num_subdiv_loops / 4; i++) {
    for (int j = 0; j < 4; j++) {
      const int subdiv_vertex = cache->subdiv_loop_subdiv_vert_index[i * 4 + j];
      int first_face_offset = vertex_offsets[subdiv_vertex] + tmp_set_faces[subdiv_vertex];
      adjacent_faces[first_face_offset] = i;
      tmp_set_faces[subdiv_vertex] += 1;
    }
  }

  MEM_freeN(tmp_set_faces);
}

static bool draw_subdiv_build_cache(DRWSubdivCache *cache,
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
    /* Resolution changed, we need to rebuild, free any existing cached data. */
    draw_subdiv_cache_free(cache);
  }

  /* If the resolution between the cache and the settings match for some reason, check if the patch
   * coordinates were not already generated. Those coordinates are specific to the resolution, so
   * they should be null either after initialization, or after freeing if the resolution (or some
   * other subdivision setting) changed.
   */
  if (cache->patch_coords != nullptr) {
    return true;
  }

  DRWCacheBuildingContext cache_building_context;
  cache_building_context.coarse_mesh = mesh_eval;
  cache_building_context.settings = &to_mesh_settings;
  cache_building_context.cache = cache;

  do_subdiv_traversal(&cache_building_context, subdiv);
  if (cache->num_subdiv_loops == 0) {
    /* Either the traversal failed, or we have an empty mesh, either way we cannot go any further.
     * The subdiv_polygon_offset cannot then be reliably stored in the cache, so free it directly.
     */
    MEM_SAFE_FREE(cache->subdiv_polygon_offset);
    return false;
  }

  /* Build buffers for the PatchMap. */
  draw_patch_map_build(&cache->gpu_patch_map, subdiv);

  cache->face_ptex_offset = BKE_subdiv_face_ptex_offset_get(subdiv);

  // Build patch coordinates for all the face dots
  cache->fdots_patch_coords = gpu_vertbuf_create_from_format(get_blender_patch_coords_format(),
                                                             mesh_eval->totpoly);
  CompressedPatchCoord *blender_fdots_patch_coords = (CompressedPatchCoord *)GPU_vertbuf_get_data(
      cache->fdots_patch_coords);
  for (int i = 0; i < mesh_eval->totpoly; i++) {
    const int ptex_face_index = cache->face_ptex_offset[i];
    if (mesh_eval->mpoly[i].totloop == 4) {
      /* For quads, the center coordinate of the coarse face has `u = v = 0.5`. */
      blender_fdots_patch_coords[i] = make_patch_coord(ptex_face_index, 0.5f, 0.5f);
    }
    else {
      /* For N-gons, since they are split into quads from the center, and since the center is
       * chosen to be the top right corner of each quad, the center coordinate of the coarse face
       * is any one of those top right corners with `u = v = 1.0`. */
      blender_fdots_patch_coords[i] = make_patch_coord(ptex_face_index, 1.0f, 1.0f);
    }
  }

  cache->resolution = to_mesh_settings.resolution;

  cache->subdiv_polygon_offset_buffer = draw_subdiv_build_origindex_buffer(
      cache->subdiv_polygon_offset, mesh_eval->totpoly);

  cache->face_ptex_offset_buffer = draw_subdiv_build_origindex_buffer(cache->face_ptex_offset,
                                                                      mesh_eval->totpoly + 1);
  cache->num_coarse_poly = mesh_eval->totpoly;
  cache->point_indices = cache_building_context.point_indices;

  build_vertex_face_adjacency_maps(cache);

  /* Cleanup. */
  MEM_freeN(cache_building_context.vert_origindex_map);
  MEM_freeN(cache_building_context.edge_origindex_map);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWSubdivUboStorage, common uniforms for the various shaders.
 * \{ */

typedef struct DRWSubdivUboStorage {
  /* Offsets in the buffers data where the source and destination data start. */
  int src_offset;
  int dst_offset;

  /* Parameters for the DRWPatchMap. */
  int min_patch_face;
  int max_patch_face;
  int max_depth;
  int patches_are_triangular;

  /* Coarse topology information. */
  int coarse_poly_count;
  uint edge_loose_offset;

  /* Refined topology information. */
  uint num_subdiv_loops;

  /* Subdivision settings, is int in C but bool in the GLSL code, as there, bools have the same
   * size as ints, so we should use int in C to ensure that the size of the structure is what GLSL
   * expects. */
  int optimal_display;
} DRWSubdivUboStorage;

static void draw_subdiv_init_ubo_storage(const DRWSubdivCache *cache,
                                         DRWSubdivUboStorage *ubo,
                                         const int src_offset,
                                         const int dst_offset)
{
  ubo->src_offset = src_offset;
  ubo->dst_offset = dst_offset;
  ubo->min_patch_face = cache->gpu_patch_map.min_patch_face;
  ubo->max_patch_face = cache->gpu_patch_map.max_patch_face;
  ubo->max_depth = cache->gpu_patch_map.max_depth;
  ubo->patches_are_triangular = cache->gpu_patch_map.patches_are_triangular;
  ubo->coarse_poly_count = cache->num_coarse_poly;
  ubo->optimal_display = cache->optimal_display;
  ubo->num_subdiv_loops = cache->num_subdiv_loops;
  ubo->edge_loose_offset = cache->num_subdiv_loops * 2;
}

static void draw_subdiv_ubo_update_and_bind(const DRWSubdivCache *cache,
                                            GPUShader *shader,
                                            const int src_offset,
                                            const int dst_offset)
{
  DRWSubdivUboStorage storage;
  draw_subdiv_init_ubo_storage(cache, &storage, src_offset, dst_offset);

  if (!cache->ubo) {
    const_cast<DRWSubdivCache *>(cache)->ubo = GPU_uniformbuf_create_ex(
        sizeof(DRWSubdivUboStorage), &storage, "DRWSubdivUboStorage");
  }

  GPU_uniformbuf_update(cache->ubo, &storage);

  const int location = GPU_shader_get_uniform_block(shader, "shader_data");
  GPU_uniformbuf_bind(cache->ubo, location);
}

/** \} */

// --------------------------------------------------------

#define PATCH_EVALUATION_WORK_GROUP_SIZE 64
static uint get_patch_evaluation_work_group_size(uint elements)
{
  return divide_ceil_u(elements, PATCH_EVALUATION_WORK_GROUP_SIZE);
}

void draw_subdiv_extract_pos_nor(const DRWSubdivCache *cache,
                                 GPUVertBuf *pos_nor,
                                 const bool do_limit_normals)
{
  Subdiv *subdiv = cache->subdiv;
  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;

  GPUVertBuf *src_buffer = evaluator->getWrappedSrcBuffer(evaluator);
  GPUVertBuf *patch_arrays_buffer = evaluator->getPatchArraysBuffer(evaluator);
  GPUVertBuf *patch_index_buffer = evaluator->getWrappedPatchIndexBuffer(evaluator);
  GPUVertBuf *patch_param_buffer = evaluator->getWrappedPatchParamBuffer(evaluator);

  GPUShader *shader = get_patch_evaluation_shader(
      do_limit_normals ? SHADER_PATCH_EVALUATION_LIMIT_NORMALS : SHADER_PATCH_EVALUATION);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(cache->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(cache->verts_orig_index, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(pos_nor, 8);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(cache->num_subdiv_quads), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

void draw_subdiv_extract_uvs(const DRWSubdivCache *cache,
                             GPUVertBuf *uvs,
                             const int face_varying_channel,
                             const int dst_offset)
{
  Subdiv *subdiv = cache->subdiv;
  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;

  int fvar_buffer_offset = 0;
  GPUVertBuf *src_buffer = evaluator->getWrappedFVarSrcBuffer(
      evaluator, face_varying_channel, &fvar_buffer_offset);

  GPUVertBuf *patch_arrays_buffer = evaluator->getFVarPatchArraysBuffer(evaluator,
                                                                        face_varying_channel);

  GPUVertBuf *patch_index_buffer = evaluator->getWrappedFVarPatchIndexBuffer(evaluator,
                                                                             face_varying_channel);

  GPUVertBuf *patch_param_buffer = evaluator->getWrappedFVarPatchParamBuffer(evaluator,
                                                                             face_varying_channel);

  GPUShader *shader = get_patch_evaluation_shader(SHADER_PATCH_EVALUATION_FVAR);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(cache->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(cache->verts_orig_index, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(uvs, 8);

  /* The buffer offset has the stride baked in (which is 2 as we have UVs) so remove the stride by
   * dividing by 2 */
  const int src_offset = fvar_buffer_offset / 2;
  draw_subdiv_ubo_update_and_bind(cache, shader, src_offset, dst_offset);

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(cache->num_subdiv_quads), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

void draw_subdiv_interp_custom_data(const DRWSubdivCache *cache,
                                    GPUVertBuf *src_data,
                                    GPUVertBuf *dst_data,
                                    int dimensions,
                                    int dst_offset)
{
  GPUShader *shader = nullptr;

  if (dimensions == 1) {
    shader = get_subdiv_shader(SHADER_COMP_CUSTOM_DATA_INTERP_1D,
                               "#define SUBDIV_POLYGON_OFFSET\n#define DIMENSIONS 1\n");
  }
  else if (dimensions == 4) {
    shader = get_subdiv_shader(
        SHADER_COMP_CUSTOM_DATA_INTERP_4D,
        "#define SUBDIV_POLYGON_OFFSET\n#define DIMENSIONS 4\n#define GPU_FETCH_U16_TO_FLOAT\n");
  }
  else {
    /* Crash if dimensions are not supported. */
  }

  GPU_shader_bind(shader);

  /* subdiv_polygon_offset is always at binding point 0 for each shader using it. */
  GPU_vertbuf_bind_as_ssbo(cache->subdiv_polygon_offset_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(src_data, 1);
  GPU_vertbuf_bind_as_ssbo(cache->face_ptex_offset_buffer, 2);
  GPU_vertbuf_bind_as_ssbo(cache->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(cache->extra_coarse_face_data, 4);
  GPU_vertbuf_bind_as_ssbo(dst_data, 5);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, dst_offset);

  GPU_compute_dispatch(shader, cache->num_subdiv_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_accumulate_normals(const DRWSubdivCache *cache,
                                    GPUVertBuf *pos_nor,
                                    GPUVertBuf *face_adjacency_offsets,
                                    GPUVertBuf *face_adjacency_lists,
                                    GPUVertBuf *vertex_normals)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_NORMALS_ACCUMULATE, nullptr);
  GPU_shader_bind(shader);

  int binding_point = 0;

  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);
  GPU_vertbuf_bind_as_ssbo(face_adjacency_offsets, binding_point++);
  GPU_vertbuf_bind_as_ssbo(face_adjacency_lists, binding_point++);
  GPU_vertbuf_bind_as_ssbo(vertex_normals, binding_point++);

  GPU_compute_dispatch(shader, cache->num_subdiv_vertis, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_finalize_normals(const DRWSubdivCache *cache,
                                  GPUVertBuf *vertex_normals,
                                  GPUVertBuf *subdiv_loop_subdiv_vert_index,
                                  GPUVertBuf *pos_nor)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_NORMALS_FINALIZE, nullptr);
  GPU_shader_bind(shader);

  int binding_point = 0;
  GPU_vertbuf_bind_as_ssbo(vertex_normals, binding_point++);
  GPU_vertbuf_bind_as_ssbo(subdiv_loop_subdiv_vert_index, binding_point++);
  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);

  GPU_compute_dispatch(shader, cache->num_subdiv_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_tris_buffer(const DRWSubdivCache *cache,
                                   GPUIndexBuf *subdiv_tris,
                                   const int material_count)
{
  const bool do_single_material = material_count <= 1;

  const char *defines = "#define SUBDIV_POLYGON_OFFSET\n";
  if (do_single_material) {
    defines = "#define SUBDIV_POLYGON_OFFSET\n#define SINGLE_MATERIAL\n";
  }

  GPUShader *shader = get_subdiv_shader(
      do_single_material ? SHADER_BUFFER_TRIS : SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS, defines);
  GPU_shader_bind(shader);

  /* Outputs */
  GPU_indexbuf_bind_as_ssbo(subdiv_tris, 1);

  if (!do_single_material) {
    GPU_vertbuf_bind_as_ssbo(cache->polygon_mat_offset, 2);
    /* subdiv_polygon_offset is always at binding point 0 for each shader using it. */
    GPU_vertbuf_bind_as_ssbo(cache->subdiv_polygon_offset_buffer, 0);

    /* Only needed for accessing the coarse_poly_count. */
    draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);
  }

  GPU_compute_dispatch(shader, cache->num_subdiv_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_fdots_buffers(const DRWSubdivCache *cache,
                                     GPUVertBuf *fdots_pos,
                                     GPUVertBuf *fdots_nor,
                                     GPUIndexBuf *fdots_indices)
{
  Subdiv *subdiv = cache->subdiv;
  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;

  GPUVertBuf *src_buffer = evaluator->getWrappedSrcBuffer(evaluator);
  GPUVertBuf *patch_arrays_buffer = evaluator->getPatchArraysBuffer(evaluator);
  GPUVertBuf *patch_index_buffer = evaluator->getWrappedPatchIndexBuffer(evaluator);
  GPUVertBuf *patch_param_buffer = evaluator->getWrappedPatchParamBuffer(evaluator);

  GPUShader *shader = get_patch_evaluation_shader(SHADER_PATCH_EVALUATION_FACE_DOTS);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(cache->fdots_patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(cache->verts_orig_index, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(fdots_pos, 8);
  GPU_vertbuf_bind_as_ssbo(fdots_nor, 9);
  GPU_indexbuf_bind_as_ssbo(fdots_indices, 10);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);

  GPU_compute_dispatch(shader, get_patch_evaluation_work_group_size(cache->num_coarse_poly), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

void draw_subdiv_build_lines_buffer(const DRWSubdivCache *cache, GPUIndexBuf *lines_indices)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LINES, nullptr);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(cache->edges_orig_index, 0);
  GPU_indexbuf_bind_as_ssbo(lines_indices, 1);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);

  GPU_compute_dispatch(shader, cache->num_subdiv_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_lines_loose_buffer(const DRWSubdivCache *cache,
                                          GPUIndexBuf *lines_indices,
                                          uint num_loose_edges)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LINES_LOOSE, "#define LINES_LOOSE\n");
  GPU_shader_bind(shader);

  GPU_indexbuf_bind_as_ssbo(lines_indices, 1);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);

  GPU_compute_dispatch(shader, num_loose_edges, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_edge_fac_buffer(const DRWSubdivCache *cache,
                                       GPUVertBuf *pos_nor,
                                       GPUVertBuf *edge_idx,
                                       GPUVertBuf *edge_fac)
{
  /* No separate shader for the AMD driver case as we assume that the GPU will not change during
   * the execution of the program. */
  const char *defines = GPU_crappy_amd_driver() ? "#define GPU_AMD_DRIVER_BYTE_BUG\n" : nullptr;
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_EDGE_FAC, defines);
  GPU_shader_bind(shader);

  GPU_vertbuf_bind_as_ssbo(pos_nor, 0);
  GPU_vertbuf_bind_as_ssbo(edge_idx, 1);
  GPU_vertbuf_bind_as_ssbo(edge_fac, 2);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);

  GPU_compute_dispatch(shader, cache->num_subdiv_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_lnor_buffer(const DRWSubdivCache *cache,
                                   GPUVertBuf *pos_nor,
                                   GPUVertBuf *lnor)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LNOR, "#define SUBDIV_POLYGON_OFFSET\n");
  GPU_shader_bind(shader);

  /* Inputs */
  GPU_vertbuf_bind_as_ssbo(pos_nor, 1);
  GPU_vertbuf_bind_as_ssbo(cache->extra_coarse_face_data, 2);
  /* subdiv_polygon_offset is always at binding point 0 for each shader using it. */
  GPU_vertbuf_bind_as_ssbo(cache->subdiv_polygon_offset_buffer, 0);

  draw_subdiv_ubo_update_and_bind(cache, shader, 0, 0);

  /* Outputs */
  GPU_vertbuf_bind_as_ssbo(lnor, 3);

  GPU_compute_dispatch(shader, cache->num_subdiv_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

/* -------------------------------------------------------------------- */

void draw_subdiv_init_mesh_render_data(Mesh *mesh,
                                       MeshRenderData *mr,
                                       const ToolSettings *toolsettings)
{
  /* Setup required data for loose geometry. */
  mr->me = mesh;
  mr->medge = mesh->medge;
  mr->mvert = mesh->mvert;
  mr->mpoly = mesh->mpoly;
  mr->mloop = mesh->mloop;
  mr->vert_len = mesh->totvert;
  mr->edge_len = mesh->totedge;
  mr->poly_len = mesh->totpoly;
  mr->loop_len = mesh->totloop;
  mr->extract_type = MR_EXTRACT_MESH;

  /* MeshRenderData is only used for generating edit mode data here. */
  if (!mesh->edit_mesh) {
    return;
  }

  BMesh *bm = mesh->edit_mesh->bm;
  BM_mesh_elem_table_ensure(bm, BM_EDGE | BM_FACE | BM_VERT);

  mr->bm = bm;
  mr->toolsettings = toolsettings;
  mr->eed_act = BM_mesh_active_edge_get(bm);
  mr->efa_act = BM_mesh_active_face_get(bm, false, true);
  mr->eve_act = BM_mesh_active_vert_get(bm);
  mr->vert_crease_ofs = CustomData_get_offset(&bm->vdata, CD_CREASE);
  mr->edge_crease_ofs = CustomData_get_offset(&bm->edata, CD_CREASE);
  mr->bweight_ofs = CustomData_get_offset(&bm->edata, CD_BWEIGHT);
#ifdef WITH_FREESTYLE
  mr->freestyle_edge_ofs = CustomData_get_offset(&bm->edata, CD_FREESTYLE_EDGE);
  mr->freestyle_face_ofs = CustomData_get_offset(&bm->pdata, CD_FREESTYLE_FACE);
#endif
}

/* For material assignements we want indices for triangles that share a common material to be laid
 * out contiguously in memory. To achieve this, we sort the indices based on which material the
 * coarse polygon was assigned. The sort is performed by offsetting the loops indices so that they
 * are directly assigned to the right sorted indices.
 *
 * Here is a visual representation, considering four quads:
 * +---------+---------+---------+---------+
 * | 3     2 | 7     6 | 11   10 | 15   14 |
 * |         |         |         |         |
 * | 0     1 | 4     5 | 8     9 | 12   13 |
 * +---------+---------+---------+---------+
 *
 * If the first and third quads have the same material, we should have:
 * +---------+---------+---------+---------+
 * | 3     2 | 11   10 | 7     6 | 15   14 |
 * |         |         |         |         |
 * | 0     1 | 8     9 | 4     5 | 12   13 |
 * +---------+---------+---------+---------+
 *
 * So the offsets would be:
 * +---------+---------+---------+---------+
 * | 0     0 | 4     4 | -4   -4 | 0     0 |
 * |         |         |         |         |
 * | 0     0 | 4     4 | -4   -4 | 0     0 |
 * +---------+---------+---------+---------+
 *
 * The offsets are computed not based on the loops indices, but on the number of subdivided
 * polygons for each coarse polygon. We then only store a single offset for each coarse polygon,
 * since all subfaces are contiguous, they all share the same offset.
 */
static void draw_subdiv_cache_ensure_mat_offsets(DRWSubdivCache *cache,
                                                 Mesh *mesh_eval,
                                                 uint mat_len)
{
  draw_subdiv_cache_free_material_data(cache);

  const int number_of_quads = cache->num_subdiv_loops / 4;

  if (mat_len == 1) {
    cache->mat_start = static_cast<int *>(MEM_callocN(sizeof(int), "subdiv mat_end"));
    cache->mat_end = static_cast<int *>(MEM_callocN(sizeof(int), "subdiv mat_end"));
    cache->mat_start[0] = 0;
    cache->mat_end[0] = number_of_quads;
    return;
  }

  /* Count number of subdivided polygons for each material. */
  int *mat_start = static_cast<int *>(MEM_callocN(sizeof(int) * mat_len, "subdiv mat_start"));
  int *subdiv_polygon_offset = cache->subdiv_polygon_offset;

  // TODO: parallel_reduce?
  for (int i = 0; i < mesh_eval->totpoly; i++) {
    const MPoly *mpoly = &mesh_eval->mpoly[i];
    const int next_offset = (i == mesh_eval->totpoly - 1) ? number_of_quads :
                                                            subdiv_polygon_offset[i + 1];
    const int quad_count = next_offset - subdiv_polygon_offset[i];
    const int mat_index = mpoly->mat_nr;
    mat_start[mat_index] += quad_count;
  }

  /* Accumulate offsets. */
  int ofs = mat_start[0];
  mat_start[0] = 0;
  for (uint i = 1; i < mat_len; i++) {
    int tmp = mat_start[i];
    mat_start[i] = ofs;
    ofs += tmp;
  }

  /* Compute per polygon offsets. */
  int *mat_end = static_cast<int *>(MEM_dupallocN(mat_start));
  int *per_polygon_mat_offset = static_cast<int *>(
      MEM_mallocN(sizeof(int) * mesh_eval->totpoly, "per_polygon_mat_offset"));

  for (int i = 0; i < mesh_eval->totpoly; i++) {
    const MPoly *mpoly = &mesh_eval->mpoly[i];
    const int mat_index = mpoly->mat_nr;
    const int single_material_index = subdiv_polygon_offset[i];
    const int material_offset = mat_end[mat_index];
    const int next_offset = (i == mesh_eval->totpoly - 1) ? number_of_quads :
                                                            subdiv_polygon_offset[i + 1];
    const int quad_count = next_offset - subdiv_polygon_offset[i];
    mat_end[mat_index] += quad_count;

    per_polygon_mat_offset[i] = material_offset - single_material_index;
  }

  cache->polygon_mat_offset = draw_subdiv_build_origindex_buffer(per_polygon_mat_offset,
                                                                 mesh_eval->totpoly);
  cache->mat_start = mat_start;
  cache->mat_end = mat_end;

  MEM_freeN(per_polygon_mat_offset);
}

static bool draw_subdiv_create_requested_buffers(const Scene *scene,
                                                 Object *ob,
                                                 Mesh *mesh,
                                                 struct MeshBatchCache *batch_cache,
                                                 MeshBufferCache *mbc,
                                                 const ToolSettings *toolsettings,
                                                 OpenSubdiv_EvaluatorCache *evaluator_cache)
{
  SubsurfModifierData *smd = BKE_object_get_last_modifier_if_subsurf(ob);
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
          subdiv, mesh_eval, nullptr, OPENSUBDIV_EVALUATOR_GLSL_COMPUTE, evaluator_cache)) {
    return false;
  }

  DRWSubdivCache *draw_cache = ensure_draw_cache(subdiv);
  if (!draw_subdiv_build_cache(draw_cache, subdiv, mesh_eval, scene, smd, is_final_render)) {
    return false;
  }

  const bool optimal_display = (smd->flags & eSubsurfModifierFlag_ControlEdges);

  draw_cache->mesh = mesh_eval;
  draw_cache->subdiv = subdiv;
  draw_cache->optimal_display = optimal_display;
  draw_cache->num_subdiv_triangles = tris_count_from_number_of_loops(draw_cache->num_subdiv_loops);
  /* We can only evaluate limit normals if the patches are adaptive. */
  draw_cache->do_limit_normals = settings.is_adaptive;

  if (DRW_ibo_requested(mbc->buff.ibo.tris)) {
    draw_subdiv_cache_ensure_mat_offsets(draw_cache, mesh_eval, batch_cache->mat_len);
  }

  draw_subdiv_cache_update_extra_coarse_face_data(draw_cache, mesh_eval);

  mesh_buffer_cache_create_requested_subdiv(batch_cache, mbc, draw_cache, toolsettings);

  return true;
}

static OpenSubdiv_EvaluatorCache *g_evaluator_cache = nullptr;

void DRW_create_subdivision(const Scene *scene,
                            Object *ob,
                            Mesh *mesh,
                            struct MeshBatchCache *batch_cache,
                            MeshBufferCache *mbc,
                            const ToolSettings *toolsettings)
{
  if (g_evaluator_cache == nullptr) {
    g_evaluator_cache = openSubdiv_createEvaluatorCache(OPENSUBDIV_EVALUATOR_GLSL_COMPUTE);
  }

#undef TIME_SUBDIV

#ifdef TIME_SUBDIV
  const double begin_time = PIL_check_seconds_timer();
#endif

  if (!draw_subdiv_create_requested_buffers(
          scene, ob, mesh, batch_cache, mbc, toolsettings, g_evaluator_cache)) {
    fprintf(stderr,
            "Cannot evaluate subdivision on the GPU, falling back to the regular draw code.\n");
    return;
  }

#ifdef TIME_SUBDIV
  const double end_time = PIL_check_seconds_timer();
  fprintf(stderr, "Time to update subdivision: %f\n", end_time - begin_time);
  fprintf(stderr, "Maximum FPS: %f\n", 1.0 / (end_time - begin_time));
#endif
}

void DRW_subdiv_free(void)
{
  for (int i = 0; i < NUM_SHADERS; ++i) {
    GPU_shader_free(g_subdiv_shaders[i]);
  }

  DRW_cache_free_old_subdiv();

  if (g_evaluator_cache) {
    openSubdiv_deleteEvaluatorCache(g_evaluator_cache);
    g_evaluator_cache = nullptr;
  }
}

static LinkNode *gpu_subdiv_free_queue = nullptr;
static ThreadMutex gpu_subdiv_queue_mutex = BLI_MUTEX_INITIALIZER;

void DRW_subdiv_cache_free(Subdiv *subdiv)
{
  BLI_mutex_lock(&gpu_subdiv_queue_mutex);
  BLI_linklist_prepend(&gpu_subdiv_free_queue, subdiv);
  BLI_mutex_unlock(&gpu_subdiv_queue_mutex);
}

void DRW_cache_free_old_subdiv()
{
  if (gpu_subdiv_free_queue == nullptr) {
    return;
  }

  BLI_mutex_lock(&gpu_subdiv_queue_mutex);

  while (gpu_subdiv_free_queue != nullptr) {
    Subdiv *subdiv = static_cast<Subdiv *>(BLI_linklist_pop(&gpu_subdiv_free_queue));
    BKE_subdiv_free(subdiv);
  }

  BLI_mutex_unlock(&gpu_subdiv_queue_mutex);
}
