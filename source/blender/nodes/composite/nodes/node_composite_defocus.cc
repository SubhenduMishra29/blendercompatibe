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
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup cmpnodes
 */

#include "node_composite_util.hh"

#include <climits>

/* ************ Defocus Node ****************** */
static bNodeSocketTemplate cmp_node_defocus_in[] = {
    {SOCK_RGBA, N_("Image"), 1.0f, 1.0f, 1.0f, 1.0f},
    {SOCK_FLOAT, N_("Z"), 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, PROP_NONE},
    {-1, ""},
};
static bNodeSocketTemplate cmp_node_defocus_out[] = {
    {SOCK_RGBA, N_("Image")},
    {-1, ""},
};

static void node_composit_init_defocus(bNodeTree *UNUSED(ntree), bNode *node)
{
  /* defocus node */
  NodeDefocus *nbd = (NodeDefocus *)MEM_callocN(sizeof(NodeDefocus), "node defocus data");
  nbd->bktype = 0;
  nbd->rotation = 0.0f;
  nbd->preview = 1;
  nbd->gamco = 0;
  nbd->samples = 16;
  nbd->fstop = 128.0f;
  nbd->maxblur = 16;
  nbd->bthresh = 1.0f;
  nbd->scale = 1.0f;
  nbd->no_zbuf = 1;
  node->storage = nbd;
}

void register_node_type_cmp_defocus(void)
{
  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_DEFOCUS, "Defocus", NODE_CLASS_OP_FILTER, 0);
  node_type_socket_templates(&ntype, cmp_node_defocus_in, cmp_node_defocus_out);
  node_type_init(&ntype, node_composit_init_defocus);
  node_type_storage(&ntype, "NodeDefocus", node_free_standard_storage, node_copy_standard_storage);

  nodeRegisterType(&ntype);
}
