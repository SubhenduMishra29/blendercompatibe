// Copyright 2015 Blender Foundation. All rights reserved.
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

#include "opensubdiv_evaluator_capi.h"

#include <opensubdiv/osd/glslPatchShaderSource.h>

#include "MEM_guardedalloc.h"
#include <new>

#include "internal/evaluator/evaluator_impl.h"

namespace {

void setCoarsePositions(OpenSubdiv_Evaluator *evaluator,
                        const float *positions,
                        const int start_vertex_index,
                        const int num_vertices)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->setCoarsePositions(
        positions, start_vertex_index, num_vertices);
    return;
  }
  evaluator->impl->eval_output->setCoarsePositions(positions, start_vertex_index, num_vertices);
}

void setVaryingData(OpenSubdiv_Evaluator *evaluator,
                    const float *varying_data,
                    const int start_vertex_index,
                    const int num_vertices)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->setVaryingData(
        varying_data, start_vertex_index, num_vertices);
    return;
  }
  evaluator->impl->eval_output->setVaryingData(varying_data, start_vertex_index, num_vertices);
}

void setFaceVaryingData(OpenSubdiv_Evaluator *evaluator,
                        const int face_varying_channel,
                        const float *face_varying_data,
                        const int start_vertex_index,
                        const int num_vertices)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->setFaceVaryingData(
        face_varying_channel, face_varying_data, start_vertex_index, num_vertices);
    return;
  }
  evaluator->impl->eval_output->setFaceVaryingData(
      face_varying_channel, face_varying_data, start_vertex_index, num_vertices);
}

void setCoarsePositionsFromBuffer(OpenSubdiv_Evaluator *evaluator,
                                  const void *buffer,
                                  const int start_offset,
                                  const int stride,
                                  const int start_vertex_index,
                                  const int num_vertices)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->setCoarsePositionsFromBuffer(
        buffer, start_offset, stride, start_vertex_index, num_vertices);
    return;
  }
  evaluator->impl->eval_output->setCoarsePositionsFromBuffer(
      buffer, start_offset, stride, start_vertex_index, num_vertices);
}

void setVaryingDataFromBuffer(OpenSubdiv_Evaluator *evaluator,
                              const void *buffer,
                              const int start_offset,
                              const int stride,
                              const int start_vertex_index,
                              const int num_vertices)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->setVaryingDataFromBuffer(
        buffer, start_offset, stride, start_vertex_index, num_vertices);
    return;
  }
  evaluator->impl->eval_output->setVaryingDataFromBuffer(
      buffer, start_offset, stride, start_vertex_index, num_vertices);
}

void setFaceVaryingDataFromBuffer(OpenSubdiv_Evaluator *evaluator,
                                  const int face_varying_channel,
                                  const void *buffer,
                                  const int start_offset,
                                  const int stride,
                                  const int start_vertex_index,
                                  const int num_vertices)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->setFaceVaryingDataFromBuffer(
        face_varying_channel, buffer, start_offset, stride, start_vertex_index, num_vertices);
    return;
  }
  evaluator->impl->eval_output->setFaceVaryingDataFromBuffer(
      face_varying_channel, buffer, start_offset, stride, start_vertex_index, num_vertices);
}

void refine(OpenSubdiv_Evaluator *evaluator)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->refine();
    return;
  }
  evaluator->impl->eval_output->refine();
}

void evaluateLimit(OpenSubdiv_Evaluator *evaluator,
                   const int ptex_face_index,
                   const float face_u,
                   const float face_v,
                   float P[3],
                   float dPdu[3],
                   float dPdv[3])
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->evaluateLimit(
        ptex_face_index, face_u, face_v, P, dPdu, dPdv);
    return;
  }
  evaluator->impl->eval_output->evaluateLimit(ptex_face_index, face_u, face_v, P, dPdu, dPdv);
}

void evaluatePatchesLimit(OpenSubdiv_Evaluator *evaluator,
                          const OpenSubdiv_PatchCoord *patch_coords,
                          const int num_patch_coords,
                          float *P,
                          float *dPdu,
                          float *dPdv)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->evaluatePatchesLimit(
        patch_coords, num_patch_coords, P, dPdu, dPdv);
    return;
  }
  evaluator->impl->eval_output->evaluatePatchesLimit(
      patch_coords, num_patch_coords, P, dPdu, dPdv);
}

void evaluatePatchesLimitFromBuffer(OpenSubdiv_Evaluator *evaluator,
                                    OpenSubdiv_BufferInterface *patch_coords,
                                    OpenSubdiv_BufferInterface *P,
                                    OpenSubdiv_BufferInterface *dPdu,
                                    OpenSubdiv_BufferInterface *dPdv)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->evaluatePatchesLimit(patch_coords, P, dPdu, dPdv);
    return;
  }
}

void evaluateVarying(OpenSubdiv_Evaluator *evaluator,
                     const int ptex_face_index,
                     float face_u,
                     float face_v,
                     float varying[3])
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->evaluateVarying(ptex_face_index, face_u, face_v, varying);
    return;
  }
  evaluator->impl->eval_output->evaluateVarying(ptex_face_index, face_u, face_v, varying);
}

void evaluateFaceVarying(OpenSubdiv_Evaluator *evaluator,
                         const int face_varying_channel,
                         const int ptex_face_index,
                         float face_u,
                         float face_v,
                         float face_varying[2])
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->evaluateFaceVarying(
        face_varying_channel, ptex_face_index, face_u, face_v, face_varying);
    return;
  }
  evaluator->impl->eval_output->evaluateFaceVarying(
      face_varying_channel, ptex_face_index, face_u, face_v, face_varying);
}

void evaluateFaceVaryingFromBuffer(OpenSubdiv_Evaluator *evaluator,
                                   const int face_varying_channel,
                                   OpenSubdiv_BufferInterface *patch_coords_buffer,
                                   OpenSubdiv_BufferInterface *face_varying_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->evaluateFaceVarying(
        face_varying_channel, patch_coords_buffer, face_varying_buffer);
    return;
  }
}

void getPatchMap(struct OpenSubdiv_Evaluator *evaluator,
                 struct OpenSubdiv_BufferInterface *patch_map_handles,
                 struct OpenSubdiv_BufferInterface *patch_map_quadtree,
                 int *min_patch_face,
                 int *max_patch_face,
                 int *max_depth,
                 int *patches_are_triangular)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->getPatchMap(patch_map_handles,
                                                  patch_map_quadtree,
                                                  min_patch_face,
                                                  max_patch_face,
                                                  max_depth,
                                                  patches_are_triangular);
    return;
  }
}

void buildPatchArraysBuffer(struct OpenSubdiv_Evaluator *evaluator,
                            struct OpenSubdiv_BufferInterface *patch_array_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildPatchArraysBuffer(patch_array_buffer);
    return;
  }
}

void buildPatchIndexBuffer(struct OpenSubdiv_Evaluator *evaluator,
                           struct OpenSubdiv_BufferInterface *patch_index_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildPatchIndexBuffer(patch_index_buffer);
    return;
  }
}

void buildPatchParamBuffer(struct OpenSubdiv_Evaluator *evaluator,
                           struct OpenSubdiv_BufferInterface *patch_param_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildPatchParamBuffer(patch_param_buffer);
    return;
  }
}

void buildSrcBuffer(struct OpenSubdiv_Evaluator *evaluator,
                    struct OpenSubdiv_BufferInterface *src_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildSrcBuffer(src_buffer);
    return;
  }
}

void buildFVarPatchArraysBuffer(struct OpenSubdiv_Evaluator *evaluator,
                                const int face_varying_channel,
                                struct OpenSubdiv_BufferInterface *patch_array_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildFVarPatchArraysBuffer(face_varying_channel,
                                                                 patch_array_buffer);
    return;
  }
}

void buildFVarPatchIndexBuffer(struct OpenSubdiv_Evaluator *evaluator,
                               const int face_varying_channel,
                               struct OpenSubdiv_BufferInterface *patch_index_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildFVarPatchIndexBuffer(face_varying_channel,
                                                                patch_index_buffer);
    return;
  }
}

void buildFVarPatchParamBuffer(struct OpenSubdiv_Evaluator *evaluator,
                               const int face_varying_channel,
                               struct OpenSubdiv_BufferInterface *patch_param_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildFVarPatchParamBuffer(face_varying_channel,
                                                                patch_param_buffer);
    return;
  }
}

void buildFVarSrcBuffer(struct OpenSubdiv_Evaluator *evaluator,
                        const int face_varying_channel,
                        struct OpenSubdiv_BufferInterface *src_buffer)
{
  if (evaluator->impl->eval_output_gpu) {
    evaluator->impl->eval_output_gpu->buildFVarSrcBuffer(face_varying_channel, src_buffer);
    return;
  }
}

void assignFunctionPointers(OpenSubdiv_Evaluator *evaluator)
{
  evaluator->setCoarsePositions = setCoarsePositions;
  evaluator->setVaryingData = setVaryingData;
  evaluator->setFaceVaryingData = setFaceVaryingData;

  evaluator->setCoarsePositionsFromBuffer = setCoarsePositionsFromBuffer;
  evaluator->setVaryingDataFromBuffer = setVaryingDataFromBuffer;
  evaluator->setFaceVaryingDataFromBuffer = setFaceVaryingDataFromBuffer;

  evaluator->refine = refine;

  evaluator->evaluateLimit = evaluateLimit;
  evaluator->evaluateVarying = evaluateVarying;
  evaluator->evaluateFaceVarying = evaluateFaceVarying;

  evaluator->evaluatePatchesLimit = evaluatePatchesLimit;
  evaluator->evaluatePatchesLimitFromBuffer = evaluatePatchesLimitFromBuffer;

  evaluator->evaluateFaceVaryingFromBuffer = evaluateFaceVaryingFromBuffer;

  evaluator->getPatchMap = getPatchMap;

  evaluator->buildPatchArraysBuffer = buildPatchArraysBuffer;
  evaluator->buildPatchIndexBuffer = buildPatchIndexBuffer;
  evaluator->buildPatchParamBuffer = buildPatchParamBuffer;
  evaluator->buildSrcBuffer = buildSrcBuffer;

  evaluator->buildFVarPatchArraysBuffer = buildFVarPatchArraysBuffer;
  evaluator->buildFVarPatchIndexBuffer = buildFVarPatchIndexBuffer;
  evaluator->buildFVarPatchParamBuffer = buildFVarPatchParamBuffer;
  evaluator->buildFVarSrcBuffer = buildFVarSrcBuffer;
}

}  // namespace

OpenSubdiv_Evaluator *openSubdiv_createEvaluatorFromTopologyRefiner(
    OpenSubdiv_TopologyRefiner *topology_refiner,
    int evaluator_type,
    OpenSubdiv_EvaluatorCache *evaluator_cache)
{
  OpenSubdiv_Evaluator *evaluator = OBJECT_GUARDED_NEW(OpenSubdiv_Evaluator);
  assignFunctionPointers(evaluator);
  evaluator->impl = openSubdiv_createEvaluatorInternal(
      topology_refiner, evaluator_type, evaluator_cache ? evaluator_cache->impl : nullptr);
  return evaluator;
}

void openSubdiv_deleteEvaluator(OpenSubdiv_Evaluator *evaluator)
{
  openSubdiv_deleteEvaluatorInternal(evaluator->impl);
  OBJECT_GUARDED_DELETE(evaluator, OpenSubdiv_Evaluator);
}

OpenSubdiv_EvaluatorCache *openSubdiv_createEvaluatorCache(int evaluator_type)
{
  OpenSubdiv_EvaluatorCache *evaluator_cache = OBJECT_GUARDED_NEW(OpenSubdiv_EvaluatorCache);
  evaluator_cache->impl = openSubdiv_createEvaluatorCacheInternal(evaluator_type);
  return evaluator_cache;
}

void openSubdiv_deleteEvaluatorCache(OpenSubdiv_EvaluatorCache *evaluator_cache)
{
  if (!evaluator_cache) {
    return;
  }

  openSubdiv_deleteEvaluatorCacheInternal(evaluator_cache->impl);
  OBJECT_GUARDED_DELETE(evaluator_cache, OpenSubdiv_EvaluatorCache);
}

const char *openSubdiv_getGLSLPatchBasisSource(void)
{
  /* Using a global string to avoid dealing with memory allocation/ownership. */
  static std::string patch_basis_source;
  if (patch_basis_source.empty()) {
    patch_basis_source = OpenSubdiv::Osd::GLSLPatchShaderSource::GetPatchBasisShaderSource();
  }
  return patch_basis_source.c_str();
}
