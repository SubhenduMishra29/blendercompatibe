
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

#ifdef LIMIT_NORMALS
layout(std430, binding = 2) readonly buffer inputDPDu
{
  vec3 input_dPdu[];
};

layout(std430, binding = 3) readonly buffer inputDPDv
{
  vec3 input_dPdv[];
};

layout(std430, binding = 4) writeonly buffer outputVertexData
{
  VertexBufferData output_verts[];
};
#else
layout(std430, binding = 2) writeonly buffer outputVertexData
{
  VertexBufferData output_verts[];
};
#endif

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

  vec3 nors[4];

#ifdef LIMIT_NORMALS
  for (int i = 0; i < 4; i++) {
    nors[i] = normalize(cross(input_dPdu[loop_index + i], input_dPdv[loop_index + i]));
  }
#else
  for (int i = 0; i < 4; i++) {
    nors[i] = vec3(0.0);
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
