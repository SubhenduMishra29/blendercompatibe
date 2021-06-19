
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 0) readonly buffer buffer0
{
  vec3 input_verts[];
};

layout(std430, binding = 1) writeonly buffer buffer1
{
  VertexBufferData output_verts[];
};

layout(std430, binding = 2) writeonly buffer buffer2
{
  uint output_tris[];
};

void main()
{
  uint quad_index = gl_GlobalInvocationID.x * 4;
  uint triangle_index = gl_GlobalInvocationID.x * 6;

  vec3 verts[4];
  for (int i = 0; i < 4; i++) {
    verts[i] = input_verts[quad_index + i];
  }

  vec3 nor = normalize(cross(verts[1] - verts[0], verts[2] - verts[0]));

  for (int i = 0; i < 4; i++) {
    output_verts[quad_index + i].pos = verts[i];
    output_verts[quad_index + i].nor = nor;
  }

  output_tris[triangle_index + 0] = quad_index + 0;
  output_tris[triangle_index + 1] = quad_index + 1;
  output_tris[triangle_index + 2] = quad_index + 2;
  output_tris[triangle_index + 3] = quad_index + 0;
  output_tris[triangle_index + 4] = quad_index + 2;
  output_tris[triangle_index + 5] = quad_index + 3;
}
