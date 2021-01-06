/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2021, Blender Foundation.
 */

#include "draw_subdivision.h"

#include <list>

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_customdata.h"
#include "BKE_subdiv.h"

#include "BKE_editmesh.h"
#include "BKE_mesh.h"
#include "BKE_subdiv.h"
#include "intern/subdiv_converter.h"

#include "BLI_math_vector.h"

#include "GPU_batch.h"
#include "GPU_compute.h"
#include "GPU_state.h"

#include "opengl/gl_batch.hh"
#include "opengl/gl_index_buffer.hh"
#include "opengl/gl_vertex_buffer.hh"

#include <opensubdiv/osd/cpuEvaluator.h>
#include <opensubdiv/osd/cpuGLVertexBuffer.h>

#include <opensubdiv/osd/glComputeEvaluator.h>
#include <opensubdiv/osd/glVertexBuffer.h>

#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/far/topologyRefinerFactory.h>

#include <opensubdiv/osd/glMesh.h>

#include "internal/topology/topology_refiner_impl.h"
#include "opensubdiv_converter_capi.h"
#include "opensubdiv_topology_refiner_capi.h"

using namespace OpenSubdiv;

#undef DEBUG_SUBDIV_CODE

#ifdef DEBUG_SUBDIV_CODE
#  define DEGBUG_INFO_PARAM(name) , DebugInfo *name
#  define PASS_DEBUG_INFO(name) , name
#else
#  define DEGBUG_INFO_PARAM(name)
#  define PASS_DEBUG_INFO(name)
#endif

#ifdef DEBUG_SUBDIV_CODE
static const char *GPU_prim_type_str(GPUPrimType prim_type)
{
#  define CASE(x) \
    case x: \
      return #x;
  switch (prim_type) {
    CASE(GPU_PRIM_POINTS)
    CASE(GPU_PRIM_LINES)
    CASE(GPU_PRIM_TRIS)
    CASE(GPU_PRIM_LINE_STRIP)
    CASE(GPU_PRIM_LINE_LOOP)
    CASE(GPU_PRIM_TRI_STRIP)
    CASE(GPU_PRIM_TRI_FAN)
    CASE(GPU_PRIM_LINES_ADJ)
    CASE(GPU_PRIM_TRIS_ADJ)
    CASE(GPU_PRIM_LINE_STRIP_ADJ)
    CASE(GPU_PRIM_PATCHES)
    CASE(GPU_PRIM_NONE)
  }
#  undef CASE
  return "ERROR";
}

static const char *OSD_patch_desc_type_str(Far::PatchDescriptor::Type type)
{
#  define CASE(x) \
    case Far::PatchDescriptor::x: \
      return #x;
  switch (type) {
    CASE(NON_PATCH)
    CASE(POINTS)
    CASE(LINES)
    CASE(QUADS)
    CASE(TRIANGLES)
    CASE(LOOP)
    CASE(REGULAR)
    CASE(GREGORY)
    CASE(GREGORY_BOUNDARY)
    CASE(GREGORY_BASIS)
    CASE(GREGORY_TRIANGLE)
  }
#  undef CASE
  return "ERROR";
}

struct DebugInfo {
  std::string name = "";

  Osd::MeshBitset bits{};

  int level = 0;
  int vertex_count = 0;
  int vbo_vertex_count = 0;
  int subdivided_vertex_count = 0;
  int subdivided_vbo_vertex_count = 0;

  int orig_vertex_buffer_id = 0;

  /* buffers from OpenSubDiv */
  int vertex_buffer_id = 0;
  int index_buffer_id = 0;

  GPUPrimType batch_prim_type = GPU_PRIM_NONE;

  size_t patch_count = 0;

  struct PatchDebugInfo {
    int elements;
    Far::PatchDescriptor::Type type;
    int index_base;
    int offset;
  };

  std::vector<PatchDebugInfo> patches;

  void clear()
  {
    patches.clear();
  }

  void print(std::ostream &os)
  {
    os << "Subdivision data for " << name << ":\n";
    os << "-- original vertex count : " << vertex_count << '\n';
    os << "-- original VBO vertex count : " << vbo_vertex_count << '\n';
    os << "-- subdivided vertex count : " << subdivided_vertex_count << '\n';
    os << "-- subdivided VBO vertex count : " << subdivided_vbo_vertex_count << '\n';
    os << "-- subdivision level : " << level << '\n';
    os << "-- vertex buffer id : " << vertex_buffer_id << '\n';
    os << "-- orig vertex buffer id : " << orig_vertex_buffer_id << '\n';
    os << "-- index buffer id : " << index_buffer_id << '\n';
    os << "-- batch prim type : " << GPU_prim_type_str(batch_prim_type) << '\n';

    os << "-- patch count : " << patch_count << '\n';

    for (PatchDebugInfo pdi : patches) {
      os << "  patch:\n";
      os << "  -- elements   : " << pdi.elements << '\n';
      os << "  -- type       : " << OSD_patch_desc_type_str(pdi.type) << '\n';
      os << "  -- index_base : " << pdi.index_base << '\n';
      os << "  -- offset     : " << pdi.offset << '\n';
    }

    os << "-- subdivision bits:\n";
#  define TEST_BIT(bit, message) \
    if (bits.test(bit)) \
    std::cerr << "    " << message << "\n"
    TEST_BIT(Osd::MeshAdaptive, "adaptive subdivision");
    TEST_BIT(Osd::MeshUseSmoothCornerPatch, "smooth corner path");
    TEST_BIT(Osd::MeshUseSingleCreasePatch, "single crease patch");
    TEST_BIT(Osd::MeshUseInfSharpPatch, "infinitely sharp patch");
    TEST_BIT(Osd::MeshInterleaveVarying, "interleaved varying data");
    TEST_BIT(Osd::MeshFVarData, "face varying data");
    TEST_BIT(Osd::MeshEndCapBilinearBasis, "end cap bilinear");
    TEST_BIT(Osd::MeshEndCapBSplineBasis, "end cap b-spline");
    TEST_BIT(Osd::MeshEndCapGregoryBasis, "end cap gregory");
    TEST_BIT(Osd::MeshEndCapLegacyGregory, "end cap legacy gregory");
#  undef TEST_BIT
  }
};

struct DebugInfoCache {
  std::list<DebugInfo> debug_infos;

  DebugInfo *info_for_name(const std::string &name)
  {
    for (DebugInfo &info : debug_infos) {
      if (info.name == name) {
        return &info;
      }
    }

    DebugInfo debug_info;
    debug_info.name = name;
    debug_infos.push_back(debug_info);
    return &debug_infos.back();
  }
};

static DebugInfoCache debug_info_cache;
#endif

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {
namespace Far {
template<>
// TODO(@kevindietrich) : make use of code from intern/opensubdiv
bool TopologyRefinerFactory<Mesh>::resizeComponentTopology(TopologyRefiner &refiner,
                                                           Mesh const &mesh)
{
  setNumBaseVertices(refiner, mesh.totvert);
  setNumBaseFaces(refiner, mesh.totpoly);

  for (int i = 0; i < mesh.totpoly; i++) {
    MPoly *mpoly = &mesh.mpoly[i];
    setNumBaseFaceVertices(refiner, i, mpoly->totloop);
  }

  return true;
}

template<>
bool TopologyRefinerFactory<Mesh>::assignComponentTopology(TopologyRefiner &refiner,
                                                           Mesh const &mesh)
{
  for (int i = 0; i < mesh.totpoly; i++) {
    MPoly *mpoly = &mesh.mpoly[i];
    MLoop *mloop = &mesh.mloop[mpoly->loopstart];

    IndexArray face_verts = getBaseFaceVertices(refiner, i);

    for (int j = 0; j < mpoly->totloop; ++j, ++mloop) {
      face_verts[j] = static_cast<int>(mloop->v);
    }
  }

  return true;
}

template<>
bool TopologyRefinerFactory<Mesh>::assignComponentTags(TopologyRefiner &refiner, Mesh const &mesh)
{
  /* Historical maximum crease weight used at Pixar, influencing the maximum in OpenSubDiv. */
  static constexpr float CREASE_SCALE = 10.0f;

  if (mesh.edit_mesh) {
    BMesh *bm = mesh.edit_mesh->bm;

    {
      BMIter iter;
      BMFace *efa;
      int i;
      BM_ITER_MESH_INDEX (efa, &iter, bm, BM_FACES_OF_MESH, i) {
        if (BM_elem_flag_test(efa, BM_ELEM_HOLE)) {
          setBaseFaceHole(refiner, i, true);
        }
      }
    }

    const int cd_edge_crease_offset = CustomData_get_offset(&bm->edata, CD_CREASE);
    if (cd_edge_crease_offset != -1) {
      BM_mesh_elem_index_ensure(bm, BM_VERT);
      BMIter iter;
      BMEdge *eee;
      int i;
      BM_ITER_MESH_INDEX (eee, &iter, bm, BM_EDGES_OF_MESH, i) {
        Index edge = findBaseEdge(refiner, BM_elem_index_get(eee->v1), BM_elem_index_get(eee->v2));

        if (edge != INDEX_INVALID) {
          float sharpness = BKE_subdiv_crease_to_sharpness_f(
              BM_ELEM_CD_GET_FLOAT(eee, cd_edge_crease_offset));
          setBaseEdgeSharpness(refiner, edge, sharpness);
        }
      }
    }

    const int cd_vertex_crease_offset = CustomData_get_offset(&bm->vdata, CD_CREASE);
    if (cd_vertex_crease_offset != -1) {
      BMIter iter;
      BMVert *eve;
      int i;
      BM_ITER_MESH_INDEX (eve, &iter, bm, BM_VERTS_OF_MESH, i) {
        float sharpness = BKE_subdiv_crease_to_sharpness_f(
            BM_ELEM_CD_GET_FLOAT(eve, cd_vertex_crease_offset));

        ConstIndexArray vert_edges = getBaseVertexEdges(refiner, i);

        if (vert_edges.size() == 2) {
          const float sharpness0 = refiner.getLevel(0).getEdgeSharpness(vert_edges[0]);
          const float sharpness1 = refiner.getLevel(0).getEdgeSharpness(vert_edges[1]);

          sharpness += std::min(sharpness0, sharpness1);
          sharpness = std::min(sharpness, CREASE_SCALE);
        }

        if (sharpness != 0.0f) {
          setBaseVertexSharpness(refiner, i, sharpness);
        }
      }
    }
  }
  else {
    for (int i = 0; i < mesh.totpoly; i++) {
      MPoly *mpoly = &mesh.mpoly[i];
      setBaseFaceHole(refiner, i, mpoly->flag & ME_HOLE);
    }

    for (int i = 0; i < mesh.totedge; i++) {
      const MEdge *medge = &mesh.medge[i];

      if (medge->crease == 0) {
        continue;
      }

      Index edge = findBaseEdge(refiner, medge->v1, medge->v2);

      if (edge != INDEX_INVALID) {
        const float sharpness = BKE_subdiv_crease_to_sharpness_char(medge->crease);
        setBaseEdgeSharpness(refiner, edge, sharpness);
      }
    }

    const float *cd_vertex_crease = static_cast<const float *>(
        CustomData_get_layer(&mesh.vdata, CD_CREASE));
    if (cd_vertex_crease) {
      for (int i = 0; i < mesh.totvert; ++i) {
        float sharpness = BKE_subdiv_crease_to_sharpness_f(cd_vertex_crease[i]);

        ConstIndexArray vert_edges = getBaseVertexEdges(refiner, i);

        if (vert_edges.size() == 2) {
          const float sharpness0 = refiner.getLevel(0).getEdgeSharpness(vert_edges[0]);
          const float sharpness1 = refiner.getLevel(0).getEdgeSharpness(vert_edges[1]);

          sharpness += std::min(sharpness0, sharpness1);
          sharpness = std::min(sharpness, CREASE_SCALE);
        }

        if (sharpness != 0.0f) {
          setBaseVertexSharpness(refiner, i, sharpness);
        }
      }
    }
  }

  return true;
}

template<>
bool TopologyRefinerFactory<Mesh>::assignFaceVaryingTopology(TopologyRefiner & /*refiner*/,
                                                             Mesh const & /*mesh*/)
{
  return true;
}

template<>
void TopologyRefinerFactory<Mesh>::reportInvalidTopology(TopologyError /*err_code*/,
                                                         char const *msg,
                                                         Mesh const & /*mesh*/)
{
  std::cerr << "There is an error chief : " << msg << '\n';
}
} /* namespace Far */
} /* namespace OPENSUBDIV_VERSION */
} /* namespace OpenSubdiv */

extern "C" {

/* Interface to access the VBO from OpenSubDiv. */
class VBOAccessor {
 public:
  /* Required interfaces */

  static VBOAccessor *Create(int num_elements, int num_vertices, void *device_context = nullptr)
  {
    static_cast<void>(device_context);
    return new VBOAccessor(num_elements, num_vertices);
  }

  int GetNumElements() const
  {
    return num_elements_;
  }

  int GetNumVertices() const
  {
    return num_vertices_;
  }

  GLuint BindVBO(void *device_context = nullptr)
  {
    static_cast<void>(device_context);
    /* make sure the VBO is created */
    GPU_vertbuf_use(vertex_buffer_);
    return get_gl_vert_buf()->vbo_id();
  }

  void UpdateData(float const * /*vertex_data*/,
                  int /*start_vertex*/,
                  int /*num_verts*/,
                  void *device_context = nullptr)
  {
    static_cast<void>(device_context);
    /* Symbolic function for the code to compile, this should be called by clients to fill the
     * vertex buffer but we do not need nor use it since we do that externally.
     */
  }

  /* Own interfaces. */

  blender::gpu::GLVertBuf *get_gl_vert_buf()
  {
    return static_cast<blender::gpu::GLVertBuf *>(blender::gpu::unwrap(vertex_buffer_));
  }

  GPUVertBuf *get_vert_buf()
  {
    return vertex_buffer_;
  }

  void initialize_vertex_buffer(GPUVertBuf *vertex_buffer)
  {
    if (vertex_buffer == vertex_buffer_) {
      return;
    }

    static GPUVertFormat format = {0};
    if (format.attr_len == 0) {
      /* WARNING Adjust #PosNorLoop struct accordingly. */
      GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
      GPU_vertformat_attr_add(&format, "nor", GPU_COMP_I10, 4, GPU_FETCH_INT_TO_FLOAT_UNIT);
      GPU_vertformat_alias_add(&format, "vnor");
    }

    GPU_vertbuf_init_with_format_ex(vertex_buffer, &format, GPU_USAGE_DYNAMIC);

    // TODO(@kevindietrich) : this overallocates on the host, num_vertices is coarse vertex count +
    // refined vertex count We should allocate for both on the device, but only for coarse vertices
    // on the host, and only copy coarse vertex count worth of data during updates.
    GPU_vertbuf_data_alloc(vertex_buffer, num_vertices_);

    this->vertex_buffer_ = vertex_buffer;
  }

 protected:
  VBOAccessor(int num_elements, int num_vertices)
      : num_elements_(num_elements), num_vertices_(num_vertices)
  {
  }

 private:
  int num_elements_;
  int num_vertices_;
  GPUVertBuf *vertex_buffer_ = nullptr;
};

// Duplicate of Mesh drawing code
// TODO(@kevindietrich) : remove normals from the VBO, compute them in the shader.
typedef struct PosNorLoop {
  float pos[3];
  GPUPackedNormal nor;
} PosNorLoop;

static void fill_vertex_buffer(Mesh &mesh, GPUVertBuf *vbo)
{
  PosNorLoop *vbo_data = (PosNorLoop *)GPU_vertbuf_get_data(vbo);

  /* Edit mode */
  if (mesh.edit_mesh) {
    BMesh *bm = mesh.edit_mesh->bm;
    BMIter iter;
    BMVert *eve;
    int i;

    BM_ITER_MESH_INDEX (eve, &iter, bm, BM_VERTS_OF_MESH, i) {
      copy_v3_v3(vbo_data[i].pos, eve->co);
    }
  }
  else {
    for (auto i = 0; i < mesh.totvert; ++i) {
      copy_v3_v3(vbo_data[i].pos, mesh.mvert[i].co);
    }
  }
}

using OsdMeshType =
    Osd::Mesh<VBOAccessor, Osd::GLStencilTableSSBO, Osd::GLComputeEvaluator, Osd::GLPatchTable>;

static void setup_vertex_buffer(GPUVertBuf *vertex_buffer, Mesh *mesh, OsdMeshType *osd_mesh)
{
  VBOAccessor *vbo_accessor = osd_mesh->GetVertexBuffer();
  vbo_accessor->initialize_vertex_buffer(vertex_buffer);

  fill_vertex_buffer(*mesh, vertex_buffer);
}

static void do_subdivision(OsdMeshType *osd_mesh)
{
  osd_mesh->Refine();

  /* Wait on workers to finish. */
  osd_mesh->Synchronize();
}

static GPUPrimType gpu_prim_type_for_desc_type(Far::PatchDescriptor::Type desc_type)
{
  switch (desc_type) {
    case Far::PatchDescriptor::NON_PATCH: {
      return GPU_PRIM_NONE;
    }
    case Far::PatchDescriptor::POINTS: {
      return GPU_PRIM_POINTS;
    }
    case Far::PatchDescriptor::LINES: {
      return GPU_PRIM_LINES;
    }
    case Far::PatchDescriptor::QUADS: {
      return GPU_PRIM_LINES_ADJ;
    }
    case Far::PatchDescriptor::TRIANGLES: {
      return GPU_PRIM_TRIS;
    }
    case Far::PatchDescriptor::LOOP: {
      /* Loops are regular triangular patches. */
      return GPU_PRIM_TRIS;
    }
    case Far::PatchDescriptor::REGULAR:
    case Far::PatchDescriptor::GREGORY:
    case Far::PatchDescriptor::GREGORY_BOUNDARY:
    case Far::PatchDescriptor::GREGORY_BASIS:
    case Far::PatchDescriptor::GREGORY_TRIANGLE: {
#if 0  // defined(GL_ARB_tessellation_shader) || defined(GL_VERSION_4_0)
       // TODO(@kevindietrich) : GL_PATCH_VERTICES
       // glPatchParameteri(GL_PATCH_VERTICES, effectDesc.desc.GetNumControlVertices());
      return GPU_PRIM_PATCHES;
#else
      return GPU_PRIM_POINTS;
#endif
    }
  }

  return GPU_PRIM_NONE;
}

static GPUShader *g_quads_to_tris_compute_shader = nullptr;

#if 0
static GPUShader *SUBD_get_quads_to_tris_shader()
{
  if (g_quads_to_tris_compute_shader == nullptr) {
    static const char *compute_code = R"(

    layout(local_size_x = 1) in;

    layout(std430, binding = 0) readonly buffer buffer0 {
       uint inputQuads[];
    };

    layout(std430, binding = 1) writeonly buffer buffer1 {
       uint outputTriangles[];
    };

    void main()
    {
        uint index = gl_GlobalInvocationID.x * 4;

        uint i0 = inputQuads[index + 0];
        uint i1 = inputQuads[index + 1];
        uint i2 = inputQuads[index + 2];
        uint i3 = inputQuads[index + 3];

        uint triangle_index = index / 4 * 6;

        outputTriangles[triangle_index + 0] = i0;
        outputTriangles[triangle_index + 1] = i1;
        outputTriangles[triangle_index + 2] = i2;

        outputTriangles[triangle_index + 3] = i0;
        outputTriangles[triangle_index + 4] = i2;
        outputTriangles[triangle_index + 5] = i3;
    }

    )";

    g_quads_to_tris_compute_shader = GPU_shader_create_compute(compute_code, nullptr, nullptr, "compute_osd");
  }

  return g_quads_to_tris_compute_shader;
}

static void do_quads_to_tris_compute(GPUIndexBuf *quads, GPUIndexBuf *tris)
{
  const uint32_t index_len = GPU_indexbuf_get_index_len(quads);
  const uint32_t number_of_quads = index_len / 4;

  GPUShader *shader = SUBD_get_quads_to_tris_shader();
  GPU_shader_bind(shader);

  GPU_indexbuf_bind_as_ssbo(quads, 0);
  GPU_indexbuf_bind_as_ssbo(tris, 1);

  GPU_compute_dispatch(shader, number_of_quads, 1, 1);

  /* Check if compute has been done. */
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void compute_tris_and_normals(GPUVertBuf *vbo,
                                     GPUIndexBuf *input,
                                     const uint32_t index_len,
                                     GPUIndexBuf *r_index_buf)
{
  const uint32_t number_of_quads = index_len / 4;
  const uint32_t number_of_triangles = number_of_quads * 2;
  const uint32_t triangles_index_len = number_of_triangles * 3;

  blender::gpu::GLIndexBuf *gl_index_buffer = reinterpret_cast<blender::gpu::GLIndexBuf *>(r_index_buf);
  gl_index_buffer->set_index_len(triangles_index_len);
  gl_index_buffer->set_index_type(blender::gpu::GPU_INDEX_U32);
  gl_index_buffer->tag_init();

  do_quads_to_tris_compute(input, r_index_buf);

  // TODO(@kevindietrich): compute normals
}
#else
static void compute_tris_and_normals(GPUVertBuf *vbo,
                                     GPUIndexBuf *input,
                                     const uint32_t index_len,
                                     GPUIndexBuf *r_index_buf)
{
  blender::gpu::GLIndexBuf *gl_index_buffer = reinterpret_cast<blender::gpu::GLIndexBuf *>(input);
  gl_index_buffer->bind();
  const uint32_t *elem = GPU_indexbuf_read(input);

  if (!elem) {
    return;
  }

  const uint32_t size = index_len;
  assert(index_len % 4 == 0);

  /* Size is the number of indices, divided by 4 gives us the number of quads, multiplying by 6
   * gives us the number of triangle vertex indices. */
  const uint32_t triangle_size = size / 4 * 6;

  uint32_t *triangles = static_cast<uint32_t *>(
      MEM_mallocN(sizeof(uint32_t) * triangle_size, "triangles"));
  uint32_t *triangles_ptr = triangles;
  uint32_t max_index = 0u;

  const PosNorLoop *pos_nor_const = static_cast<const PosNorLoop *>(GPU_vertbuf_read(vbo));
  PosNorLoop *pos_nor = static_cast<PosNorLoop *>(GPU_vertbuf_unmap(vbo, pos_nor_const));

  const uint32_t vertex_len = GPU_vertbuf_get_vertex_len(vbo);

  struct Normal {
    float x, y, z;
  };

  std::vector<Normal> normals(vertex_len);

  for (Normal &nor : normals) {
    nor.x = 0.0f;
    nor.y = 0.0f;
    nor.z = 0.0f;
  }

  for (auto i = 0u; i < size; i += 4) {
    const uint32_t i0 = elem[i];
    const uint32_t i1 = elem[i + 1];
    const uint32_t i2 = elem[i + 2];
    const uint32_t i3 = elem[i + 3];

    float e0[3];
    float e1[3];

    sub_v3_v3v3(e0, pos_nor[i0].pos, pos_nor[i1].pos);
    sub_v3_v3v3(e1, pos_nor[i0].pos, pos_nor[i2].pos);

    float nor[3];
    cross_v3_v3v3(nor, e0, e1);

    normalize_v3(nor);

    add_v3_v3(&normals[i0].x, nor);
    add_v3_v3(&normals[i1].x, nor);
    add_v3_v3(&normals[i2].x, nor);
    add_v3_v3(&normals[i3].x, nor);

    max_index = std::max(max_index, i0);
    max_index = std::max(max_index, i1);
    max_index = std::max(max_index, i2);
    max_index = std::max(max_index, i3);

    *triangles_ptr++ = i0;
    *triangles_ptr++ = i1;
    *triangles_ptr++ = i2;

    *triangles_ptr++ = i0;
    *triangles_ptr++ = i2;
    *triangles_ptr++ = i3;
  }

  for (uint32_t i = 0; i < vertex_len; ++i) {
    normalize_v3(&normals[i].x);
    pos_nor[i].nor = GPU_normal_convert_i10_v3(&normals[i].x);
  }

  GPU_vertbuf_use(vbo);
  GPU_vertbuf_update_sub(vbo, 0, vertex_len * sizeof(PosNorLoop), pos_nor);

  MEM_freeN(pos_nor);

  GPUIndexBufBuilder index_buf_builder;
  index_buf_builder.data = triangles;
  index_buf_builder.max_allowed_index = max_index;
  index_buf_builder.index_len = triangle_size;
  index_buf_builder.max_index_len = triangle_size;
  index_buf_builder.prim_type = GPU_PRIM_TRIS;
  GPU_indexbuf_build_in_place(&index_buf_builder, r_index_buf);
}
#endif

static void setup_index_buffer(GPUIndexBuf *index_buf,
                               OsdMeshType *osd_mesh DEGBUG_INFO_PARAM(debug_info))
{
  OsdMeshType::PatchTable *patch_table = osd_mesh->GetPatchTable();
#ifdef DEBUG_SUBDIV_CODE
  debug_info->index_buffer_id = patch_table->GetPatchIndexBuffer();

  /* For debug prints. */
  static_cast<blender::gpu::GLBatch *>(surface_batch)->is_subdivision_batch = true;
#endif

  GPUIndexBuf *tmp_index_buffer = GPU_indexbuf_calloc();
  blender::gpu::GLIndexBuf *gl_index_buffer = reinterpret_cast<blender::gpu::GLIndexBuf *>(
      tmp_index_buffer);

  uint32_t index_len = 0;

  for (const auto &array : patch_table->GetPatchArrays()) {
    const auto &desc = array.GetDescriptor();

    // TODO(@kevindietrich) : what if multiple patches?
    // surface_batch->prim_type = gpu_prim_type_for_desc_type(desc.GetType());

    index_len = array.GetNumPatches() * desc.GetNumControlVertices();

    gl_index_buffer->set_index_len(index_len);

#ifdef DEBUG_SUBDIV_CODE
    DebugInfo::PatchDebugInfo &pdi = debug_info->patches.emplace_back();
    pdi.elements = array.GetNumPatches() * desc.GetNumControlVertices();
    pdi.type = desc.GetType();
    pdi.index_base = array.GetIndexBase();
    pdi.offset = array.GetIndexBase() * sizeof(uint32_t);
#endif
  }

#ifdef DEBUG_SUBDIV_CODE
  debug_info->batch_prim_type = surface_batch->prim_type;
  debug_info->patch_count = patch_table->GetPatchArrays().size();
#endif

  gl_index_buffer->set_ibo(patch_table->GetPatchIndexBuffer());
  gl_index_buffer->set_index_type(blender::gpu::GPU_INDEX_U32);

  compute_tris_and_normals(
      osd_mesh->GetVertexBuffer()->get_vert_buf(), tmp_index_buffer, index_len, index_buf);

  GPU_indexbuf_discard(tmp_index_buffer);
}

struct OsdMeshData {
  OsdMeshType *osd_mesh;
  int level;
};

static std::unordered_map<Mesh *, OsdMeshData> mesh_map;
using MeshMapIterator = std::unordered_map<Mesh *, OsdMeshData>::iterator;

static bool should_rebuild_mesh(Mesh *mesh, int new_levels, int old_levels)
{
  if (new_levels != old_levels) {
    return true;
  }

  return false;
}

static OsdMeshType *get_or_create_osd_mesh(const Scene *scene,
                                           Mesh *mesh DEGBUG_INFO_PARAM(debug_info))
{
  const int level = BKE_mesh_get_subdivision_level(scene, mesh);

  MeshMapIterator mesh_map_iterator = mesh_map.find(mesh);

  if (mesh_map_iterator != mesh_map.end()) {
    OsdMeshData data = mesh_map_iterator->second;

    // TODO(@kevindietrich)
//    if (!should_rebuild_mesh(mesh, level, data.level)) {
//      return data.osd_mesh;
//    }

    /* Different levels, rebuild the mesh.
     * TODO(@kevindietrich): keep track of more options
     */
    delete data.osd_mesh;
  }

#if 0
  OpenSubdiv_TopologyRefinerSettings topology_refiner_settings;
  topology_refiner_settings.level = BKE_mesh_get_subdivision_level(scene, mesh);
  topology_refiner_settings.is_adaptive = mesh->adaptive_subdivision;

  SubdivSettings settings;
  settings.level = BKE_mesh_get_subdivision_level(scene, mesh);
  settings.is_adaptive = mesh->adaptive_subdivision;

  OpenSubdiv_Converter converter;
  BKE_subdiv_converter_init_for_mesh(&converter, &settings, mesh);

  OpenSubdiv_TopologyRefiner *osd_refiner = openSubdiv_createTopologyRefinerFromConverter(&converter, &topology_refiner_settings);

  Far::TopologyRefiner *refiner = osd_refiner->impl->topology_refiner;
#else
  // TODO(@kevindietrich) : options
  Sdc::SchemeType sdctype = Sdc::SCHEME_CATMARK;
  Sdc::Options sdcoptions;

  Far::TopologyRefiner *refiner = Far::TopologyRefinerFactory<Mesh>::Create(
      *mesh, Far::TopologyRefinerFactory<Mesh>::Options(sdctype, sdcoptions));
#endif

  // TODO(@kevindietrich) : bits
  Osd::MeshBitset bits;
  bits.set(Osd::MeshAdaptive, mesh->adaptive_subdivision != 0);
  //	bits.set(Osd::MeshUseSmoothCornerPatch, g_smoothCornerPatch != 0);
  //	bits.set(Osd::MeshUseSingleCreasePatch, g_singleCreasePatch != 0);
  //	bits.set(Osd::MeshUseInfSharpPatch,     g_infSharpPatch != 0);
  //	bits.set(Osd::MeshInterleaveVarying,    g_shadingMode == kShadingInterleavedVaryingColor);
  //	bits.set(Osd::MeshFVarData,             g_shadingMode == kShadingFaceVaryingColor);
  //	bits.set(Osd::MeshEndCapBilinearBasis,  g_endCap == kEndCapBilinearBasis);
  //	bits.set(Osd::MeshEndCapBSplineBasis,   g_endCap == kEndCapBSplineBasis);
  //	bits.set(Osd::MeshEndCapGregoryBasis,   g_endCap == kEndCapGregoryBasis);
  //	bits.set(Osd::MeshEndCapLegacyGregory,  g_endCap == kEndCapLegacyGregory);

  // 001_0111_0000 (matches example app)
  bits.set(Osd::MeshUseSmoothCornerPatch, true);
  bits.set(Osd::MeshUseSingleCreasePatch, true);
  bits.set(Osd::MeshUseInfSharpPatch, true);
  bits.set(Osd::MeshEndCapBSplineBasis, true);

#ifdef DEBUG_SUBDIV_CODE
  debug_info->bits = bits;
  debug_info->level = mesh->subdiv_viewport_level;
#endif

  // TODO(@kevindietrich): create own evaluator cache
  static Osd::EvaluatorCacheT<Osd::GLComputeEvaluator> glComputeEvaluatorCache;

  const int num_vertex_elements = 4;

  // TODO(@kevindietrich): attributes (UVs, vertex colors, etc.)
  const int num_varying_elements = 0;

  OsdMeshType *osd_mesh = new OsdMeshType(
      refiner, num_vertex_elements, num_varying_elements, level, bits, &glComputeEvaluatorCache);

  mesh_map.insert({mesh, {osd_mesh, mesh->subdiv_viewport_level}});

  mesh->runtime.subdivision_mesh = osd_mesh;

  return osd_mesh;
}

void DRW_create_subdivision(const Scene *scene,
                            Mesh *mesh,
                            GPUVertBuf **vert_buf,
                            GPUIndexBuf **index_buf)
{
#ifdef DEBUG_SUBDIV_CODE
  DebugInfo *debug_info = debug_info_cache.info_for_name(mesh->id.name + 2);
  debug_info->clear();
#endif

  OsdMeshType *osd_mesh = get_or_create_osd_mesh(scene, mesh PASS_DEBUG_INFO(debug_info));

  setup_vertex_buffer(*vert_buf, mesh, osd_mesh);

  do_subdivision(osd_mesh);

  setup_index_buffer(*index_buf, osd_mesh PASS_DEBUG_INFO(debug_info));

#ifdef DEBUG_SUBDIV_CODE
  debug_info->print(std::cerr);
#endif
}

void DRW_subdivision_mesh_free(Mesh *mesh)
{
  // TODO(@kevindietrich) : this is called in edit mode as well, and is causing crashes.
  // delete static_cast<OsdMeshType *>(mesh->runtime.subdivision_mesh);
  // mesh->runtime.subdivision_mesh = nullptr;
}

void DRW_subdiv_free(void)
{
  GPU_shader_free(g_quads_to_tris_compute_shader);
}
}
