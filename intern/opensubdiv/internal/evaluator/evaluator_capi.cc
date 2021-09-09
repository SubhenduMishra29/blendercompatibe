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
  evaluator->impl->eval_output->setCoarsePositions(positions, start_vertex_index, num_vertices);
}

void setVaryingData(OpenSubdiv_Evaluator *evaluator,
                    const float *varying_data,
                    const int start_vertex_index,
                    const int num_vertices)
{
  evaluator->impl->eval_output->setVaryingData(varying_data, start_vertex_index, num_vertices);
}

void setFaceVaryingData(OpenSubdiv_Evaluator *evaluator,
                        const int face_varying_channel,
                        const float *face_varying_data,
                        const int start_vertex_index,
                        const int num_vertices)
{
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
  evaluator->impl->eval_output->setFaceVaryingDataFromBuffer(
      face_varying_channel, buffer, start_offset, stride, start_vertex_index, num_vertices);
}

void refine(OpenSubdiv_Evaluator *evaluator)
{
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
  evaluator->impl->eval_output->evaluateLimit(ptex_face_index, face_u, face_v, P, dPdu, dPdv);
}

void evaluatePatchesLimit(OpenSubdiv_Evaluator *evaluator,
                          const OpenSubdiv_PatchCoord *patch_coords,
                          const int num_patch_coords,
                          float *P,
                          float *dPdu,
                          float *dPdv)
{
  evaluator->impl->eval_output->evaluatePatchesLimit(
      patch_coords, num_patch_coords, P, dPdu, dPdv);
}

void evaluateVarying(OpenSubdiv_Evaluator *evaluator,
                     const int ptex_face_index,
                     float face_u,
                     float face_v,
                     float varying[3])
{
  evaluator->impl->eval_output->evaluateVarying(ptex_face_index, face_u, face_v, varying);
}

void evaluateFaceVarying(OpenSubdiv_Evaluator *evaluator,
                         const int face_varying_channel,
                         const int ptex_face_index,
                         float face_u,
                         float face_v,
                         float face_varying[2])
{
  evaluator->impl->eval_output->evaluateFaceVarying(
      face_varying_channel, ptex_face_index, face_u, face_v, face_varying);
}

void getPatchMap(struct OpenSubdiv_Evaluator *evaluator,
                 struct GPUVertBuf **patch_map_handles,
                 struct GPUVertBuf **patch_map_quadtree,
                 int *min_patch_face,
                 int *max_patch_face,
                 int *max_depth,
                 int *patches_are_triangular)
{
  evaluator->impl->eval_output->getPatchMap(patch_map_handles,
                                            patch_map_quadtree,
                                            min_patch_face,
                                            max_patch_face,
                                            max_depth,
                                            patches_are_triangular);
}

GPUVertBuf *getPatchArraysBuffer(struct OpenSubdiv_Evaluator *evaluator)
{
  return evaluator->impl->eval_output->getPatchArraysBuffer();
}

GPUVertBuf *getWrappedPatchIndexBuffer(struct OpenSubdiv_Evaluator *evaluator)
{
  return evaluator->impl->eval_output->getWrappedPatchIndexBuffer();
}

GPUVertBuf *getWrappedPatchParamBuffer(struct OpenSubdiv_Evaluator *evaluator)
{
  return evaluator->impl->eval_output->getWrappedPatchParamBuffer();
}

GPUVertBuf *getWrappedSrcBuffer(struct OpenSubdiv_Evaluator *evaluator)
{
  return evaluator->impl->eval_output->getWrappedSrcBuffer();
}

GPUVertBuf *getFVarPatchArraysBuffer(struct OpenSubdiv_Evaluator *evaluator,
                                     const int face_varying_channel)
{
  return evaluator->impl->eval_output->getFVarPatchArraysBuffer(face_varying_channel);
}

GPUVertBuf *getWrappedFVarPatchIndexBuffer(struct OpenSubdiv_Evaluator *evaluator,
                                           const int face_varying_channel)
{
  return evaluator->impl->eval_output->getWrappedFVarPatchIndexBuffer(face_varying_channel);
}

GPUVertBuf *getWrappedFVarPatchParamBuffer(struct OpenSubdiv_Evaluator *evaluator,
                                           const int face_varying_channel)
{
  return evaluator->impl->eval_output->getWrappedFVarPatchParamBuffer(face_varying_channel);
}

GPUVertBuf *getWrappedFVarSrcBuffer(struct OpenSubdiv_Evaluator *evaluator,
                                    const int face_varying_channel,
                                    int *buffer_offset)
{
  return evaluator->impl->eval_output->getWrappedFVarSrcBuffer(face_varying_channel,
                                                               buffer_offset);
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

  evaluator->getPatchMap = getPatchMap;

  evaluator->getPatchArraysBuffer = getPatchArraysBuffer;
  evaluator->getWrappedPatchIndexBuffer = getWrappedPatchIndexBuffer;
  evaluator->getWrappedPatchParamBuffer = getWrappedPatchParamBuffer;
  evaluator->getWrappedSrcBuffer = getWrappedSrcBuffer;

  evaluator->getFVarPatchArraysBuffer = getFVarPatchArraysBuffer;
  evaluator->getWrappedFVarPatchIndexBuffer = getWrappedFVarPatchIndexBuffer;
  evaluator->getWrappedFVarPatchParamBuffer = getWrappedFVarPatchParamBuffer;
  evaluator->getWrappedFVarSrcBuffer = getWrappedFVarSrcBuffer;
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
