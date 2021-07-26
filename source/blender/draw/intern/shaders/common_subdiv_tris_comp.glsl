
/* Generate triangles from subdivision quads indices. */
layout(local_size_x = 1) in;

layout(std430, binding = 0) writeonly buffer outputTriangles
{
  uint output_tris[];
};

#ifndef SINGLE_MATERIAL
layout(std430, binding = 1) readonly buffer inputPolygonMatOffset
{
  int polygon_mat_offset[];
};

layout(std430, binding = 2) readonly buffer inputSubdivPolygonOffset
{
  uint subdiv_polygon_offset[];
};

uniform int coarse_poly_count;

/* Given the index of the subdivision quad, return the index of the corresponding coarse polygon.
 * This uses subdiv_polygon_offset and since it is a growing list of offsets, we can use binary
 * search to locate the right index.
 * TODO(kevindietrich): try to deduplicate this with the version in
 * common_subdiv_buffer_lnor_comp.glsl but we cannot pass an array to a function. */
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
#endif

void main()
{
  /* We execute for each quad. */
  uint quad_index = gl_GlobalInvocationID.x;
  uint loop_index = quad_index * 4;

#ifdef SINGLE_MATERIAL
  uint triangle_loop_index = quad_index * 6;
#else
  uint coarse_quad_index = coarse_polygon_index_from_subdiv_loop_index(quad_index);
  int mat_offset = polygon_mat_offset[coarse_quad_index];

  int triangle_loop_index = (int(quad_index) + mat_offset) * 6;
#endif

  output_tris[triangle_loop_index + 0] = loop_index + 0;
  output_tris[triangle_loop_index + 1] = loop_index + 1;
  output_tris[triangle_loop_index + 2] = loop_index + 2;
  output_tris[triangle_loop_index + 3] = loop_index + 0;
  output_tris[triangle_loop_index + 4] = loop_index + 2;
  output_tris[triangle_loop_index + 5] = loop_index + 3;
}
