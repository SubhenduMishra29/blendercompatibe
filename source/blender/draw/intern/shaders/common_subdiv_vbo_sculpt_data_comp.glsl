
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430) buffer;

struct SculptData {
  uint face_set_color;
  float mask;
};

layout(binding = 0) readonly restrict buffer sculptMask
{
  float sculpt_mask[];
};

layout(binding = 1) readonly restrict buffer faceSetColor
{
  uint face_set_color[];
};

layout(binding = 2) writeonly restrict buffer sculptData
{
  SculptData sculpt_data[];
};

void main()
{
  /* We execute for each quad. */
  uint quad_index = gl_GlobalInvocationID.x;
  uint start_loop_index = quad_index * 4;

  for (uint loop_index = start_loop_index; loop_index < start_loop_index + 4; loop_index++) {
    SculptData data;
    data.face_set_color = face_set_color[loop_index];

    if (has_sculpt_mask) {
      data.mask = sculpt_mask[loop_index];
    }
    else {
      data.mask = 0.0;
    }

    sculpt_data[loop_index] = data;
  }
}
