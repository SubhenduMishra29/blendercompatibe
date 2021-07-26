
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputVertexData
{
  PosNorLoop pos_nor[];
};

layout(std430, binding = 1) readonly buffer inputFaceFlags
{
  uint input_face_flags[];
};

layout(std430, binding = 2) readonly buffer inputSubdivPolygonOffset
{
  uint subdiv_polygon_offset[];
};

layout(std430, binding = 3) writeonly buffer outputLoopNormals
{
  vec3 output_lnor[];
};

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
  /* We execute for each quad, so the start index of the loop is quad_index * 4. */
  uint quad_index = gl_GlobalInvocationID.x;
  uint start_loop_index = quad_index * 4;

  uint coarse_quad_index = coarse_polygon_index_from_subdiv_loop_index(quad_index);

  if ((input_face_flags[coarse_quad_index] & 0xff) != 0) {
    /* Face is smooth, use vertex normals. */
    for (int i = 0; i < 4; i++) {
      output_lnor[start_loop_index + i] = get_vertex_nor(pos_nor[start_loop_index + i]);
    }
  }
  else {
    /* Face is flat shaded, compute flat face normal from an inscribed triangle. */
    vec3 verts[3];
    for (int i = 0; i < 3; i++) {
      verts[i] = get_vertex_pos(pos_nor[start_loop_index + i]);
    }

    vec3 face_normal = normalize(cross(verts[1] - verts[0], verts[2] - verts[0]));
    for (int i = 0; i < 4; i++) {
      output_lnor[start_loop_index + i] = face_normal;
    }
  }
}
