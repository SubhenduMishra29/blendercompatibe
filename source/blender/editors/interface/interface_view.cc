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
 * \ingroup edinterface
 */

#include <memory>
#include <variant>

#include "BLI_listbase.h"

#include "interface_intern.h"

#include "UI_interface.hh"
#include "UI_tree_view.hh"

using namespace blender;
using namespace blender::ui;

/**
 * Wrapper to store views in a #ListBase.
 */
struct uiViewLink : public Link {
  using TreeViewPtr = std::unique_ptr<uiAbstractTreeView>;

  std::string idname;
  std::variant<TreeViewPtr> view;
};

uiAbstractTreeView &UI_block_add_view(uiBlock *block,
                                      StringRef idname,
                                      std::unique_ptr<uiAbstractTreeView> tree_view)
{
  uiViewLink *view_link = OBJECT_GUARDED_NEW(uiViewLink);
  BLI_addtail(&block->views, view_link);

  view_link->view = std::move(tree_view);
  view_link->idname = idname;

  return *std::get<uiViewLink::TreeViewPtr>(view_link->view);
}

void ui_block_free_views(uiBlock *block)
{
  LISTBASE_FOREACH_MUTABLE (uiViewLink *, link, &block->views) {
    OBJECT_GUARDED_DELETE(link, uiViewLink);
  }
}
