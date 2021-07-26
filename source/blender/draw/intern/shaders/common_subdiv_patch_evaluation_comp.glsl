
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(std430) buffer;

// source and destination buffers

layout(binding = 0) buffer src_buffer
{
  float srcVertexBuffer[];
};

// GPUPatchMap
layout(binding = 1) readonly buffer inputPatchHandles
{
  PatchHandle input_patch_handles[];
};

layout(binding = 2) readonly buffer inputQuadNodes
{
  QuadNode quad_nodes[];
};

layout(binding = 3) readonly buffer inputPatchCoords
{
  BlenderPatchCoord patch_coords[];
};

layout(binding = 4) readonly buffer inputVertOrigIndices
{
  int input_vert_origindex[];
};

// patch buffers

layout(binding = 5) buffer patchArray_buffer
{
  OsdPatchArray patchArrayBuffer[];
};
layout(binding = 6) buffer patchIndex_buffer
{
  int patchIndexBuffer[];
};
layout(binding = 7) buffer patchParam_buffer
{
  OsdPatchParam patchParamBuffer[];
};

  // Outputs

#if defined(FVAR_EVALUATION)
layout(binding = 8) writeonly buffer outputFVarData
{
  vec2 output_fvar[];
};
#elif defined(FDOTS_EVALUATION)
/* For face dots, we build the position, normals, and index buffers in one go. */
layout(binding = 8) writeonly buffer outputVertices
{
  vec3 output_verts[];
};
layout(binding = 9) writeonly buffer outputNormals
{
  vec3 output_nors[];
};
layout(std430, binding = 10) writeonly buffer outputFdotsIndices
{
  uint output_indices[];
};
#else
layout(binding = 8) writeonly buffer outputVertexData
{
  PosNorLoop output_verts[];
};
#endif

vec2 read_vec2(int index)
{
  vec2 result;
  result.x = srcVertexBuffer[index * 2];
  result.y = srcVertexBuffer[index * 2 + 1];
  return result;
}

vec3 read_vec3(int index)
{
  vec3 result;
  result.x = srcVertexBuffer[index * 3];
  result.y = srcVertexBuffer[index * 3 + 1];
  result.z = srcVertexBuffer[index * 3 + 2];
  return result;
}

OsdPatchArray GetPatchArray(int arrayIndex)
{
  return patchArrayBuffer[arrayIndex];
}

OsdPatchParam GetPatchParam(int patchIndex)
{
  return patchParamBuffer[patchIndex];
}

// ------------------------------------------------------------------------------
// Patch Coordinate lookup. Return an OsdPatchCoord for the given patch_index and uvs.
// This code is a port of the OpenSubdiv PatchMap lookup code.

uniform int min_patch_face;
uniform int max_patch_face;
uniform int max_depth;
uniform int patches_are_triangular;

PatchHandle bogus_patch_handle()
{
  PatchHandle ret;
  ret.array_index = -1;
  ret.vertex_index = -1;
  ret.patch_index = -1;
  return ret;
}

int transformUVToQuadQuadrant(float median, inout float u, inout float v)
{
  int uHalf = (u >= median) ? 1 : 0;
  if (uHalf != 0)
    u -= median;

  int vHalf = (v >= median) ? 1 : 0;
  if (vHalf != 0)
    v -= median;

  return (vHalf << 1) | uHalf;
}

int transformUVToTriQuadrant(float median, inout float u, inout float v, inout bool rotated)
{

  if (!rotated) {
    if (u >= median) {
      u -= median;
      return 1;
    }
    if (v >= median) {
      v -= median;
      return 2;
    }
    if ((u + v) >= median) {
      rotated = true;
      return 3;
    }
    return 0;
  }
  else {
    if (u < median) {
      v -= median;
      return 1;
    }
    if (v < median) {
      u -= median;
      return 2;
    }
    u -= median;
    v -= median;
    if ((u + v) < median) {
      rotated = false;
      return 3;
    }
    return 0;
  }
}

PatchHandle find_patch(int face_index, float u, float v)
{
  if (face_index < min_patch_face || face_index > max_patch_face) {
    return bogus_patch_handle();
  }

  QuadNode node = quad_nodes[face_index - min_patch_face];

  if (!is_set(node.child[0])) {
    return bogus_patch_handle();
  }

  float median = 0.5;
  bool tri_rotated = false;

  for (int depth = 0; depth <= max_depth; ++depth, median *= 0.5) {
    int quadrant = (patches_are_triangular != 0) ?
                       transformUVToTriQuadrant(median, u, v, tri_rotated) :
                       transformUVToQuadQuadrant(median, u, v);

    if (is_leaf(node.child[quadrant])) {
      return input_patch_handles[get_index(node.child[quadrant])];
    }

    node = quad_nodes[get_index(node.child[quadrant])];
  }
}

OsdPatchCoord bogus_patch_coord(int face_index, float u, float v)
{
  OsdPatchCoord coord;
  coord.arrayIndex = 0;
  coord.patchIndex = face_index;
  coord.vertIndex = 0;
  coord.s = u;
  coord.t = v;
  return coord;
}

OsdPatchCoord GetPatchCoord(int face_index, float u, float v)
{
  PatchHandle patch_handle = find_patch(face_index, u, v);

  if (patch_handle.array_index == -1) {
    return bogus_patch_coord(face_index, u, v);
  }

  OsdPatchCoord coord;
  coord.arrayIndex = patch_handle.array_index;
  coord.patchIndex = patch_handle.patch_index;
  coord.vertIndex = patch_handle.vertex_index;
  coord.s = u;
  coord.t = v;
  return coord;
}

// ------------------------------------------------------------------------------
// Patch evaluation. Note that the 1st and 2nd derivatives are always computed, although we
// only return and use the 1st derivatives if adaptive patches are used. This could
// perhaps be optimized.

#if defined(FVAR_EVALUATION)
uniform int src_offset = 0;
void evaluate_patches_limits(int patch_index, float u, float v, inout vec2 dst)
{
  OsdPatchCoord coord = GetPatchCoord(patch_index, u, v);
  OsdPatchArray array = GetPatchArray(coord.arrayIndex);
  OsdPatchParam param = GetPatchParam(coord.patchIndex);

  int patchType = OsdPatchParamIsRegular(param) ? array.regDesc : array.desc;

  float wP[20], wDu[20], wDv[20], wDuu[20], wDuv[20], wDvv[20];
  int nPoints = OsdEvaluatePatchBasis(
      patchType, param, coord.s, coord.t, wP, wDu, wDv, wDuu, wDuv, wDvv);

  int indexBase = array.indexBase + array.stride * (coord.patchIndex - array.primitiveIdBase);

  for (int cv = 0; cv < nPoints; ++cv) {
    int index = patchIndexBuffer[indexBase + cv];
    vec2 src_fvar = read_vec2(src_offset + index);
    dst += src_fvar * wP[cv];
  }
}
#else
void evaluate_patches_limits(
    int patch_index, float u, float v, inout vec3 dst, inout vec3 du, inout vec3 dv)
{
  OsdPatchCoord coord = GetPatchCoord(patch_index, u, v);
  OsdPatchArray array = GetPatchArray(coord.arrayIndex);
  OsdPatchParam param = GetPatchParam(coord.patchIndex);

  int patchType = OsdPatchParamIsRegular(param) ? array.regDesc : array.desc;

  float wP[20], wDu[20], wDv[20], wDuu[20], wDuv[20], wDvv[20];
  int nPoints = OsdEvaluatePatchBasis(
      patchType, param, coord.s, coord.t, wP, wDu, wDv, wDuu, wDuv, wDvv);

  int indexBase = array.indexBase + array.stride * (coord.patchIndex - array.primitiveIdBase);

  for (int cv = 0; cv < nPoints; ++cv) {
    int index = patchIndexBuffer[indexBase + cv];
    vec3 src_vertex = read_vec3(index);

    dst += src_vertex * wP[cv];
    du += src_vertex * wDu[cv];
    dv += src_vertex * wDv[cv];
  }
}
#endif

// ------------------------------------------------------------------------------
// Entry point.

#if defined(FVAR_EVALUATION)
uniform int dst_offset = 0;
void main()
{
  uint quad_index = gl_GlobalInvocationID.x;
  uint start_loop_index = quad_index * 4;

  for (uint loop_index = start_loop_index; loop_index < start_loop_index + 4; loop_index++) {
    vec2 fvar = vec2(0.0);

    BlenderPatchCoord patch_co = patch_coords[loop_index];
    vec2 uv = decode_uv(patch_co.encoded_uv);

    evaluate_patches_limits(patch_co.patch_index, uv.x, uv.y, fvar);
    output_fvar[dst_offset + loop_index] = fvar;
  }
}
#elif defined(FDOTS_EVALUATION)
void main()
{
  /* The face dots evaluation is done for each coarse quads. */
  uint coarse_quad_index = gl_GlobalInvocationID.x;

  BlenderPatchCoord patch_co = patch_coords[coarse_quad_index];
  vec2 uv = decode_uv(patch_co.encoded_uv);

  vec3 pos = vec3(0.0);
  vec3 du = vec3(0.0);
  vec3 dv = vec3(0.0);
  evaluate_patches_limits(patch_co.patch_index, uv.x, uv.y, pos, du, dv);
  vec3 nor = normalize(cross(du, dv));

  output_verts[coarse_quad_index] = pos;
  output_nors[coarse_quad_index] = nor;
  output_indices[coarse_quad_index] = coarse_quad_index;
}
#else
void main()
{
  uint quad_index = gl_GlobalInvocationID.x;
  uint start_loop_index = quad_index * 4;

  for (uint loop_index = start_loop_index; loop_index < start_loop_index + 4; loop_index++) {
    vec3 pos = vec3(0.0);
    vec3 du = vec3(0.0);
    vec3 dv = vec3(0.0);

    BlenderPatchCoord patch_co = patch_coords[loop_index];
    vec2 uv = decode_uv(patch_co.encoded_uv);

    evaluate_patches_limits(patch_co.patch_index, uv.x, uv.y, pos, du, dv);

#  if defined(LIMIT_NORMALS)
    vec3 nor = normalize(cross(du, dv));
#  else
    // This will be computed later.
    vec3 nor = vec3(0.0);
#  endif

    int origindex = input_vert_origindex[loop_index];
    uint flag = 0;
    if (origindex == -1) {
      flag = -1;
    }

    PosNorLoop vertex_data;
    set_vertex_pos(vertex_data, pos);
    set_vertex_nor(vertex_data, nor, flag);
    output_verts[loop_index] = vertex_data;
  }
}
#endif
