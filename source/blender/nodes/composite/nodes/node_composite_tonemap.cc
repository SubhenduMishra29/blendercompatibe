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

namespace blender::nodes {

static void cmp_node_tonemap_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>("Image");
}

}  // namespace blender::nodes

static void node_composit_init_tonemap(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeTonemap *ntm = (NodeTonemap *)MEM_callocN(sizeof(NodeTonemap), "node tonemap data");
  ntm->type = 1;
  ntm->key = 0.18;
  ntm->offset = 1;
  ntm->gamma = 1;
  ntm->f = 0;
  ntm->m = 0; /* Actual value is set according to input. */
  /* Default a of 1 works well with natural HDR images, but not always so for CGI.
   * Maybe should use 0 or at least lower initial value instead. */
  ntm->a = 1;
  ntm->c = 0;
  node->storage = ntm;
}

void register_node_type_cmp_tonemap(void)
{
  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_TONEMAP, "Tonemap", NODE_CLASS_OP_COLOR, 0);
  ntype.declare = blender::nodes::cmp_node_tonemap_declare;
  node_type_init(&ntype, node_composit_init_tonemap);
  node_type_storage(&ntype, "NodeTonemap", node_free_standard_storage, node_copy_standard_storage);

  nodeRegisterType(&ntype);
}
