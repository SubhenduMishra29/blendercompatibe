/* Uniform block for #DRWSubivUboStorage. */
layout(std140) uniform shader_data
{
  /* Offsets in the buffers data where the source and destination data start. */
  int src_offset;
  int dst_offset;

  /* Parameters for the GPUPatchMap. */
  int min_patch_face;
  int max_patch_face;
  int max_depth;
  int patches_are_triangular;

  /* Coarse topology information. */
  int coarse_poly_count;

  /* Subdivision settings. */
  bool optimal_display;
};

/* This structure is a carbon copy of OpenSubDiv's PatchCoord. */
struct BlenderPatchCoord {
  int patch_index;
  uint encoded_uv;
};

vec2 decode_uv(uint encoded_uv)
{
  float u = float((encoded_uv >> 16) & 0xffff) / 65535.0;
  float v = float((encoded_uv) & 0xffff) / 65535.0;
  return vec2(u, v);
}

/* This structure is a carbon copy of OpenSubDiv's PatchTable::PatchHandle. */
struct PatchHandle {
  int array_index;
  int patch_index;
  int vertex_index;
};

/* This structure is a carbon copy of OpenSubDiv's PatchCoord. */
struct PatchCoord {
  int array_index;
  int patch_index;
  int vertex_index;
  float u;
  float v;
};

/* This structure is a carbon copy of OpenSubDiv's PatchCoord.QuadNode.
 * Each child is a bitfield. */
struct QuadNode {
  uvec4 child;
};

bool is_set(uint i)
{
  /* QuadNode.Child.isSet is the first bit of the bitfield. */
  return (i & 0x1) != 0;
}

bool is_leaf(uint i)
{
  /* QuadNode.Child.isLeaf is the second bit of the bitfield. */
  return (i & 0x2) != 0;
}

uint get_index(uint i)
{
  /* QuadNode.Child.index is made of the remaining bits. */
  return (i >> 2) & 0x3FFFFFFF;
}

/* Duplicate of #PosNorLoop from the mesh extract CPU code.
 * We do not use a vec3 for the position as it will be padded to a vec4 which is incompatible with
 * the format.  */
struct PosNorLoop {
  float x, y, z;
#ifdef HQ_NORMALS
  /* High quality normals are stored in 16-bits, so store 2 per uint. */
  uint nor_xy;
  uint nor_zw;
#else
  uint nor;
#endif
};

vec3 get_vertex_pos(PosNorLoop vertex_data)
{
  return vec3(vertex_data.x, vertex_data.y, vertex_data.z);
}

#ifdef HQ_NORMALS
float gpu_unpack_float_from_uint(uint x)
{
  return (float(x) - 32768.0) / 32767.0;
}

uint gpu_pack_uint_from_float(float x)
{
  return uint(clamp(x * 32767.0 + 32768.0, 0.0, 65535.0));
}

vec3 get_vertex_nor(PosNorLoop vertex_data)
{
  uint inor_xy = vertex_data.nor_xy;
  uint inor_zw = vertex_data.nor_zw;
  float x = gpu_unpack_float_from_uint((inor_xy >> 16) & 0xffff);
  float y = gpu_unpack_float_from_uint(inor_xy & 0xffff);
  float z = gpu_unpack_float_from_uint((inor_zw >> 16) & 0xffff);
  return vec3(x, y, z);
}

void compress_normal(vec3 nor, uint flag, out uint nor_xy, out uint nor_zw)
{
  uint x = gpu_pack_uint_from_float(nor.x);
  uint y = gpu_pack_uint_from_float(nor.y);
  uint z = gpu_pack_uint_from_float(nor.z);
  nor_xy = x << 16 | y;
  nor_zw = z << 16 | (flag & 0xffff);
}
#else
float gpu_unpack_float_from_uint(uint x)
{
  return (float(x) - 512.0) / 511.0;
}

uint gpu_pack_uint_from_float(float x)
{
  return uint(clamp(x * 511.0 + 512.0, 0.0, 1023.0));
}

vec3 get_vertex_nor(PosNorLoop vertex_data)
{
  uint inor = vertex_data.nor;
  float x = gpu_unpack_float_from_uint(inor & 0x3ff);
  float y = gpu_unpack_float_from_uint((inor >> 10) & 0x3ff);
  float z = gpu_unpack_float_from_uint((inor >> 20) & 0x3ff);
  return vec3(x, y, z);
}

uint compress_normal(vec3 nor, uint flag)
{
  uint x = gpu_pack_uint_from_float(nor.x);
  uint y = gpu_pack_uint_from_float(nor.y);
  uint z = gpu_pack_uint_from_float(nor.z);
  return x | y << 10 | z << 20 | flag << 30;
}
#endif

void set_vertex_pos(inout PosNorLoop vertex_data, vec3 pos)
{
  vertex_data.x = pos.x;
  vertex_data.y = pos.y;
  vertex_data.z = pos.z;
}

/* Set the vertex normal but preserve the existing flag. This is for when we compute manually the
 * vertex normals when we cannot use the limit surface, in which case the flag and the normal are
 * set by two separate compute pass. */
void set_vertex_nor(inout PosNorLoop vertex_data, vec3 nor)
{
#ifdef HQ_NORMALS
  uint flag = vertex_data.nor_zw & 0xffff;
  compress_normal(nor, flag, vertex_data.nor_xy, vertex_data.nor_zw);
#else
  uint flag = (vertex_data.nor >> 30) & 0x3;
  vertex_data.nor = compress_normal(nor, flag);
#endif
}

void set_vertex_nor(inout PosNorLoop vertex_data, vec3 nor, uint flag)
{
#ifdef HQ_NORMALS
  compress_normal(nor, flag, vertex_data.nor_xy, vertex_data.nor_zw);
#else
  vertex_data.nor = compress_normal(nor, flag);
#endif
}

#define ORIGINDEX_NONE -1

#ifdef SUBDIV_POLYGON_OFFSET
layout(std430, binding = 0) readonly buffer inputSubdivPolygonOffset
{
  uint subdiv_polygon_offset[];
};

/* Given the index of the subdivision quad, return the index of the corresponding coarse polygon.
 * This uses subdiv_polygon_offset and since it is a growing list of offsets, we can use binary
 * search to locate the right index. */
uint coarse_polygon_index_from_subdiv_quad_index(uint subdiv_quad_index, uint coarse_poly_count)
{
  uint first = 0;
  uint last = coarse_poly_count;

  while (first != last) {
    uint middle = (first + last) / 2;

    if (subdiv_polygon_offset[middle] < subdiv_quad_index) {
      first = middle + 1;
    }
    else {
      last = middle;
    }
  }

  if (subdiv_polygon_offset[first] == subdiv_quad_index) {
    return first;
  }

  return first - 1;
}
#endif
