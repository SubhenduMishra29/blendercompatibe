/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputPatchHandles
{
  PatchHandle input_patch_handles[];
};

layout(std430, binding = 1) readonly buffer inputQuadNodes
{
  QuadNode quad_nodes[];
};

layout(std430, binding = 2) readonly buffer inputPatchCoords
{
  BlenderPatchCoord patch_coords[];
};

layout(std430, binding = 3) writeonly buffer outputPatchHandle
{
  PatchCoord output_patch_coords[];
};

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

PatchCoord bogus_patch_coord(int face_index, float u, float v)
{
  PatchCoord coord;
  coord.array_index = 0;
  coord.patch_index = face_index;
  coord.vertex_index = 0;
  coord.u = u;
  coord.v = v;
  return coord;
}

PatchCoord get_patch_coord(int face_index, float u, float v)
{
  PatchHandle patch_handle = find_patch(face_index, u, v);

  if (patch_handle.array_index == -1) {
    return bogus_patch_coord(face_index, u, v);
  }

  PatchCoord coord;
  coord.array_index = patch_handle.array_index;
  coord.patch_index = patch_handle.patch_index;
  coord.vertex_index = patch_handle.vertex_index;
  coord.u = u;
  coord.v = v;
  return coord;
}

void main()
{
  // foreach input, look up right patchcoord.
  uint index = gl_GlobalInvocationID.x;

  BlenderPatchCoord patch_co = patch_coords[index];
  vec2 uv = decode_uv(patch_co.encoded_uv);
  output_patch_coords[index] = get_patch_coord(patch_co.patch_index, uv.x, uv.y);
}
