
/* This structure is a carbon copy of OpenSubDiv's PatchCoord. */
struct PatchCoord {
  int vertex_index;
  int array_index;
  int patch_index;
  float u;
  float v;
};

/* This reprensents the layout for the pos_nor VBO that will be later used for drawing. */
struct VertexBufferData {
  vec3 pos;
  vec3 nor;
};
