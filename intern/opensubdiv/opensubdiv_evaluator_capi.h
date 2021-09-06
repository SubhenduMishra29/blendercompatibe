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

// Interface for doing input/output operations on buffers.
// Useful to abstract GPU buffers.
typedef struct OpenSubdiv_BufferInterface {
  // Bind the buffer to the GPU and return the device pointer, or buffer name in OpenGL parlance.
  unsigned int (*bind_gpu)(const struct OpenSubdiv_BufferInterface *buffer);

  // Allocate the buffer on the host for the given size in bytes.
  void *(*alloc)(const struct OpenSubdiv_BufferInterface *buffer, const unsigned int size);

  // Allocate the buffer directly on the device for the given size in bytes.
  void (*device_alloc)(const struct OpenSubdiv_BufferInterface *buffer, const unsigned int size);

  // Return the number of individual elements in the buffer.
  int (*num_vertices)(const struct OpenSubdiv_BufferInterface *buffer);

  // Wrap an existing GPU buffer, given its device pointer or buffer name in OpenGL parlance, for
  // read-only use.
  void (*wrap)(const struct OpenSubdiv_BufferInterface *buffer, unsigned int device_ptr);

  // Update the given range of the buffer with new data.
  void (*update_data)(const struct OpenSubdiv_BufferInterface *buffer,
                      unsigned int start,
                      unsigned int len,
                      const void *data);

  // Offset in the buffer where the data starts, if a single buffer is used for multiple data
  // channels.
  int buffer_offset;

  // Pointer to user defined data.
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

  // Evaluate face-varying data from a given array of bilinear coordinates.
  void (*evaluateFaceVaryingFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                        const int face_varying_channel,
                                        const OpenSubdiv_BufferInterface *patch_coords_buffer,
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

  // Evaluate the limit surface at the given patch coordinates.
  void (*evaluatePatchesLimitFromBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                         const struct OpenSubdiv_BufferInterface *patch_coords,
                                         struct OpenSubdiv_BufferInterface *P,
                                         struct OpenSubdiv_BufferInterface *dPdu,
                                         struct OpenSubdiv_BufferInterface *dPdv);

  // Copy the patch map to the given buffers, and output some topology information.
  void (*getPatchMap)(struct OpenSubdiv_Evaluator *evaluator,
                      struct OpenSubdiv_BufferInterface *patch_map_handles,
                      struct OpenSubdiv_BufferInterface *patch_map_quadtree,
                      int *min_patch_face,
                      int *max_patch_face,
                      int *max_depth,
                      int *patches_are_triangular);

  // Fill the given buffer with data from the evaluator's patch array buffer.
  void (*wrapPatchArraysBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                struct OpenSubdiv_BufferInterface *patch_array_buffer);

  // Fill the given buffer with data from the evaluator's patch index buffer.
  void (*wrapPatchIndexBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                               struct OpenSubdiv_BufferInterface *patch_index_buffer);

  // Fill the given buffer with data from the evaluator's patch parameter buffer.
  void (*wrapPatchParamBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                               struct OpenSubdiv_BufferInterface *patch_param_buffer);

  // Fill the given buffer with data from the evaluator's source buffer.
  void (*wrapSrcBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                        struct OpenSubdiv_BufferInterface *src_buffer);

  // Fill the given buffer with data from the evaluator's face varying patch array buffer.
  void (*wrapFVarPatchArraysBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                    const int face_varying_channel,
                                    struct OpenSubdiv_BufferInterface *patch_array_buffer);

  // Fill the given buffer with data from the evaluator's face varying patch index buffer.
  void (*wrapFVarPatchIndexBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                   const int face_varying_channel,
                                   struct OpenSubdiv_BufferInterface *patch_index_buffer);

  // Fill the given buffer with data from the evaluator's face varying patch parameter buffer.
  void (*wrapFVarPatchParamBuffer)(struct OpenSubdiv_Evaluator *evaluator,
                                   const int face_varying_channel,
                                   struct OpenSubdiv_BufferInterface *patch_param_buffer);

  // Fill the given buffer with data from the evaluator's face varying source buffer.
  void (*wrapFVarSrcBuffer)(struct OpenSubdiv_Evaluator *evaluator,
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

// Return the GLSL source code from the OpenSubDiv library used for patch evaluation.
const char *openSubdiv_getGLSLPatchBasisSource(void);

#ifdef __cplusplus
}
#endif

#endif  // OPENSUBDIV_EVALUATOR_CAPI_H_
