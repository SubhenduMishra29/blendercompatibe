
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputPositions
{
  vec3 input_verts[];
};

layout(std430, binding = 1) readonly buffer inputVertOrigIndices
{
  int input_vert_origindex[];
};

layout(std430, binding = 2) readonly buffer inputFaceFlags
{
  uint input_face_flags[];
};

layout(std430, binding = 3) readonly buffer inputSubdivPolygonOffset
{
  uint subdiv_polygon_offset[];
};

#ifdef SMOOTH_NORMALS
layout(std430, binding = 4) readonly buffer inputDPDu
{
  vec3 input_dPdu[];
};

layout(std430, binding = 5) readonly buffer inputDPDv
{
  vec3 input_dPdv[];
};

layout(std430, binding = 6) writeonly buffer outputVertexData
{
  VertexBufferData output_verts[];
};
#else
layout(std430, binding = 4) writeonly buffer outputVertexData
{
  VertexBufferData output_verts[];
};
#endif

uniform int coarse_poly_count;

/* Given the index of the subdivision quad, return the index of the corresponding coarse polygon.
 * This uses subdiv_polygon_offset and since it is a growing list of offsets, we can use binary
 * search to locate the right index. */
uint coarse_polygon_index_from_subdiv_loop_index(uint subdiv_quad_index)
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

void main()
{
  /* We execute for each quad. */
  uint quad_index = gl_GlobalInvocationID.x;
  uint loop_index = quad_index * 4;
  uint triangle_loop_index = quad_index * 6;

  vec3 verts[4];
  for (int i = 0; i < 4; i++) {
    verts[i] = input_verts[loop_index + i];
  }

  vec3 face_normal = normalize(cross(verts[1] - verts[0], verts[2] - verts[0]));
  vec3 nors[4];

#ifdef SMOOTH_NORMALS
  uint coarse_quad_index = coarse_polygon_index_from_subdiv_loop_index(quad_index);
  for (int i = 0; i < 4; i++) {
    if ((input_face_flags[coarse_quad_index] & 0xff) != 0) {
      nors[i] = normalize(cross(input_dPdu[loop_index + i], input_dPdv[loop_index + i]));
    }
    else {
      nors[i] = face_normal;
    }
  }
#else
  for (int i = 0; i < 4; i++) {
    nors[i] = face_normal;
  }
#endif

  // Flags:
  // -1.0 : face or vertex is hidden, or has no origindex
  //  1.0 : vertex is selected
  //  0.0 : nothing

  for (int i = 0; i < 4; i++) {
    VertexBufferData vertex_data;
    set_vertex_pos(vertex_data, verts[i]);
    set_vertex_nor(vertex_data, nors[i]);

    int origindex = input_vert_origindex[loop_index + i];

    if (origindex == -1) {
      vertex_data.nor.w = -1.0;
    }
    else {
      vertex_data.nor.w = 0.0;
    }

    output_verts[loop_index + i] = vertex_data;
  }
}
