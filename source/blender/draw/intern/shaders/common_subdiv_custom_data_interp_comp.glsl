
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430) buffer;

layout(binding = 0) readonly restrict buffer sourceBuffer
{
  float src_data[];
};

layout(binding = 1) readonly restrict buffer subdivPolygonOffset
{
  uint subdiv_polygon_offset[];
};

layout(binding = 2) readonly restrict buffer facePTexOffset
{
  uint face_ptex_offset[];
};

layout(binding = 3) readonly restrict buffer patchCoords
{
  BlenderPatchCoord patch_coords[];
};

layout(binding = 4) readonly restrict buffer extraCoarseFaceData
{
  uint extra_coarse_face_data[];
};

layout(binding = 5) writeonly restrict buffer destBuffer
{
  float dst_data[];
};

uniform int dst_offset = 0;
uniform int coarse_poly_count = 0;

vec4 read_vec4(uint index)
{
  uint base_index = index * 4;
  vec4 result;
  result.x = src_data[base_index + 0];
  result.y = src_data[base_index + 1];
  result.z = src_data[base_index + 2];
  result.w = src_data[base_index + 3];
  return result;
}

void write_vec4(uint index, vec4 data)
{
  uint base_index = dst_offset + index * 4;
  dst_data[base_index + 0] = data.x;
  dst_data[base_index + 1] = data.y;
  dst_data[base_index + 2] = data.z;
  dst_data[base_index + 3] = data.w;
}

/* Given the index of the subdivision quad, return the index of the corresponding coarse polygon.
 * This uses subdiv_polygon_offset and since it is a growing list of offsets, we can use binary
 * search to locate the right index.
 * TODO(kevindietrich): try to deduplicate this with the version in common_subdiv_tris_comp.glsl
 * but we cannot pass an array to a function. */
uint coarse_polygon_index_from_subdiv_quad_index(uint subdiv_quad_index)
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

uint get_vertex_count(uint coarse_polygon)
{
  uint number_of_patches = face_ptex_offset[coarse_polygon + 1] - face_ptex_offset[coarse_polygon];
  if (number_of_patches == 1) {
    /* If there is only one patch for the current coarse polygon, then it is a quad. */
    return 4;
  }
  /* Otherwise, the number of patches is the number of vertices. */
  return number_of_patches;
}

uint get_polygon_corner_index(uint coarse_polygon, uint patch_index)
{
  uint patch_offset = face_ptex_offset[coarse_polygon];
  return patch_index - patch_offset;
}

uint get_loop_start(uint coarse_polygon)
{
  return extra_coarse_face_data[coarse_polygon] & 0x7fffffff;
}

vec4 interp_vec4(vec4 a, vec4 b, vec4 c, vec4 d, vec2 uv)
{
  vec4 e = mix(a, b, uv.x);
  vec4 f = mix(c, d, uv.x);
  return mix(e, f, uv.y);
}

void main()
{
  /* We execute for each quad. */
  uint quad_index = gl_GlobalInvocationID.x;
  uint start_loop_index = quad_index * 4;

  /* Find which coarse polygon we came from. */
  uint coarse_polygon = coarse_polygon_index_from_subdiv_quad_index(quad_index);
  uint loop_start = get_loop_start(coarse_polygon);

  /* Find the number of vertices for the coarse polygon. */
  vec4 d0 = vec4(0.0);
  vec4 d1 = vec4(0.0);
  vec4 d2 = vec4(0.0);
  vec4 d3 = vec4(0.0);

  uint number_of_vertices = get_vertex_count(coarse_polygon);
  if (number_of_vertices == 4) {
    // Interpolate the src data.
    d0 = read_vec4(loop_start + 0);
    d1 = read_vec4(loop_start + 1);
    d2 = read_vec4(loop_start + 2);
    d3 = read_vec4(loop_start + 3);
  }
  else {
    // Interpolate the src data for the center.
    uint loop_end = loop_start + number_of_vertices - 1;
    vec4 center_value = vec4(0.0);

    for (uint l = loop_start; l < loop_end; l++) {
      center_value += read_vec4(l);
    }

    center_value /= float(number_of_vertices);

    // Interpolate between the previous and next corner for the middle values for the edges.
    uint patch_index = uint(patch_coords[start_loop_index].patch_index);
    uint current_coarse_corner = get_polygon_corner_index(coarse_polygon, patch_index);
    uint next_coarse_corner = (current_coarse_corner + 1) % number_of_vertices;
    uint prev_coarse_corner = (current_coarse_corner + number_of_vertices - 1) % number_of_vertices;

    d0 = read_vec4(loop_start);
    d1 = (d0 + read_vec4(loop_start + next_coarse_corner)) * 0.5;
    d3 = (d0 + read_vec4(loop_start + prev_coarse_corner)) * 0.5;

    // Interpolate between the current value, and the ones for the center and mid-edges.
    d2 = center_value;
  }

  /* Do a linear interpolation of the data based on the UVs for each loop of this subdivided quad. */
  for (uint loop_index = start_loop_index; loop_index < start_loop_index + 4; loop_index++) {
    BlenderPatchCoord co = patch_coords[loop_index];
    vec2 uv = decode_uv(co.encoded_uv);
    /* NOTE: d2 and d3 are reversed to stay consistent with the interpolation weight on the x-axis:
     *
     * d3 +-----+ d2
     *    |     |
     *    |     |
     * d0 +-----+ d1
     *
     * otherwise, weight would be `1.0 - uv.x` for `d2 <-> d3`, but `uv.x` for `d0 <-> d1`.
     */
    vec4 result = interp_vec4(d0, d1, d3, d2, uv);
    write_vec4(loop_index, result);
  }
}
