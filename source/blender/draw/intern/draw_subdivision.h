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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "BLI_sys_types.h"

struct GPUIndexBuf;
struct GPUVertBuf;
struct Mesh;
struct MeshBatchCache;
struct MeshBufferCache;
struct MeshRenderData;
struct Object;
struct Scene;
struct Subdiv;
struct ToolSettings;

/* -------------------------------------------------------------------- */
/** \name GPUPatchMap is a GPU version of the OpenSubDiv PatchMap. The quad tree and the patch
 * handles are copied to GPU buffers in order to lookup the right patch for a given set of patch
 * coordinates.
 * \{ */

typedef struct GPUPatchMap {
  struct GPUVertBuf *patch_map_handles;
  struct GPUVertBuf *patch_map_quadtree;
  int min_patch_face;
  int max_patch_face;
  int max_depth;
  int patches_are_triangular;
} GPUPatchMap;

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache holds the various buffers used to evaluate and render subdivision through
 * OpenGL.
 * \{ */

typedef struct DRWSubdivCache {
  struct Mesh *mesh;
  struct Subdiv *subdiv;
  bool optimal_display;
  bool do_hq_normals;
  bool do_limit_normals;

  /* Coordinates used to evaluate patches for UVs, positions, and normals. */
  struct GPUVertBuf *patch_coords;
  /* Coordinates used to evaluate patches for the face centers (or face dots) in edit-mode. */
  struct GPUVertBuf *fdots_patch_coords;

  /* Resolution used to generate the patch coordinates. */
  int resolution;

  /* Number of subdivided loops, also the number of patch coordinates since we have one coordinate
   * but quad corner/vertex. */
  uint num_subdiv_loops;
  uint num_subdiv_edges;
  uint num_subdiv_triangles;
  uint num_subdiv_vertis;
  uint num_subdiv_quads;

  /* Number of polygons in the coarse mesh, notably used to compute a coarse polygon index given a
   * subdivision loop index. */
  int num_coarse_poly;

  /* Maps subdivision loop to subdivided vertex index. */
  int *subdiv_loop_subdiv_vert_index;
  /* Maps subdivision loop to original coarse poly index. */
  int *subdiv_loop_poly_index;

  /* Indices of faces adjacent to the vertices, ordered by vertex index, with no particular
   * winding. */
  struct GPUVertBuf *subdiv_vertex_face_adjacency;
  /* The difference between value (i + 1) and (i) gives the number of faces adjacent to vertex (i).
   */
  struct GPUVertBuf *subdiv_vertex_face_adjacency_offsets;

  /* Maps subdivision loop to original coarse vertex index, only really useful for edit mode. */
  struct GPUVertBuf *verts_orig_index;
  /* Maps subdivision loop to original coarse edge index, only really useful for edit mode. */
  struct GPUVertBuf *edges_orig_index;

  /* Owned by #Subdiv. Indexed by coarse polygon index, difference between value (i + 1) and (i)
   * gives the number of ptex faces for coarse polygon (i).  */
  int *face_ptex_offset;
  /* Vertex buffer for face_ptex_offset. */
  struct GPUVertBuf *face_ptex_offset_buffer;

  int *subdiv_polygon_offset;
  struct GPUVertBuf *subdiv_polygon_offset_buffer;

  /* Contains the start loop index and the smooth flag for each coarse polygon. */
  struct GPUVertBuf *extra_coarse_face_data;

  /* Computed for ibo.points, one value per subdivided vertex, mapping coarse vertices ->
   * subdivided loop */
  int *point_indices;

  /* Material offsets. */
  int *mat_start;
  int *mat_end;
  struct GPUVertBuf *polygon_mat_offset;

  GPUPatchMap gpu_patch_map;
} DRWSubdivCache;

/** \} */

void DRW_create_subdivision(const struct Scene *scene,
                            struct Object *ob,
                            struct Mesh *mesh,
                            struct MeshBatchCache *batch_cache,
                            struct MeshBufferCache *mbc,
                            const struct ToolSettings *toolsettings);

void DRW_subdiv_free(void);

void DRW_subdiv_cache_free(struct Subdiv *subdiv);

void draw_subdiv_init_mesh_render_data(struct Mesh *mesh,
                                       struct MeshRenderData *mr,
                                       const struct ToolSettings *toolsettings);

void draw_subdiv_init_origindex_buffer(struct GPUVertBuf *buffer,
                                       int *vert_origindex,
                                       uint num_loops,
                                       uint loose_len);

struct GPUVertBuf *draw_subdiv_build_origindex_buffer(int *vert_origindex, uint num_loops);

/* Compute shader functions. */

void draw_subdiv_accumulate_normals(const DRWSubdivCache *cache,
                                    struct GPUVertBuf *pos_nor,
                                    struct GPUVertBuf *face_adjacency_offsets,
                                    struct GPUVertBuf *face_adjacency_lists,
                                    struct GPUVertBuf *vertex_normals);

void draw_subdiv_finalize_normals(const DRWSubdivCache *cache,
                                  struct GPUVertBuf *vertex_normals,
                                  struct GPUVertBuf *subdiv_loop_subdiv_vert_index,
                                  struct GPUVertBuf *pos_nor,
                                  const bool do_hq_normals);

void draw_subdiv_extract_pos_nor(const DRWSubdivCache *cache,
                                 struct GPUVertBuf *pos_nor,
                                 const bool do_limit_normals,
                                 const bool do_hq_normals);

void draw_subdiv_interp_custom_data(const DRWSubdivCache *cache,
                                    struct GPUVertBuf *src_data,
                                    struct GPUVertBuf *dst_buffer,
                                    int dimensions,
                                    int dst_offset);

void draw_subdiv_extract_uvs(const DRWSubdivCache *cache,
                             struct GPUVertBuf *uvs,
                             const int face_varying_channel,
                             const int dst_offset);

void draw_subdiv_build_edge_fac_buffer(const DRWSubdivCache *cache,
                                       struct GPUVertBuf *pos_nor,
                                       struct GPUVertBuf *edge_idx,
                                       bool optimal_display,
                                       struct GPUVertBuf *edge_fac,
                                       const bool do_hq_normals);

void draw_subdiv_build_tris_buffer(const DRWSubdivCache *cache,
                                   struct GPUIndexBuf *subdiv_tris,
                                   const int material_count);

void draw_subdiv_build_lines_buffer(const DRWSubdivCache *cache,
                                    struct GPUIndexBuf *lines_indices,
                                    const bool optimal_display);

void draw_subdiv_build_fdots_buffers(const DRWSubdivCache *cache,
                                     struct GPUVertBuf *fdots_pos,
                                     struct GPUVertBuf *fdots_nor,
                                     struct GPUIndexBuf *fdots_indices);

void draw_subdiv_build_lnor_buffer(const DRWSubdivCache *cache,
                                   struct GPUVertBuf *pos_nor,
                                   struct GPUVertBuf *lnor,
                                   const bool do_hq_normals);

#ifdef __cplusplus
}
#endif
