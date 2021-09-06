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
 * The Original Code is Copyright (C) 2021 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edtransform
 */

#include "MEM_guardedalloc.h"

#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_context.h"
#include "BKE_report.h"

#include "SEQ_iterator.h"
#include "SEQ_relations.h"
#include "SEQ_sequencer.h"
#include "SEQ_time.h"
#include "SEQ_transform.h"
#include "SEQ_utils.h"

#include "UI_view2d.h"

#include "transform.h"
#include "transform_convert.h"

/** Used for sequencer transform. */
typedef struct TransDataSeq {
  struct Sequence *seq;
  float orig_scale_x;
  float orig_scale_y;
  float orig_rotation;
} TransDataSeq;

static TransData *SeqToTransData(
    Sequence *seq, TransData *td, TransData2D *td2d, TransDataSeq *tdseq, int vert_index)
{
  const StripTransform *transform = seq->strip->transform;
  float vertex[2] = {transform->xofs, transform->yofs};

  /* Add control vertex, so rotation and scale can be calculated. */
  if (vert_index == 1) {
    vertex[0] += 1.0f;
  }
  else if (vert_index == 2) {
    vertex[1] += 1.0f;
  }

  td2d->loc[0] = vertex[0];
  td2d->loc[1] = vertex[1];
  td2d->loc2d = NULL;
  td->loc = td2d->loc;
  copy_v3_v3(td->iloc, td->loc);

  td->center[0] = transform->xofs;
  td->center[1] = transform->yofs;

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;
  unit_m3(td->mtx);
  unit_m3(td->smtx);

  tdseq->seq = seq;
  tdseq->orig_scale_x = transform->scale_x;
  tdseq->orig_scale_y = transform->scale_y;
  tdseq->orig_rotation = transform->rotation;

  td->extra = (void *)tdseq;
  td->ext = NULL;
  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  return td;
}

void createTransSeqImageData(TransInfo *t)
{
  Editing *ed = SEQ_editing_get(t->scene);
  ListBase *seqbase = SEQ_active_seqbase_get(ed);
  SeqCollection *strips = Seq_query_rendered_strips(seqbase, t->scene->r.cfra, 0);
  SEQ_filter_selected_strips(strips);

  const int count = SEQ_collection_len(strips);
  if (ed == NULL || count == 0) {
    SEQ_collection_free(strips);
    return;
  }

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tc->data_len = count * 3; /* 3 vertices per sequence are needed. */
  TransData *td = tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransSeq TransData");
  TransData2D *td2d = tc->data_2d = MEM_callocN(tc->data_len * sizeof(TransData2D),
                                                "TransSeq TransData2D");
  TransDataSeq *tdseq = MEM_callocN(tc->data_len * sizeof(TransDataSeq), "TransSeq TransDataSeq");

  Sequence *seq;
  SEQ_ITERATOR_FOREACH (seq, strips) {
    SeqToTransData(seq, td++, td2d++, tdseq++, 0);
    SeqToTransData(seq, td++, td2d++, tdseq++, 1);
    SeqToTransData(seq, td++, td2d++, tdseq++, 2);
  }

  SEQ_collection_free(strips);
}

void recalcData_sequencer_image(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  TransData *td = NULL;
  TransData2D *td2d = NULL;
  int i;

  for (i = 0, td = tc->data, td2d = tc->data_2d; i < tc->data_len; i++, td++, td2d++) {
    /* Origin. */
    float loc[2];
    copy_v2_v2(loc, td2d->loc);
    i++, td++, td2d++;

    /* X and Y helper handle points used to read scale and rotation. */
    float handle_x[2];
    copy_v2_v2(handle_x, td2d->loc);
    sub_v2_v2(handle_x, loc);
    i++, td++, td2d++;
    float handle_y[2];
    copy_v2_v2(handle_y, td2d->loc);
    sub_v2_v2(handle_y, loc);

    TransDataSeq *tdseq = td->extra;
    Sequence *seq = tdseq->seq;
    StripTransform *transform = seq->strip->transform;
    transform->xofs = round_fl_to_int(loc[0]);
    transform->yofs = round_fl_to_int(loc[1]);
    transform->scale_x = tdseq->orig_scale_x * fabs(len_v2(handle_x));
    transform->scale_y = tdseq->orig_scale_y * fabs(len_v2(handle_y));
    /* Scaling can cause negative rotation. */
    if (t->mode == TFM_ROTATION) {
      transform->rotation = tdseq->orig_rotation + angle_signed_v2v2(handle_x, (float[]){1, 0});
      transform->rotation += DEG2RAD(360.0);
      transform->rotation = fmod(transform->rotation, DEG2RAD(360.0));
    }
    SEQ_relations_invalidate_cache_preprocessed(t->scene, seq);
  }
}
