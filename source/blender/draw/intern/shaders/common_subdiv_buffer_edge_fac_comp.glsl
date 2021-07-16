
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer inputVertexData
{
  VertexBufferData pos_nor[];
};

layout(std430, binding = 1) readonly buffer inputEdgeIndex
{
  uint input_edge_index[];
};

layout(std430, binding = 2) writeonly buffer outputEdgeFactors
{
  float output_edge_fac[];
};

uniform bool optimal_display;

// From extract_mesh_vbo_edge_fac.cc, keep in sync!
float loop_edge_factor_get(vec3 f_no, vec3 v_co, vec3 v_no, vec3 v_next_co)
{
  vec3 evec = v_next_co - v_co;
  vec3 enor = normalize(cross(v_no, evec));
  float d = abs(dot(enor, f_no));
  /* Re-scale to the slider range. */
  d *= (1.0 / 0.065);
  return clamp(d, 0.0, 1.0);
}

void emit_line(uint start_loop_index, uint corner_index, vec3 face_normal)
{
  uint vertex_index = start_loop_index + corner_index;
  uint edge_index = input_edge_index[vertex_index];

  if (edge_index == -1 && optimal_display) {
    output_edge_fac[vertex_index] = 0.0;
    return;
  }

  /* Mod 4 so we loop back at the first vertex on the last loop index (3). */
  uint next_vertex_index = start_loop_index + (corner_index + 1) % 4;

  vec3 vertex_pos = get_vertex_pos(pos_nor[vertex_index]);
  vec3 vertex_nor = get_vertex_nor(pos_nor[vertex_index]);
  vec3 next_vertex_pos = get_vertex_pos(pos_nor[next_vertex_index]);

  float factor = loop_edge_factor_get(face_normal, vertex_pos, vertex_nor, next_vertex_pos);
  output_edge_fac[vertex_index] = factor;
}

void main()
{
  /* We execute for each quad, so the start index of the loop is quad_index * 4. */
  uint start_loop_index = gl_GlobalInvocationID.x * 4;

  /* First compute the face normal, we need it to compute the bihedral edge angle. */
  vec3 v0 = get_vertex_pos(pos_nor[start_loop_index + 0]);
  vec3 v1 = get_vertex_pos(pos_nor[start_loop_index + 1]);
  vec3 v2 = get_vertex_pos(pos_nor[start_loop_index + 2]);
  vec3 face_normal = normalize(cross(v1 - v0, v2 - v0));

  for (int i = 0; i < 4; i++) {
    emit_line(start_loop_index, i, face_normal);
  }
}
