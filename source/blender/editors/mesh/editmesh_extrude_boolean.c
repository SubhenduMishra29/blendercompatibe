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
 */

/** \file
 * \ingroup edmesh
 */

#include "MEM_guardedalloc.h"

#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_report.h"

#include "ED_mesh.h"
#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_view3d.h"

#include "GPU_batch.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_resources.h"

#include "WM_api.h"

#include "mesh_intern.h" /* own include */

#include "tools/bmesh_boolean.h"
#include "tools/bmesh_intersect.h"

/* -------------------------------------------------------------------- */
/** \name Mesh Pre-Select Element Gizmo
 * \{ */

typedef struct ExtrudeBooleanData {
  BMesh *bm;
  /* Start verts and end verts. */
  BMVert **moving_verts;
  int moving_verts_len;
  float normal[3];

  BMLoop *(*looptris)[3];
  int looptris_len;

  struct {
    float center[3];
    float normal[3];

    float dist_initial;
    float dist_curr;
  } interaction_data;

  struct {
    ARegion *region;
    float color[4];
    void *draw_handle;

    GPUBatch *batch_faces;
    GPUBatch *batch_edges;
    GPUVertBuf *vbo;
    GPUIndexBuf *ibo_faces;
    GPUIndexBuf *ibo_edges;
    float (*v_co)[3];
  } draw_data;

  struct {
    View3D *v3d;
    char gizmo_flag_old;
  } exit_data;
} ExtrudeBooleanData;

static void extrude_boolean_drawdata_clear(ExtrudeBooleanData *extrudata)
{
  GPU_batch_discard(extrudata->draw_data.batch_faces);
  GPU_batch_discard(extrudata->draw_data.batch_edges);
  GPU_vertbuf_discard(extrudata->draw_data.vbo);
  GPU_indexbuf_discard(extrudata->draw_data.ibo_faces);
  GPU_indexbuf_discard(extrudata->draw_data.ibo_edges);
}

static void extrude_boolean_drawdata_create(ExtrudeBooleanData *extrudata)
{
  static GPUVertFormat v_format = {0};
  static GPUVertFormat line_format = {0};
  if (v_format.attr_len == 0) {
    GPU_vertformat_attr_add(&v_format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    GPU_vertformat_attr_add(&line_format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  }

  BMesh *bm = extrudata->bm;
  BMIter iter;
  BMVert *v;
  BMEdge *e;
  BMFace *f;
  int vert_len = 2 * extrudata->moving_verts_len;
  int i;

  GPUVertBuf *vbo = GPU_vertbuf_create_with_format_ex(&v_format, GPU_USAGE_DYNAMIC);
  float(*v_co)[3] = NULL;
  {
    GPU_vertbuf_data_alloc(vbo, vert_len);
    v_co = GPU_vertbuf_get_data(vbo);

    for (i = 0; i < vert_len; i++) {
      v = extrudata->moving_verts[i];
      copy_v3_v3(v_co[i], v->co);
      BM_elem_index_set(v, i);
    }

    bm->elem_index_dirty |= BM_VERT;
  }

  GPUIndexBuf *ibo_faces;
  {
    BMLoop *(*looptris)[3] = extrudata->looptris;
    int looptris_draw_len = 0;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
        looptris_draw_len += f->len - 2;
      }
    }

    GPUIndexBufBuilder builder;
    GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, looptris_draw_len, vert_len);
    int loop_first = 0;
    BM_ITER_MESH_INDEX (f, &iter, bm, BM_FACES_OF_MESH, i) {
      if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
        int ltri_index = poly_to_tri_count(i, loop_first);
        int tri_len = f->len - 2;
        while (tri_len--) {
          BMLoop **ltri = looptris[ltri_index++];
          GPU_indexbuf_add_tri_verts(&builder,
                                     BM_elem_index_get(ltri[0]->v),
                                     BM_elem_index_get(ltri[1]->v),
                                     BM_elem_index_get(ltri[2]->v));
        }
      }
      loop_first += f->len;
    }

    ibo_faces = GPU_indexbuf_build(&builder);
  }

  GPUIndexBuf *ibo_edges;
  {
    int edges_draw_len = 0;
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e->v1, BM_ELEM_TAG) && BM_elem_flag_test(e->v2, BM_ELEM_TAG)) {
        BM_elem_flag_enable(e, BM_ELEM_TAG);
        edges_draw_len++;
      }
      else {
        BM_elem_flag_disable(e, BM_ELEM_TAG);
      }
    }

    GPUIndexBufBuilder builder;
    GPU_indexbuf_init(&builder, GPU_PRIM_LINES, edges_draw_len, vert_len);
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e, BM_ELEM_TAG)) {
        GPU_indexbuf_add_line_verts(&builder, BM_elem_index_get(e->v1), BM_elem_index_get(e->v2));
      }
    }

    ibo_edges = GPU_indexbuf_build(&builder);
  }

  GPUBatch *batch_faces = GPU_batch_create(GPU_PRIM_TRIS, vbo, ibo_faces);
  GPU_batch_program_set_builtin(batch_faces, GPU_SHADER_3D_UNIFORM_COLOR);

  GPUBatch *batch_edges = GPU_batch_create(GPU_PRIM_LINES, vbo, ibo_edges);
  GPU_batch_program_set_builtin(batch_edges, GPU_SHADER_3D_UNIFORM_COLOR);

  extrudata->draw_data.batch_faces = batch_faces;
  extrudata->draw_data.batch_edges = batch_edges;
  extrudata->draw_data.vbo = vbo;
  extrudata->draw_data.ibo_faces = ibo_faces;
  extrudata->draw_data.ibo_edges = ibo_edges;
  extrudata->draw_data.v_co = v_co;
}

static void extrude_boolean_data_exit(ExtrudeBooleanData *extrudata)
{
  if (extrudata->draw_data.batch_faces) {
    extrude_boolean_drawdata_clear(extrudata);
  }

  if (extrudata->bm) {
    BM_mesh_free(extrudata->bm);
  }

  if (extrudata->looptris) {
    MEM_freeN(extrudata->looptris);
  }

  ED_region_draw_cb_exit(extrudata->draw_data.region->type, extrudata->draw_data.draw_handle);
  extrudata->exit_data.v3d->gizmo_flag = extrudata->exit_data.gizmo_flag_old;
  MEM_freeN(extrudata->moving_verts);
  MEM_freeN(extrudata);
}

static void extrude_boolean_draw_fn(const bContext *C, ARegion *region, void *data)
{
  ExtrudeBooleanData *extrudata = data;
  Object *obedit = CTX_data_edit_object(C);
  if (!obedit || obedit->type != OB_MESH) {
    return;
  }

  const RegionView3D *rv3d = region->regiondata;

  GPU_matrix_push();
  GPU_matrix_mul(obedit->obmat);

  if (GPU_vertbuf_get_status(extrudata->draw_data.vbo) & GPU_VERTBUF_DATA_DIRTY) {
    GPU_vertbuf_use(extrudata->draw_data.vbo);
  }

  ED_view3d_polygon_offset(rv3d, 1.0f);
  GPU_depth_mask(false);

  GPUShader *sh = extrudata->draw_data.batch_faces->shader;
  GPU_shader_bind(sh);

  GPU_shader_uniform_4fv(sh, "color", (float[4]){0.75f, 0.75f, 0.75f, 1.0f});
  GPU_batch_draw(extrudata->draw_data.batch_edges);

  GPU_shader_uniform_4fv(sh, "color", extrudata->draw_data.color);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);

  GPU_face_culling(GPU_CULL_BACK);
  GPU_batch_draw(extrudata->draw_data.batch_faces);

  GPU_face_culling(GPU_CULL_FRONT);
  GPU_batch_draw(extrudata->draw_data.batch_faces);

  ED_view3d_polygon_offset(rv3d, 0.0f);

  GPU_depth_mask(true);
  GPU_matrix_pop();
}

static float extrude_boolean_interaction_dist(ExtrudeBooleanData *extrudata, const int mval[2])
{
  float ray_start[3], ray_dir[3];
  ED_view3d_win_to_ray(extrudata->draw_data.region, (float[2]){UNPACK2(mval)}, ray_start, ray_dir);

  float dist;
  isect_ray_ray_v3(extrudata->interaction_data.center,
                   extrudata->interaction_data.normal,
                   ray_start,
                   ray_dir,
                   &dist,
                   NULL);

  return dist;
}

static ExtrudeBooleanData *extrude_boolean_data_create(bContext *C, const int mval[2])
{
  BMOperator bmop;
  BMOIter siter;
  BMIter iter;
  BMFace *f;
  BMVert *v;

  Object *obedit = CTX_data_edit_object(C);
  if (!obedit || obedit->type != OB_MESH) {
    return NULL;
  }

  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  if (!em->bm->totfacesel) {
    return NULL;
  }

  ExtrudeBooleanData *extrudata = MEM_callocN(sizeof(*extrudata), __func__);
  extrudata->bm = BM_mesh_copy(em->bm);
  int totvert = 0;
  float normal[3], center[3];
  zero_v3(normal);
  zero_v3(center);

  BM_mesh_elem_hflag_disable_all(extrudata->bm, BM_VERT | BM_FACE, BM_ELEM_TAG, false);
  BM_ITER_MESH (f, &iter, extrudata->bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
      BM_elem_flag_enable(f, BM_ELEM_TAG);
      add_v3_v3(normal, f->no);
      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        v = l_iter->v;
        if (!BM_elem_flag_test(v, BM_ELEM_TAG)) {
          BM_elem_flag_enable(v, BM_ELEM_TAG);
          add_v3_v3(center, v->co);
          totvert++;
        }
      } while ((l_iter = l_iter->next) != l_first);
    }
  }

  copy_v3_v3(extrudata->normal, normal);

  mul_v3_fl(center, 1.0f / totvert);
  add_v3_v3(normal, center);
  mul_m4_v3(obedit->obmat, normal);
  mul_m4_v3(obedit->obmat, center);
  sub_v3_v3(normal, center);

  BMVert **moving_verts = MEM_mallocN(sizeof(*moving_verts) * 2 * totvert, __func__);
  extrudata->moving_verts = moving_verts;
  extrudata->moving_verts_len = totvert;

  BMO_op_initf(extrudata->bm, &bmop, 0, "duplicate geom=%hf", BM_ELEM_TAG);
  BMO_op_exec(extrudata->bm, &bmop);
  BMO_ITER (f, &siter, bmop.slots_out, "geom_orig.out", BM_FACE) {
    BM_elem_flag_disable(f, BM_ELEM_TAG);
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_elem_flag_disable(l_iter->v, BM_ELEM_TAG);
    } while ((l_iter = l_iter->next) != l_first);
  }

  totvert = 0;
  BMO_ITER (v, &siter, bmop.slots_out, "geom.out", BM_VERT) {
    moving_verts[totvert++] = v;
  }
  BMO_op_finish(extrudata->bm, &bmop);

  BMO_op_initf(extrudata->bm, &bmop, 0, "extrude_face_region geom=%hf", BM_ELEM_TAG);
  BMO_op_exec(extrudata->bm, &bmop);
  BMO_ITER (v, &siter, bmop.slots_out, "geom.out", BM_VERT) {
    moving_verts[totvert++] = v;
  }
  BMO_op_finish(extrudata->bm, &bmop);

  if (!normalize_v3(extrudata->normal)) {
    extrudata->normal[2] = 1.0f;
  }
  if (!normalize_v3(normal)) {
    normal[2] = 1.0f;
  }

  copy_v3_v3(extrudata->interaction_data.center, center);
  copy_v3_v3(extrudata->interaction_data.normal, normal);

  {
    int looptris_len = poly_to_tri_count(extrudata->bm->totface, extrudata->bm->totloop);
    BMLoop *(*looptris)[3] = MEM_mallocN(sizeof(*looptris) * looptris_len, __func__);
    BM_mesh_calc_tessellation_ex(extrudata->bm,
                                 looptris,
                                 &(const struct BMeshCalcTessellation_Params){
                                     .face_normals = false,
                                 });

    extrudata->looptris_len = looptris_len;
    extrudata->looptris = looptris;
  }

  ARegion *region = CTX_wm_region(C);
  extrudata->draw_data.region = region;
  extrudata->draw_data.draw_handle = ED_region_draw_cb_activate(
      region->type, extrude_boolean_draw_fn, extrudata, REGION_DRAW_POST_VIEW);

  UI_GetThemeColor4fv(TH_GIZMO_PRIMARY, extrudata->draw_data.color);
  extrudata->draw_data.color[3] = 0.25f;

  View3D *v3d = CTX_wm_view3d(C);
  extrudata->exit_data.v3d = v3d;
  extrudata->exit_data.gizmo_flag_old = v3d->gizmo_flag;
  v3d->gizmo_flag = V3D_GIZMO_HIDE;

  if (mval) {
    float dist;
    dist = extrude_boolean_interaction_dist(extrudata, mval);
    extrudata->interaction_data.dist_initial = dist;
  }

  return extrudata;
}

static void vbo_tag_dirty(GPUVertBuf *vbo)
{
  /* Workarround to tag dirty. */
  GPU_vertbuf_init_with_format_ex(vbo, GPU_vertbuf_get_format(vbo), GPU_USAGE_DYNAMIC);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Ruler Operator
 * \{ */

static void mesh_extrude_drawdata_update(ExtrudeBooleanData *extrudata, float distance)
{
  BMVert **v_ptr;
  float(*v_co)[3];
  if (distance < 0.0f) {
    v_ptr = &extrudata->moving_verts[0];
    v_co = &extrudata->draw_data.v_co[0];
  }
  else {
    v_ptr = &extrudata->moving_verts[extrudata->moving_verts_len];
    v_co = &extrudata->draw_data.v_co[extrudata->moving_verts_len];
  }

  float offset[3];
  mul_v3_v3fl(offset, extrudata->normal, distance);
  for (int i = 0; i < extrudata->moving_verts_len; i++, v_ptr++) {
    add_v3_v3v3(v_co[i], (*v_ptr)->co, offset);
  }

  vbo_tag_dirty(extrudata->draw_data.vbo);
}

/**
 * Compare selected/unselected.
 */
static int bm_face_isect_pair(BMFace *f, void *UNUSED(user_data))
{
  if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
    return -1;
  }
#ifdef WITH_GMP
  if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
#else
  if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
#endif
    return 1;
  }
  return 0;
}

static int mesh_extrude_boolean_exec(bContext *C, wmOperator *op)
{
  ExtrudeBooleanData *extrudata = op->customdata;
  float distance_start, distance_end;
  if (!extrudata) {
    extrudata = extrude_boolean_data_create(C, NULL);
    if (!extrudata) {
      return OPERATOR_CANCELLED;
    }
    distance_start = RNA_float_get(op->ptr, "distance_start");
    distance_end = RNA_float_get(op->ptr, "distance_end");
  }
  else {
    if (extrudata->interaction_data.dist_curr > 0.0f) {
      distance_start = 0.0f;
      distance_end = extrudata->interaction_data.dist_curr;
    }
    else {
      distance_start = extrudata->interaction_data.dist_curr;
      distance_end = 0.0f;
    }
    RNA_float_set(op->ptr, "distance_start", distance_start);
    RNA_float_set(op->ptr, "distance_end", distance_end);
  }

  int operation = (fabsf(distance_start) < fabsf(distance_end)) ? BMESH_ISECT_BOOLEAN_UNION :
                                                                  BMESH_ISECT_BOOLEAN_DIFFERENCE;

  float offset[3];
  if (distance_start) {
    mul_v3_v3fl(offset, extrudata->normal, distance_start);
    BMVert **v_ptr = extrudata->moving_verts;
    for (int i = 0; i < extrudata->moving_verts_len; i++, v_ptr++) {
      add_v3_v3((*v_ptr)->co, offset);
    }
  }

  if (distance_end) {
    mul_v3_v3fl(offset, extrudata->normal, distance_end);
    BMVert **v_ptr = &extrudata->moving_verts[extrudata->moving_verts_len];
    for (int i = 0; i < extrudata->moving_verts_len; i++, v_ptr++) {
      add_v3_v3((*v_ptr)->co, offset);
    }
  }

  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);

  BM_mesh_free(em->bm);
  MEM_SAFE_FREE(em->looptris);

  em->bm = extrudata->bm;
  em->looptris = extrudata->looptris;
  em->tottri = extrudata->looptris_len;

  extrudata->bm = NULL;
  extrudata->looptris = NULL;

  BKE_editmesh_looptri_and_normals_calc(em);
  bool has_isect;
#ifdef WITH_GMP
  has_isect = BM_mesh_boolean(
      em->bm, em->looptris, em->tottri, bm_face_isect_pair, NULL, 2, true, true, false, operation);
#else
  BMIter iter;
  BMFace *f;
  BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
      BM_face_select_set(em->bm, f, true);
    }
  }
  has_isect = BM_mesh_intersect(em->bm,
                                em->looptris,
                                em->tottri,
                                bm_face_isect_pair,
                                NULL,
                                false,
                                false,
                                true,
                                true,
                                false,
                                true,
                                operation,
                                0.000001f);
#endif

  if (!has_isect) {
    BKE_report(op->reports, RPT_WARNING, "No intersections found");
  }

  extrude_boolean_data_exit(extrudata);

  BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  EDBM_update(obedit->data,
              &(const struct EDBMUpdate_Params){
                  .calc_looptri = true,
                  .calc_normals = true,
                  .is_destructive = true,
              });

  return OPERATOR_FINISHED;
}

static int mesh_extrude_boolean_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ExtrudeBooleanData *extrudata = op->customdata;
  if (ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE)) {
    extrude_boolean_data_exit(extrudata);
    return OPERATOR_CANCELLED;
  }

  if (event->type == LEFTMOUSE) {
    return mesh_extrude_boolean_exec(C, op);
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  float dist = extrude_boolean_interaction_dist(extrudata, event->mval);
  dist -= extrudata->interaction_data.dist_initial;

  extrudata->interaction_data.dist_curr = dist;
  mesh_extrude_drawdata_update(extrudata, dist);

  ARegion *region = extrudata->draw_data.region;
  ED_region_tag_redraw(region);

  return OPERATOR_RUNNING_MODAL;
}

static int mesh_extrude_boolean_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ExtrudeBooleanData *extrudata = extrude_boolean_data_create(C, event->mval);
  if (!extrudata) {
    return OPERATOR_CANCELLED;
  }

  op->customdata = extrudata;

  extrude_boolean_drawdata_create(extrudata);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void MESH_OT_extrude_boolean(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Mesh Extrude Boolean";
  ot->idname = "MESH_OT_extrude_boolean";
  ot->description = "Extrude and Boolean";

  ot->poll = ED_operator_editmesh_view3d;
  ot->invoke = mesh_extrude_boolean_invoke;
  ot->exec = mesh_extrude_boolean_exec;
  ot->modal = mesh_extrude_boolean_modal;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  RNA_def_float_distance(ot->srna,
                         "distance_start",
                         0.0f,
                         -FLT_MAX,
                         FLT_MAX,
                         "Distance Start",
                         "",
                         -FLT_MAX,
                         FLT_MAX);
  RNA_def_float_distance(
      ot->srna, "distance_end", 1.0f, -FLT_MAX, FLT_MAX, "End", "", -FLT_MAX, FLT_MAX);
}

/** \} */
