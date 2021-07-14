
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

/* Inputs. */

layout(std430, binding = 0) readonly buffer inputFaceFlags
{
  int input_face_flag[];
};

layout(std430, binding = 1) readonly buffer inputPositions
{
  vec3 input_positions[];
};

layout(std430, binding = 2) readonly buffer inputDPDu
{
  vec3 input_dPdu[];
};

layout(std430, binding = 3) readonly buffer inputDPDv
{
  vec3 input_dPdv[];
};

/* Outputs. */

layout(std430, binding = 4) writeonly buffer outputFdotsPos
{
  vec3 output_face_dots_pos[];
};

layout(std430, binding = 5) writeonly buffer outputFdotsNor
{
  vec3 output_face_dots_nor[];
};

layout(std430, binding = 6) writeonly buffer outputFdotsIndices
{
  uint output_indices[];
};

void main()
{
  /* We execute for each coarse quad. */
  uint index = gl_GlobalInvocationID.x;

  output_face_dots_pos[index] = input_positions[index];
  output_face_dots_nor[index] = cross(input_dPdu[index], input_dPdv[index]);
  output_indices[index] = index;
}
