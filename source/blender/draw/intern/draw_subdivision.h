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

struct Mesh;
struct Object;
struct Scene;
struct MeshBatchCache;
struct MeshBufferCache;
struct MeshRenderData;
struct Subdiv;
struct ToolSettings;

void DRW_create_subdivision(const struct Scene *scene,
                            struct Object *ob,
                            struct Mesh *mesh,
                            struct MeshBatchCache *batch_cache,
                            struct MeshBufferCache *mbc,
                            const struct ToolSettings *toolsettings);

void DRW_subdiv_free(void);

void DRW_subdiv_cache_free(struct Subdiv *subdiv);
#include "BLI_sys_types.h"

struct GPUVertBuf;
struct GPUIndexBuf;
struct GPUPatchMap;
struct MemArena;

typedef struct LooseVertex {
  struct LooseVertex *next;
  int coarse_vertex_index;
  int subdiv_vertex_index;
  float co[3];
} LooseVertex;

typedef struct LooseEdge {
  struct LooseEdge *next;
  uint v1;
  uint v2;
  int coarse_edge_index;
} LooseEdge;

typedef struct GPUPatchMap {
  struct GPUVertBuf *patch_map_handles;
  struct GPUVertBuf *patch_map_quadtree;
  int min_patch_face;
  int max_patch_face;
  int max_depth;
  int patches_are_triangular;
} GPUPatchMap;

typedef struct DRWSubdivCache {
  struct Mesh *mesh;
  struct Subdiv *subdiv;
  bool optimal_display;
  bool do_hq_normals;
  bool do_limit_normals;

  /* Coordinates used to evaluate patches, for UVs, positions, and normals. */
  struct GPUVertBuf *patch_coords;
  struct GPUVertBuf *fdots_patch_coords;

  /* Resolution used to generate the patch coordinates. */
  int resolution;

  /* Number of coordinantes. */
  uint num_patch_coords;

  uint number_of_triangles;

  int coarse_poly_count;
  uint num_vertices;  // subdiv_vertex_count;

  uint num_edges;
  uint number_of_quads;

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

  /* Maps to original element in the coarse mesh, only for edit mode. */
  /* Maps subdivision loop to original coarse vertex index. */
  struct GPUVertBuf *verts_orig_index;
  /* Maps subdivision loop to original coarse edge index. */
  struct GPUVertBuf *edges_orig_index;

  struct GPUVertBuf *face_ptex_offset_buffer;
  int *face_ptex_offset;
  struct GPUVertBuf *subdiv_polygon_offset_buffer;
  int *subdiv_polygon_offset;

  /* Contains the start loop index and the smooth flag for each coarse polygon. */
  struct GPUVertBuf *extra_coarse_face_data;

  /* ibo.points, one value per subdivided vertex, mapping coarse vertices -> subdivided loop */
  int *point_indices;

  /* Material offsets. */
  int *mat_start;
  int *mat_end;
  struct GPUVertBuf *polygon_mat_offset;

  GPUPatchMap gpu_patch_map;

  /* Loose Vertices and Edges */
  struct MemArena *loose_memarena;
  struct LooseVertex *loose_verts;
  struct LooseEdge *loose_edges;

  int vert_loose_len;
  int edge_loose_len;
  int loop_loose_len;
} DRWSubdivCache;

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
