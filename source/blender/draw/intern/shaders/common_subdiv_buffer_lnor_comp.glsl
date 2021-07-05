
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputVertexData
{
  VertexBufferData pos_nor[];
};

layout(std430, binding = 1) writeonly buffer outputLoopNormals
{
  vec3 output_lnor[];
};

void main()
{
  /* We execute for each quad, so the start index of the loop is quad_index * 4. */
  uint start_loop_index = gl_GlobalInvocationID.x * 4;

  for (int i = 0; i < 4; i++) {
    output_lnor[start_loop_index + i] = get_vertex_nor(pos_nor[start_loop_index + i]);
  }
}
