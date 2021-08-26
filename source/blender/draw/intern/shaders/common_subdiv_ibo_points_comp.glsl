
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputVertOrigIndices
{
  int input_vert_origindex[];
};

layout(std430, binding = 1) writeonly buffer outputPoints
{
  uint output_points[];
};

void main()
{
  /* We execute for each quad, so the start index of the loop is quad_index * 4. */
  uint start_loop_index = gl_GlobalInvocationID.x * 4;

  for (int i = 0; i < 4; i++) {
    int origindex = input_vert_origindex[start_loop_index + i];

    if (origindex != ORIGINDEX_NONE) {
      output_points[start_loop_index + i] = start_loop_index + i;
    }
    else {
      output_points[start_loop_index + i] = 0xffffffff;
    }
  }
}
