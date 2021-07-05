
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputEdgeOrigIndex
{
  int input_origindex[];
};

layout(std430, binding = 1) writeonly buffer outputLinesIndices
{
  uint output_lines[];
};

void emit_line(uint line_offset, uint start_loop_index, uint corner_index)
{
  uint vertex_index = start_loop_index + corner_index;

  if (input_origindex[vertex_index] == ORIGINDEX_NONE) {
    output_lines[line_offset + 0] = 0xffffffff;
    output_lines[line_offset + 1] = 0xffffffff;
  }
  else {
    /* Mod 4 so we loop back at the first vertex on the last loop index (3). */
    uint next_vertex_index = start_loop_index + (corner_index + 1) % 4;

    output_lines[line_offset + 0] = vertex_index;
    output_lines[line_offset + 1] = next_vertex_index;
  }
}

void main()
{
  /* We execute for each quad, so the start index of the loop is quad_index * 4. */
  uint start_loop_index = gl_GlobalInvocationID.x * 4;
  /* We execute for each quad, so the start index of the line (2 vertex per line) is quad_index * 8. */
  uint start_line_index = gl_GlobalInvocationID.x * 8;

  for (int i = 0; i < 4; i++) {
    emit_line(start_line_index + i * 2, start_loop_index, i);
  }
}
