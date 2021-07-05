
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

layout(std430, binding = 2) readonly buffer inputFlags
{
  int input_flags[];
};

#ifdef SMOOTH_NORMALS
layout(std430, binding = 3) readonly buffer inputDPDu
{
  vec3 input_dPdu[];
};

layout(std430, binding = 4) readonly buffer inputDPDv
{
  vec3 input_dPdv[];
};

layout(std430, binding = 5) writeonly buffer outputVertexData
{
  VertexBufferData output_verts[];
};

layout(std430, binding = 6) writeonly buffer outputTriangles
{
  uint output_tris[];
};
#else
layout(std430, binding = 3) writeonly buffer outputVertexData
{
  VertexBufferData output_verts[];
};

layout(std430, binding = 4) writeonly buffer outputTriangles
{
  uint output_tris[];
};
#endif

void main()
{
  uint quad_index = gl_GlobalInvocationID.x * 4;
  uint triangle_index = gl_GlobalInvocationID.x * 6;

  vec3 verts[4];
  for (int i = 0; i < 4; i++) {
    verts[i] = input_verts[quad_index + i];
  }

  vec3 face_normal = normalize(cross(verts[1] - verts[0], verts[2] - verts[0]));
  vec3 nors[4];

#ifdef SMOOTH_NORMALS
  for (int i = 0; i < 4; i++) {
    if ((input_flags[quad_index + i] & 0xff) != 0) {
      nors[i] = normalize(cross(input_dPdu[quad_index + i], input_dPdv[quad_index + i]));
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

    int origindex = input_vert_origindex[quad_index + i];

    if (origindex != -1) {
      vertex_data.nor.w = -1.0;
    }
    else {
      vertex_data.nor.w = 0.0;
    }

    output_verts[quad_index + i] = vertex_data;
  }

  output_tris[triangle_index + 0] = quad_index + 0;
  output_tris[triangle_index + 1] = quad_index + 1;
  output_tris[triangle_index + 2] = quad_index + 2;
  output_tris[triangle_index + 3] = quad_index + 0;
  output_tris[triangle_index + 4] = quad_index + 2;
  output_tris[triangle_index + 5] = quad_index + 3;
}
