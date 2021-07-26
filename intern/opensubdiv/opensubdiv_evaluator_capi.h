// Copyright 2013 Blender Foundation. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// Author: Sergey Sharybin

#ifndef OPENSUBDIV_EVALUATOR_CAPI_H_
#define OPENSUBDIV_EVALUATOR_CAPI_H_

#ifdef __cplusplus
extern "C" {
#endif

struct OpenSubdiv_EvaluatorInternal;
struct OpenSubdiv_PatchCoord;
struct OpenSubdiv_TopologyRefiner;

typedef struct OpenSubdiv_BufferInterface {
  unsigned int (*bind)(struct OpenSubdiv_BufferInterface *buffer);

  void *(*alloc)(struct OpenSubdiv_BufferInterface *buffer, const unsigned int size);

  void (*device_alloc)(struct OpenSubdiv_BufferInterface *buffer, const unsigned int size);

  int (*num_vertices)(struct OpenSubdiv_BufferInterface *buffer);

  int buffer_offset;

  void (*wrap)(struct OpenSubdiv_BufferInterface *buffer, unsigned int device_ptr);

  void (*update_data)(struct OpenSubdiv_BufferInterface *buffer,
                      unsigned int start,
                      unsigned int len,
                      const void *data);

  void *data;
} OpenSubdiv_BufferInterface;

typedef struct OpenSubdiv_Evaluator {
  // Set coarse positions from a continuous array of coordinates.
  void (*setCoarsePositions)(struct OpenSubdiv_Evaluator *evaluator,
                             const float *positions,
                             const int start_vertex_index,
                             const int num_vertices);
  // Set varying data from a continuous array of data.
  void (*setVaryingData)(struct OpenSubdiv_Evaluator *evaluator,
                         const float *varying_data,
                         const int start_vertex_index,
                         const int num_vertices);
  // Set face varying data from a continuous array of data.
  //
  // TODO(sergey): Find a better name for vertex here. It is not the vertex of
  // geometry, but a vertex of UV map.
  void (*setFaceVaryingData)(struct OpenSubdiv_Evaluator *evaluator,
                             const int face_varying_channel,
                             const float *face_varying_data,
                             const int start_vertex_index,
                             const int num_vertices);

  // Set coarse vertex position from a continuous memory buffer where
  // first coordinate starts at offset of `start_offset` and there is `stride`
  // bytes between adjacent vertex coordinates.
  void (*setCoarsePositionsFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                       const void *buffer,
                                       const int start_offset,
                                       const int stride,
                                       const int start_vertex_index,
                                       const int num_vertices);
  // Set varying data from a continuous memory buffer where
  // first coordinate starts at offset of `start_offset` and there is `stride`
  // bytes between adjacent vertex coordinates.
  void (*setVaryingDataFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                   const void *buffer,
                                   const int start_offset,
                                   const int stride,
                                   const int start_vertex_index,
                                   const int num_vertices);
  // Set face varying data from a continuous memory buffer where
  // first coordinate starts at offset of `start_offset` and there is `stride`
  // bytes between adjacent vertex coordinates.
  //
  // TODO(sergey): Find a better name for vertex here. It is not the vertex of
  // geometry, but a vertex of UV map.
  void (*setFaceVaryingDataFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                       const int face_varying_channel,
                                       const void *buffer,
                                       const int start_offset,
                                       const int stride,
                                       const int start_vertex_index,
                                       const int num_vertices);

  // Refine after coarse positions update.
  void (*refine)(struct OpenSubdiv_Evaluator *evaluator);

  // Evaluate given ptex face at given bilinear coordinate.
  // If derivatives are NULL, they will not be evaluated.
  void (*evaluateLimit)(struct OpenSubdiv_Evaluator *evaluator,
                        const int ptex_face_index,
                        float face_u,
                        float face_v,
                        float P[3],
                        float dPdu[3],
                        float dPdv[3]);

  // Evaluate varying data at a given bilinear coordinate of given ptex face.
  void (*evaluateVarying)(struct OpenSubdiv_Evaluator *evaluator,
                          const int ptex_face_index,
                          float face_u,
                          float face_v,
                          float varying[3]);

  // Evaluate face-varying data at a given bilinear coordinate of given
  // ptex face.
  void (*evaluateFaceVarying)(struct OpenSubdiv_Evaluator *evaluator,
                              const int face_varying_channel,
                              const int ptex_face_index,
                              float face_u,
                              float face_v,
                              float face_varying[2]);

  void (*evaluateFaceVaryingFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                        const int face_varying_channel,
                                        OpenSubdiv_BufferInterface *patch_coords_buffer,
                                        OpenSubdiv_BufferInterface *face_varying_buffer);

  // Batched evaluation of multiple input coordinates.

  // Evaluate limit surface.
  // If derivatives are NULL, they will not be evaluated.
  //
  // NOTE: Output arrays must point to a memory of size float[3]*num_patch_coords.
  void (*evaluatePatchesLimit)(struct OpenSubdiv_Evaluator *evaluator,
                               const struct OpenSubdiv_PatchCoord *patch_coords,
                               const int num_patch_coords,
                               float *P,
                               float *dPdu,
                               float *dPdv);

  void (*evaluatePatchesLimitFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                         struct OpenSubdiv_BufferInterface *patch_coords,
                                         struct OpenSubdiv_BufferInterface *P,
                                         struct OpenSubdiv_BufferInterface *dPdu,
                                         struct OpenSubdiv_BufferInterface *dPdv);

  void (*buildPatchCoordsBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                 const struct OpenSubdiv_PatchCoord *patch_coords,
                                 int num_patch_coords,
                                 struct OpenSubdiv_BufferInterface *buffer);

  void (*getPatchMap)(struct OpenSubdiv_Evaluator *evaluator,
                      struct OpenSubdiv_BufferInterface *patch_map_handles,
                      struct OpenSubdiv_BufferInterface *patch_map_quadtree,
                      int *min_patch_face,
                      int *max_patch_face,
                      int *max_depth,
                      int *patches_are_triangular);

  void (*buildPatchArraysBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                 struct OpenSubdiv_BufferInterface *patch_array_buffer);

  void (*buildPatchIndexBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                struct OpenSubdiv_BufferInterface *patch_index_buffer);

  void (*buildPatchParamBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                struct OpenSubdiv_BufferInterface *patch_param_buffer);

  void (*buildSrcBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                         struct OpenSubdiv_BufferInterface *src_buffer);

  void (*buildFVarPatchArraysBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                     const int face_varying_channel,
                                     struct OpenSubdiv_BufferInterface *patch_array_buffer);

  void (*buildFVarPatchIndexBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                    const int face_varying_channel,
                                    struct OpenSubdiv_BufferInterface *patch_index_buffer);

  void (*buildFVarPatchParamBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                    const int face_varying_channel,
                                    struct OpenSubdiv_BufferInterface *patch_param_buffer);

  void (*buildFVarSrcBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                             const int face_varying_channel,
                             struct OpenSubdiv_BufferInterface *src_buffer);

  // Implementation of the evaluator.
  struct OpenSubdiv_EvaluatorImpl *impl;
} OpenSubdiv_Evaluator;

typedef struct OpenSubdiv_EvaluatorCache {
  // Implementation of the evaluator cache.
  struct OpenSubdiv_EvaluatorCacheImpl *impl;
} OpenSubdiv_EvaluatorCache;

OpenSubdiv_Evaluator *openSubdiv_createEvaluatorFromTopologyRefiner(
    struct OpenSubdiv_TopologyRefiner *topology_refiner,
    int evaluator_type,
    OpenSubdiv_EvaluatorCache *evaluator_cache);

void openSubdiv_deleteEvaluator(OpenSubdiv_Evaluator *evaluator);

OpenSubdiv_EvaluatorCache *openSubdiv_createEvaluatorCache(int evaluator_type);

void openSubdiv_deleteEvaluatorCache(OpenSubdiv_EvaluatorCache *evaluator_cache);

const char *openSubdiv_getGLSLPatchBasisSource(void);

#ifdef __cplusplus
}
#endif

#endif  // OPENSUBDIV_EVALUATOR_CAPI_H_
