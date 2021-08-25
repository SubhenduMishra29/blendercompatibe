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

#if 0
static void print_index_buffer(const char *message, int *index, int len)
{
  fprintf(stderr, "%s\n", message);
  for (int i = 0; i < len; i++) {
    fprintf(stderr, "index at %d: %d\n", i, index[i]);
  }
}
#endif

/* To-dos:
 * - improve OpenSubdiv_BufferInterface
 * - comments
 * - better structure for this file
 * - add support for vertex colors
 * - holes: deduplicate operator code in edit_mesh_tools.
 */

extern char datatoc_common_subdiv_lib_glsl[];
extern char datatoc_common_subdiv_buffer_lines_comp_glsl[];
extern char datatoc_common_subdiv_buffer_lnor_comp_glsl[];
extern char datatoc_common_subdiv_buffer_edge_fac_comp_glsl[];
extern char datatoc_common_subdiv_buffer_points_comp_glsl[];
extern char datatoc_common_subdiv_tris_comp_glsl[];
extern char datatoc_common_subdiv_normals_accumulate_comp_glsl[];
extern char datatoc_common_subdiv_normals_finalize_comp_glsl[];
extern char datatoc_common_subdiv_patch_evaluation_comp_glsl[];
extern char datatoc_common_subdiv_custom_data_interp_comp_glsl[];

enum {
  SHADER_BUFFER_LINES,
  SHADER_BUFFER_EDGE_FAC,
  SHADER_BUFFER_EDGE_FAC_HQ,  // for high quality normals
  SHADER_BUFFER_LNOR,
  SHADER_BUFFER_LNOR_HQ,  // for high quality normals
  SHADER_BUFFER_TRIS,
  SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS,
  SHADER_BUFFER_NORMALS_ACCUMULATE,
  SHADER_BUFFER_NORMALS_FINALIZE,
  SHADER_BUFFER_NORMALS_FINALIZE_HQ,  // for high quality normals
  SHADER_PATCH_EVALUATION,
  SHADER_PATCH_EVALUATION_LIMIT_NORMALS,
  SHADER_PATCH_EVALUATION_HQ,                // for high quality normals
  SHADER_PATCH_EVALUATION_LIMIT_NORMALS_HQ,  // for high quality normals
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
    case SHADER_BUFFER_LINES: {
      return datatoc_common_subdiv_buffer_lines_comp_glsl;
    }
    case SHADER_BUFFER_EDGE_FAC:
    case SHADER_BUFFER_EDGE_FAC_HQ: {
      return datatoc_common_subdiv_buffer_edge_fac_comp_glsl;
    }
    case SHADER_BUFFER_LNOR:
    case SHADER_BUFFER_LNOR_HQ: {
      return datatoc_common_subdiv_buffer_lnor_comp_glsl;
    }
    case SHADER_BUFFER_TRIS:
    case SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS: {
      return datatoc_common_subdiv_tris_comp_glsl;
    }
    case SHADER_BUFFER_NORMALS_ACCUMULATE: {
      return datatoc_common_subdiv_normals_accumulate_comp_glsl;
    }
    case SHADER_BUFFER_NORMALS_FINALIZE:
    case SHADER_BUFFER_NORMALS_FINALIZE_HQ: {
      return datatoc_common_subdiv_normals_finalize_comp_glsl;
    }
    case SHADER_PATCH_EVALUATION:
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS:
    case SHADER_PATCH_EVALUATION_HQ:
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS_HQ:
    case SHADER_PATCH_EVALUATION_FVAR:
    case SHADER_PATCH_EVALUATION_FACE_DOTS: {
      return datatoc_common_subdiv_patch_evaluation_comp_glsl;
    }
    case SHADER_COMP_CUSTOM_DATA_INTERP_1D:
    case SHADER_COMP_CUSTOM_DATA_INTERP_4D: {
      return datatoc_common_subdiv_custom_data_interp_comp_glsl;
    }
  }
  return NULL;
}

static const char *get_shader_name(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_LINES: {
      return "subdiv lines build";
    }
    case SHADER_BUFFER_LNOR: {
      return "subdiv lnor build";
    }
    case SHADER_BUFFER_LNOR_HQ: {
      return "subdiv lnor build hq";
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return "subdiv edge fac build";
    }
    case SHADER_BUFFER_EDGE_FAC_HQ: {
      return "subdiv edge fac build hq";
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
    case SHADER_BUFFER_NORMALS_FINALIZE_HQ: {
      return "subdiv normals finalize";
    }
    case SHADER_PATCH_EVALUATION: {
      return "subdiv patch evaluation";
    }
    case SHADER_PATCH_EVALUATION_HQ: {
      return "subdiv patch evaluation hq";
    }
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS: {
      return "subdiv patch evaluation limit normals";
    }
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS_HQ: {
      return "subdiv patch evaluation limit normals hq";
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
  return NULL;
}

static GPUShader *get_patch_evaluation_shader(int shader_type)
{
  if (g_subdiv_shaders[shader_type] == NULL) {
    const char *compute_code = get_shader_code(shader_type);

    const char *defines = NULL;
    if (shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define LIMIT_NORMALS\n";
    }
    else if (shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS_HQ) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define LIMIT_NORMALS\n"
          "#define HQ_NORMALS\n";
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
    else if (shader_type == SHADER_PATCH_EVALUATION_HQ) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define HQ_NORMALS\n";
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
      shader_type == SHADER_PATCH_EVALUATION_HQ ||
      shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS_HQ ||
      shader_type == SHADER_PATCH_EVALUATION_FVAR ||
      shader_type == SHADER_PATCH_EVALUATION_FACE_DOTS) {
    return get_patch_evaluation_shader(shader_type);
  }
  if (g_subdiv_shaders[shader_type] == NULL) {
    const char *compute_code = get_shader_code(shader_type);
    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, datatoc_common_subdiv_lib_glsl, defines, get_shader_name(shader_type));
  }
  return g_subdiv_shaders[shader_type];
}

/* Vertex format for OpenSubdiv::Osd::PatchArray. */
static GPUVertFormat *get_patch_array_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "regDesc", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "desc", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "numPatches", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "indexBase", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "stride", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "primitiveIdBase", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }
  return &format;
}

static GPUVertFormat *get_uvs_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "uvs", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format for OpenSubdiv::Osd::PatchParam, not really used, it is only for making sure that
 * the GPUVertBuf used to wrap the OpenSubdiv patch param buffer is valid. */
static GPUVertFormat *get_patch_param_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format for the patches' vertices index buffer. */
static GPUVertFormat *get_patch_index_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_I32, 1, GPU_FETCH_INT);
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

typedef struct CompressedPatchCoord {
  int ptex_face_index;
  /* UV coordinate encoded as u << 16 | v, where u and v are quantized on 16-bits. */
  unsigned int encoded_uv;
} CompressedPatchCoord;

MINLINE CompressedPatchCoord make_patch_coord(int ptex_face_index, float u, float v)
{
  CompressedPatchCoord patch_coord = {
      .ptex_face_index = ptex_face_index,
      .encoded_uv = ((unsigned int)(u * 65535.0f) << 16) | (unsigned int)(v * 65535.0f),
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

/* Vertex format used for the PatchTable::PatchHandle. */
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

/* Vertex format used for the quadtree nodes of the PatchMap. */
static GPUVertFormat *get_quadtree_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "child", GPU_COMP_U32, 4, GPU_FETCH_INT);
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

static void vertbuf_device_alloc(OpenSubdiv_BufferInterface *interface, const uint len)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  /* This assumes that GPU_USAGE_DEVICE_ONLY was used, which won't allocate host memory. */
  BLI_assert(GPU_vertbuf_get_usage(verts) == GPU_USAGE_DEVICE_ONLY);
  GPU_vertbuf_data_alloc(verts, len);
}

static int vertbuf_num_vertices(OpenSubdiv_BufferInterface *interface)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  return GPU_vertbuf_get_vertex_len(verts);
}

static void vertbuf_wrap(OpenSubdiv_BufferInterface *interface, uint device_ptr)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  GPU_vertbuf_wrap_device_ptr(verts, device_ptr);
}

static void vertbuf_update_data(OpenSubdiv_BufferInterface *interface,
                                uint start,
                                uint len,
                                const void *data)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  GPU_vertbuf_update_sub(verts, start, len, data);
}

static void opensubdiv_gpu_buffer_init(OpenSubdiv_BufferInterface *buffer_interface,
                                       GPUVertBuf *vertbuf)
{
  buffer_interface->data = vertbuf;
  buffer_interface->bind = vertbuf_bind_gpu;
  buffer_interface->alloc = vertbuf_alloc;
  buffer_interface->num_vertices = vertbuf_num_vertices;
  buffer_interface->buffer_offset = 0;
  buffer_interface->wrap = vertbuf_wrap;
  buffer_interface->device_alloc = vertbuf_device_alloc;
  buffer_interface->update_data = vertbuf_update_data;
}

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
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

static void gpu_patch_map_build(GPUPatchMap *gpu_patch_map, Subdiv *subdiv)
{
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

  gpu_patch_map->patch_map_handles = patch_map_handles;
  gpu_patch_map->patch_map_quadtree = patch_map_quadtree;
  gpu_patch_map->min_patch_face = min_patch_face;
  gpu_patch_map->max_patch_face = max_patch_face;
  gpu_patch_map->max_depth = max_depth;
  gpu_patch_map->patches_are_triangular = patches_are_triangular;
}

static void gpu_patch_map_free(GPUPatchMap *gpu_patch_map)
{
  GPU_VERTBUF_DISCARD_SAFE(gpu_patch_map->patch_map_handles);
  GPU_VERTBUF_DISCARD_SAFE(gpu_patch_map->patch_map_quadtree);
  gpu_patch_map->min_patch_face = 0;
  gpu_patch_map->max_patch_face = 0;
  gpu_patch_map->max_depth = 0;
  gpu_patch_map->patches_are_triangular = 0;
}

#include "BLI_bitmap.h"
#include "BLI_memarena.h"

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
 * \{ */

static void draw_subdiv_cache_free_material_data(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->polygon_mat_offset);
  MEM_SAFE_FREE(cache->mat_start);
  MEM_SAFE_FREE(cache->mat_end);
}

static void draw_free_edit_mode_cache(DRWSubdivCache *cache)
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
  cache->num_patch_coords = 0;
  cache->coarse_poly_count = 0;
  cache->number_of_quads = 0;
  draw_free_edit_mode_cache(cache);
  draw_subdiv_cache_free_material_data(cache);
  gpu_patch_map_free(&cache->gpu_patch_map);

  cache->edge_loose_len = 0;
  cache->vert_loose_len = 0;
  cache->loop_loose_len = 0;
}

static void draw_subdiv_cache_update_extra_coarse_face_data(DRWSubdivCache *cache, Mesh *mesh)
{
  if (cache->extra_coarse_face_data == NULL) {
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

static void draw_subdiv_cache_print_memory_used(DRWSubdivCache *cache)
{
  size_t memory_used = 0;

  if (cache->patch_coords) {
    memory_used += cache->num_patch_coords * 8;
  }

  if (cache->subdiv_polygon_offset_buffer) {
    memory_used += cache->coarse_poly_count * sizeof(int);
  }

  if (cache->extra_coarse_face_data) {
    memory_used += cache->coarse_poly_count * sizeof(int);
  }

  if (cache->edges_orig_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->subdiv_loop_subdiv_vert_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->subdiv_loop_poly_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->verts_orig_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->point_indices) {
    memory_used += cache->num_vertices * sizeof(int);
  }

  if (cache->subdiv_vertex_face_adjacency_offsets) {
    memory_used += cache->num_vertices * sizeof(int);
  }

  if (cache->subdiv_vertex_face_adjacency) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

#if 0
  GPU_VERTBUF_DISCARD_SAFE(cache->polygon_mat_offset);
  MEM_SAFE_FREE(cache->mat_start);
  MEM_SAFE_FREE(cache->mat_end);
  GPU_VERTBUF_DISCARD_SAFE(cache->verts_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->edges_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->faces_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->fdots_patch_coords);
#endif

  fprintf(stderr, "Memory used by the GPU subdivision cache: %lu bytes\n", memory_used);

  if (cache->patch_coords) {
    fprintf(stderr, "Memory used for the patch coords: %u bytes\n", cache->num_patch_coords * 8);
  }
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
    // fprintf(stderr, "Creating a new cache !\n");
    draw_cache = static_cast<DRWSubdivCache *>(
        MEM_callocN(sizeof(DRWSubdivCache), "DRWSubdivCache"));
  }
  subdiv->draw_cache = draw_cache;
  subdiv->free_draw_cache = free_draw_cache_from_subdiv_cb;
  return draw_cache;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWCacheBuildingContext
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

static bool patch_coords_topology_info(const SubdivForeachContext *foreach_context,
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
  cache->num_edges = (uint)num_edges;
  cache->num_patch_coords = (uint)num_loops;
  cache->num_vertices = (uint)num_vertices;
  cache->number_of_quads = (uint)num_polygons;
  cache->subdiv_polygon_offset = static_cast<int *>(MEM_dupallocN(subdiv_polygon_offset));

  /* Initialize cache buffers, prefer dynamic usage so we can reuse memory on the host even after
   * it was sent to the device, since we may use the data while building other buffers on the CPU
   * side. */
  cache->patch_coords = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      cache->patch_coords, get_blender_patch_coords_format(), GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(cache->patch_coords, cache->num_patch_coords);

  cache->verts_orig_index = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      cache->verts_orig_index, get_origindex_format(), GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(cache->verts_orig_index, cache->num_patch_coords);

  cache->edges_orig_index = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      cache->edges_orig_index, get_origindex_format(), GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(cache->edges_orig_index, cache->num_patch_coords);

  cache->subdiv_loop_subdiv_vert_index = static_cast<int *>(
      MEM_mallocN(cache->num_patch_coords * sizeof(int), "subdiv_loop_subdiv_vert_index"));

  cache->subdiv_loop_poly_index = static_cast<int *>(
      MEM_mallocN(cache->num_patch_coords * sizeof(int), "subdiv_loop_poly_index"));

  cache->point_indices = static_cast<int *>(
      MEM_mallocN(cache->num_vertices * sizeof(int), "point_indices"));
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
      MEM_mallocN(cache->num_vertices * sizeof(int), "subdiv_vert_origindex_map"));
  for (int i = 0; i < num_vertices; i++) {
    ctx->vert_origindex_map[i] = -1;
  }

  ctx->edge_origindex_map = static_cast<int *>(
      MEM_mallocN(cache->num_edges * sizeof(int), "subdiv_edge_origindex_map"));
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
  foreach_context->topology_info = patch_coords_topology_info;
  foreach_context->loop = draw_subdiv_loop_cb;
  foreach_context->edge = draw_subdiv_edge_cb;
  foreach_context->vertex_corner = draw_subdiv_vertex_corner_cb;
  foreach_context->vertex_edge = draw_subdiv_vertex_edge_cb;
}

static void build_cached_data_from_subdiv(DRWCacheBuildingContext *cache_building_context,
                                          Subdiv *subdiv)
{
  SubdivForeachContext foreach_context;
  draw_subdiv_foreach_callbacks(&foreach_context);
  foreach_context.user_data = cache_building_context;

  BKE_subdiv_foreach_subdiv_geometry(subdiv,
                                     &foreach_context,
                                     cache_building_context->settings,
                                     cache_building_context->coarse_mesh);

  /* Now that traversal is done, we can set up the right original indices for the loop to edge map.
   */
  for (int i = 0; i < cache_building_context->cache->num_patch_coords; i++) {
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

static void build_vertex_face_adjacency_maps(DRWSubdivCache *cache)
{
  /* +1 so that we do not require a special for the last vertex, this extra offset will contain the
   * total number of adjacent faces. */
  cache->subdiv_vertex_face_adjacency_offsets = gpu_vertbuf_create_from_format(
      get_origindex_format(), cache->num_vertices + 1);

  int *vertex_offsets = (int *)GPU_vertbuf_get_data(cache->subdiv_vertex_face_adjacency_offsets);
  memset(vertex_offsets, 0, sizeof(int) * cache->num_vertices + 1);

  for (int i = 0; i < cache->num_patch_coords; i++) {
    vertex_offsets[cache->subdiv_loop_subdiv_vert_index[i]]++;
  }

  int ofs = vertex_offsets[0];
  vertex_offsets[0] = 0;
  for (uint i = 1; i < cache->num_vertices + 1; i++) {
    int tmp = vertex_offsets[i];
    vertex_offsets[i] = ofs;
    ofs += tmp;
  }

  cache->subdiv_vertex_face_adjacency = gpu_vertbuf_create_from_format(get_origindex_format(),
                                                                       cache->num_patch_coords);
  int *adjacent_faces = (int *)GPU_vertbuf_get_data(cache->subdiv_vertex_face_adjacency);
  int *tmp_set_faces = static_cast<int *>(
      MEM_callocN(sizeof(int) * cache->num_vertices, "tmp subdiv vertex offset"));

  for (int i = 0; i < cache->num_patch_coords / 4; i++) {
    for (int j = 0; j < 4; j++) {
      const int subdiv_vertex = cache->subdiv_loop_subdiv_vert_index[i * 4 + j];
      int first_face_offset = vertex_offsets[subdiv_vertex] + tmp_set_faces[subdiv_vertex];
      adjacent_faces[first_face_offset] = i;
      tmp_set_faces[subdiv_vertex] += 1;
    }
  }

  MEM_freeN(tmp_set_faces);
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
    // fprintf(stderr, "Resolution changed rebuilding cache !\n");
    /* Resolution chaged, we need to rebuild. */
    draw_subdiv_cache_free(cache);
  }

  if (cache->patch_coords != NULL) {
    // fprintf(stderr, "Cache does not need to be rebuilt !\n");
    /* No need to rebuild anything. */
    return true;
  }

  DRWCacheBuildingContext cache_building_context;
  cache_building_context.coarse_mesh = mesh_eval;
  cache_building_context.settings = &to_mesh_settings;
  cache_building_context.cache = cache;

  build_cached_data_from_subdiv(&cache_building_context, subdiv);
  if (cache->num_patch_coords == 0) {
    MEM_SAFE_FREE(cache->subdiv_polygon_offset);
    return false;
  }

  /* Build buffers for the PatchMap. */
  gpu_patch_map_build(&cache->gpu_patch_map, subdiv);

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
       * is anyone of those top right corner with `u = v = 1.0`. */
      blender_fdots_patch_coords[i] = make_patch_coord(ptex_face_index, 1.0f, 1.0f);
    }
  }

  cache->resolution = to_mesh_settings.resolution;

  cache->subdiv_polygon_offset_buffer = draw_subdiv_build_origindex_buffer(
      cache->subdiv_polygon_offset, mesh_eval->totpoly);

  cache->face_ptex_offset_buffer = draw_subdiv_build_origindex_buffer(cache->face_ptex_offset,
                                                                      mesh_eval->totpoly + 1);
  cache->coarse_poly_count = mesh_eval->totpoly;
  cache->point_indices = cache_building_context.point_indices;

  build_vertex_face_adjacency_maps(cache);

  /* Cleanup. */
  MEM_freeN(cache_building_context.vert_origindex_map);
  MEM_freeN(cache_building_context.edge_origindex_map);

  return true;
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
                                 const bool do_limit_normals,
                                 const bool do_hq_normals)
{
  Subdiv *subdiv = cache->subdiv;
  GPUVertBuf *src_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(src_buffer, get_work_vertex_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface src_buffer_interface;
  opensubdiv_gpu_buffer_init(&src_buffer_interface, src_buffer);
  subdiv->evaluator->buildSrcBuffer(subdiv->evaluator, &src_buffer_interface);

  GPUVertBuf *patch_arrays_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_arrays_buffer, get_patch_array_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_arrays_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_arrays_buffer_interface, patch_arrays_buffer);
  subdiv->evaluator->buildPatchArraysBuffer(subdiv->evaluator, &patch_arrays_buffer_interface);

  GPUVertBuf *patch_index_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_index_buffer, get_patch_index_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_index_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_index_buffer_interface, patch_index_buffer);
  subdiv->evaluator->buildPatchIndexBuffer(subdiv->evaluator, &patch_index_buffer_interface);

  GPUVertBuf *patch_param_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_param_buffer, get_patch_param_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_param_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_param_buffer_interface, patch_param_buffer);
  subdiv->evaluator->buildPatchParamBuffer(subdiv->evaluator, &patch_param_buffer_interface);

  GPUShader *shader = get_patch_evaluation_shader(
      do_limit_normals ? (do_hq_normals ? SHADER_PATCH_EVALUATION_LIMIT_NORMALS_HQ :
                                          SHADER_PATCH_EVALUATION_LIMIT_NORMALS) :
                         (do_hq_normals ? SHADER_PATCH_EVALUATION_HQ : SHADER_PATCH_EVALUATION));
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "min_patch_face", cache->gpu_patch_map.min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", cache->gpu_patch_map.max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", cache->gpu_patch_map.max_depth);
  GPU_shader_uniform_1i(
      shader, "patches_are_triangular", cache->gpu_patch_map.patches_are_triangular);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(cache->gpu_patch_map.patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(cache->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(cache->verts_orig_index, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(pos_nor, 8);

  GPU_compute_dispatch(shader, get_patch_evaluation_work_group_size(cache->number_of_quads), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  /* Free the memory used for allocating a GPUVertBuf, but not the underlying buffers. */
  GPU_vertbuf_wrap_device_ptr(patch_index_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(patch_param_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(src_buffer, 0);

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

void draw_subdiv_extract_uvs(const DRWSubdivCache *buffers,
                             GPUVertBuf *uvs,
                             const int face_varying_channel,
                             const int dst_offset)
{
  Subdiv *subdiv = buffers->subdiv;

  GPUVertBuf *src_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(src_buffer, get_uvs_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface src_buffer_interface;
  opensubdiv_gpu_buffer_init(&src_buffer_interface, src_buffer);
  subdiv->evaluator->buildFVarSrcBuffer(
      subdiv->evaluator, face_varying_channel, &src_buffer_interface);

  GPUVertBuf *patch_arrays_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_arrays_buffer, get_patch_array_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_arrays_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_arrays_buffer_interface, patch_arrays_buffer);
  subdiv->evaluator->buildFVarPatchArraysBuffer(
      subdiv->evaluator, face_varying_channel, &patch_arrays_buffer_interface);

  GPUVertBuf *patch_index_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_index_buffer, get_patch_index_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_index_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_index_buffer_interface, patch_index_buffer);
  subdiv->evaluator->buildFVarPatchIndexBuffer(
      subdiv->evaluator, face_varying_channel, &patch_index_buffer_interface);

  GPUVertBuf *patch_param_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_param_buffer, get_patch_param_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_param_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_param_buffer_interface, patch_param_buffer);
  subdiv->evaluator->buildFVarPatchParamBuffer(
      subdiv->evaluator, face_varying_channel, &patch_param_buffer_interface);

  GPUShader *shader = get_patch_evaluation_shader(SHADER_PATCH_EVALUATION_FVAR);
  GPU_shader_bind(shader);

  /* The buffer offset has the stride baked in (which is 2 as we have UVs) so remove the stride by
   * dividing by 2 */
  GPU_shader_uniform_1i(shader, "src_offset", src_buffer_interface.buffer_offset / 2);
  GPU_shader_uniform_1i(shader, "dst_offset", dst_offset);
  GPU_shader_uniform_1i(shader, "min_patch_face", buffers->gpu_patch_map.min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", buffers->gpu_patch_map.max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", buffers->gpu_patch_map.max_depth);
  GPU_shader_uniform_1i(
      shader, "patches_are_triangular", buffers->gpu_patch_map.patches_are_triangular);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map.patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map.patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(buffers->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(buffers->verts_orig_index, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(uvs, 8);

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(buffers->number_of_quads), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  /* Free the memory used for allocating a GPUVertBuf, but not the underlying buffers. */
  GPU_vertbuf_wrap_device_ptr(patch_index_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(patch_param_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(src_buffer, 0);

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

void draw_subdiv_interp_custom_data(const DRWSubdivCache *buffers,
                                    GPUVertBuf *src_data,
                                    GPUVertBuf *dst_data,
                                    int dimensions,
                                    int dst_offset)
{
  GPUShader *shader = NULL;

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

  GPU_shader_uniform_1i(shader, "dst_offset", dst_offset);
  GPU_shader_uniform_1i(shader, "coarse_poly_count", buffers->coarse_poly_count);

  /* subdiv_polygon_offset is always at binding point 0 for each shader using it. */
  GPU_vertbuf_bind_as_ssbo(buffers->subdiv_polygon_offset_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(src_data, 1);
  GPU_vertbuf_bind_as_ssbo(buffers->face_ptex_offset_buffer, 2);
  GPU_vertbuf_bind_as_ssbo(buffers->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(buffers->extra_coarse_face_data, 4);
  GPU_vertbuf_bind_as_ssbo(dst_data, 5);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_accumulate_normals(const DRWSubdivCache *buffers,
                                    GPUVertBuf *pos_nor,
                                    GPUVertBuf *face_adjacency_offsets,
                                    GPUVertBuf *face_adjacency_lists,
                                    GPUVertBuf *vertex_normals)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_NORMALS_ACCUMULATE, NULL);
  GPU_shader_bind(shader);

  int binding_point = 0;

  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);
  GPU_vertbuf_bind_as_ssbo(face_adjacency_offsets, binding_point++);
  GPU_vertbuf_bind_as_ssbo(face_adjacency_lists, binding_point++);
  GPU_vertbuf_bind_as_ssbo(vertex_normals, binding_point++);

  GPU_compute_dispatch(shader, buffers->num_vertices, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_finalize_normals(const DRWSubdivCache *buffers,
                                  GPUVertBuf *vertex_normals,
                                  GPUVertBuf *subdiv_loop_subdiv_vert_index,
                                  GPUVertBuf *pos_nor,
                                  const bool do_hq_normals)
{
  GPUShader *shader = do_hq_normals ? get_subdiv_shader(SHADER_BUFFER_NORMALS_FINALIZE_HQ,
                                                        "#define HQ_NORMALS\n") :
                                      get_subdiv_shader(SHADER_BUFFER_NORMALS_FINALIZE, NULL);
  GPU_shader_bind(shader);

  int binding_point = 0;
  GPU_vertbuf_bind_as_ssbo(vertex_normals, binding_point++);
  GPU_vertbuf_bind_as_ssbo(subdiv_loop_subdiv_vert_index, binding_point++);
  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_tris_buffer(const DRWSubdivCache *buffers,
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
    GPU_shader_uniform_1i(shader, "coarse_poly_count", buffers->coarse_poly_count);
    GPU_vertbuf_bind_as_ssbo(buffers->polygon_mat_offset, 2);
    /* subdiv_polygon_offset is always at binding point 0 for each shader using it. */
    GPU_vertbuf_bind_as_ssbo(buffers->subdiv_polygon_offset_buffer, 0);
  }

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

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
  GPUVertBuf *src_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(src_buffer, get_work_vertex_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface src_buffer_interface;
  opensubdiv_gpu_buffer_init(&src_buffer_interface, src_buffer);
  subdiv->evaluator->buildSrcBuffer(subdiv->evaluator, &src_buffer_interface);

  GPUVertBuf *patch_arrays_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_arrays_buffer, get_patch_array_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_arrays_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_arrays_buffer_interface, patch_arrays_buffer);
  subdiv->evaluator->buildPatchArraysBuffer(subdiv->evaluator, &patch_arrays_buffer_interface);

  GPUVertBuf *patch_index_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_index_buffer, get_patch_index_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_index_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_index_buffer_interface, patch_index_buffer);
  subdiv->evaluator->buildPatchIndexBuffer(subdiv->evaluator, &patch_index_buffer_interface);

  GPUVertBuf *patch_param_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_param_buffer, get_patch_param_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_param_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_param_buffer_interface, patch_param_buffer);
  subdiv->evaluator->buildPatchParamBuffer(subdiv->evaluator, &patch_param_buffer_interface);

  GPUShader *shader = get_patch_evaluation_shader(SHADER_PATCH_EVALUATION_FACE_DOTS);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "min_patch_face", cache->gpu_patch_map.min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", cache->gpu_patch_map.max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", cache->gpu_patch_map.max_depth);
  GPU_shader_uniform_1i(
      shader, "patches_are_triangular", cache->gpu_patch_map.patches_are_triangular);

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

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(cache->coarse_poly_count), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  /* Free the memory used for allocating a GPUVertBuf, but not the underlying buffers. */
  GPU_vertbuf_wrap_device_ptr(patch_index_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(patch_param_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(src_buffer, 0);

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

void draw_subdiv_build_lines_buffer(const DRWSubdivCache *cache,
                                    GPUIndexBuf *lines_indices,
                                    const bool optimal_display)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LINES, NULL);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "optimal_display", optimal_display);

  GPU_vertbuf_bind_as_ssbo(cache->edges_orig_index, 0);
  GPU_indexbuf_bind_as_ssbo(lines_indices, 1);

  GPU_compute_dispatch(shader, cache->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_edge_fac_buffer(const DRWSubdivCache *cache,
                                       GPUVertBuf *pos_nor,
                                       GPUVertBuf *edge_idx,
                                       bool optimal_display,
                                       GPUVertBuf *edge_fac,
                                       const bool do_hq_normals)
{
  /* No separate shader for the AMD driver case as we assume that the GPU will not change during
   * the execution of the program. */
  const char *defines = GPU_crappy_amd_driver() ?
                            (do_hq_normals ?
                                 "#define HQ_NORMALS\n#define GPU_AMD_DRIVER_BYTE_BUG\n" :
                                 "#define GPU_AMD_DRIVER_BYTE_BUG\n") :
                            (do_hq_normals ? "#define HQ_NORMALS\n" : NULL);
  GPUShader *shader = get_subdiv_shader(
      do_hq_normals ? SHADER_BUFFER_EDGE_FAC_HQ : SHADER_BUFFER_EDGE_FAC, defines);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "optimal_display", optimal_display);

  GPU_vertbuf_bind_as_ssbo(pos_nor, 0);
  GPU_vertbuf_bind_as_ssbo(edge_idx, 1);
  GPU_vertbuf_bind_as_ssbo(edge_fac, 2);

  GPU_compute_dispatch(shader, cache->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_subdiv_build_lnor_buffer(const DRWSubdivCache *cache,
                                   GPUVertBuf *pos_nor,
                                   GPUVertBuf *lnor,
                                   const bool do_hq_normals)
{
  GPUShader *shader = do_hq_normals ?
                          get_subdiv_shader(
                              SHADER_BUFFER_LNOR_HQ,
                              "#define HQ_NORMALS\n#define SUBDIV_POLYGON_OFFSET\n") :
                          get_subdiv_shader(SHADER_BUFFER_LNOR, "#define SUBDIV_POLYGON_OFFSET\n");
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "coarse_poly_count", cache->coarse_poly_count);

  /* Inputs */
  GPU_vertbuf_bind_as_ssbo(pos_nor, 1);
  GPU_vertbuf_bind_as_ssbo(cache->extra_coarse_face_data, 2);
  /* subdiv_polygon_offset is always at binding point 0 for each shader using it. */
  GPU_vertbuf_bind_as_ssbo(cache->subdiv_polygon_offset_buffer, 0);

  /* Outputs */
  GPU_vertbuf_bind_as_ssbo(lnor, 3);

  GPU_compute_dispatch(shader, cache->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

/* -------------------------------------------------------------------- */

static void print_requests(MeshBufferList *mbc)
{
  fprintf(stderr, "============== REQUESTS ==============\n");

#define PRINT_VBO_REQUEST(request) \
  if (DRW_vbo_requested(mbc->request)) \
  fprintf(stderr, #request " requested\n")
#define PRINT_IBO_REQUEST(request) \
  if (DRW_ibo_requested(mbc->request)) \
  fprintf(stderr, #request " requested\n")

  PRINT_VBO_REQUEST(vbo.lnor);
  PRINT_VBO_REQUEST(vbo.pos_nor);
  PRINT_VBO_REQUEST(vbo.uv);
  PRINT_VBO_REQUEST(vbo.vcol);
  PRINT_VBO_REQUEST(vbo.sculpt_data);
  PRINT_VBO_REQUEST(vbo.weights);
  PRINT_VBO_REQUEST(vbo.edge_fac);
  PRINT_VBO_REQUEST(vbo.mesh_analysis);
  PRINT_VBO_REQUEST(vbo.tan);
  PRINT_VBO_REQUEST(vbo.orco);
  PRINT_VBO_REQUEST(vbo.edit_data);
  PRINT_VBO_REQUEST(vbo.fdots_pos);
  PRINT_VBO_REQUEST(vbo.fdots_nor);
  PRINT_VBO_REQUEST(vbo.skin_roots);
  PRINT_VBO_REQUEST(vbo.vert_idx);
  PRINT_VBO_REQUEST(vbo.edge_idx);
  PRINT_VBO_REQUEST(vbo.poly_idx);
  PRINT_VBO_REQUEST(vbo.fdot_idx);
  PRINT_VBO_REQUEST(vbo.edituv_data);
  PRINT_VBO_REQUEST(vbo.edituv_stretch_area);
  PRINT_VBO_REQUEST(vbo.edituv_stretch_angle);
  PRINT_VBO_REQUEST(vbo.fdots_uv);
  PRINT_VBO_REQUEST(vbo.fdots_edituv_data);

  PRINT_IBO_REQUEST(ibo.tris);
  PRINT_IBO_REQUEST(ibo.lines);
  PRINT_IBO_REQUEST(ibo.lines_loose);
  PRINT_IBO_REQUEST(ibo.lines_adjacency);
  PRINT_IBO_REQUEST(ibo.lines_paint_mask);
  PRINT_IBO_REQUEST(ibo.points);
  PRINT_IBO_REQUEST(ibo.fdots);
  PRINT_IBO_REQUEST(ibo.edituv_tris);
  PRINT_IBO_REQUEST(ibo.edituv_lines);
  PRINT_IBO_REQUEST(ibo.edituv_points);
  PRINT_IBO_REQUEST(ibo.edituv_fdots);

#undef PRINT_IBO_REQUEST
#undef PRINT_VBO_REQUEST
}

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

  const int number_of_quads = cache->num_patch_coords / 4;

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
          subdiv, mesh_eval, NULL, OPENSUBDIV_EVALUATOR_GLSL_COMPUTE, evaluator_cache)) {
    return false;
  }

  DRWSubdivCache *draw_cache = ensure_draw_cache(subdiv);
  if (!generate_required_cached_data(draw_cache, subdiv, mesh_eval, scene, smd, is_final_render)) {
    return false;
  }

  const bool do_hq_normals = (scene->r.perf_flag & SCE_PERF_HQ_NORMALS) != 0 ||
                             GPU_use_hq_normals_workaround();

  const bool optimal_display = (smd->flags & eSubsurfModifierFlag_ControlEdges);

  draw_cache->mesh = mesh_eval;
  draw_cache->subdiv = subdiv;
  draw_cache->do_hq_normals = do_hq_normals;
  draw_cache->optimal_display = optimal_display;
  draw_cache->number_of_triangles = tris_count_from_number_of_loops(draw_cache->num_patch_coords);
  /* We can only evaluate limit normals if the patches are adaptive. */
  draw_cache->do_limit_normals = settings.is_adaptive;

  if (DRW_ibo_requested(mbc->buff.ibo.tris)) {
    draw_subdiv_cache_ensure_mat_offsets(draw_cache, mesh_eval, batch_cache->mat_len);
  }

  draw_subdiv_cache_update_extra_coarse_face_data(draw_cache, mesh_eval);

  // print_requests(mbc);

  mesh_buffer_cache_create_requested_subdiv(batch_cache, mbc, draw_cache, toolsettings);

  // draw_subdiv_cache_print_memory_used(draw_cache);
  return true;
}

static OpenSubdiv_EvaluatorCache *g_evaluator_cache = NULL;

void DRW_create_subdivision(const Scene *scene,
                            Object *ob,
                            Mesh *mesh,
                            struct MeshBatchCache *batch_cache,
                            MeshBufferCache *mbc,
                            const ToolSettings *toolsettings)
{
  if (g_evaluator_cache == NULL) {
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
    g_evaluator_cache = NULL;
  }
}

static LinkNode *gpu_subdiv_free_queue = NULL;
static ThreadMutex gpu_subdiv_queue_mutex = BLI_MUTEX_INITIALIZER;

void DRW_subdiv_cache_free(Subdiv *subdiv)
{
  BLI_mutex_lock(&gpu_subdiv_queue_mutex);
  BLI_linklist_prepend(&gpu_subdiv_free_queue, subdiv);
  BLI_mutex_unlock(&gpu_subdiv_queue_mutex);
}

void DRW_cache_free_old_subdiv()
{
  if (gpu_subdiv_free_queue == NULL) {
    return;
  }

  BLI_mutex_lock(&gpu_subdiv_queue_mutex);

  while (gpu_subdiv_free_queue != NULL) {
    Subdiv *subdiv = static_cast<Subdiv *>(BLI_linklist_pop(&gpu_subdiv_free_queue));
    BKE_subdiv_free(subdiv);
  }

  BLI_mutex_unlock(&gpu_subdiv_queue_mutex);
}
