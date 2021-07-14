
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

/* This reprensents the layout for the pos_nor VBO that will be later used for drawing. */
struct VertexBufferData {
  vec4 pos;
  vec4 nor;
};

vec3 get_vertex_pos(VertexBufferData vertex_data)
{
  return vertex_data.pos.xyz;
}

vec3 get_vertex_nor(VertexBufferData vertex_data)
{
  return vertex_data.nor.xyz;
}

void set_vertex_pos(inout VertexBufferData vertex_data, vec3 pos)
{
  vertex_data.pos.xyz = pos;
}

void set_vertex_nor(inout VertexBufferData vertex_data, vec3 nor)
{
  vertex_data.nor.xyz = nor;
}

#define ORIGINDEX_NONE -1
