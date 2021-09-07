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

#include "DNA_userdef_types.h"

#include "interface_intern.h"

#include "UI_interface.h"

#include "UI_tree_view.hh"

namespace blender::ui {

/* ---------------------------------------------------------------------- */

/**
 * Add a tree to the container. This is the only place where items should be added, it handles
 * important invariants!
 */
uiAbstractTreeViewItem &uiTreeViewItemContainer::add_tree_item(
    std::unique_ptr<uiAbstractTreeViewItem> item)
{
  children_.append(std::move(item));

  /* The first item that will be added to the root sets this. */
  if (root == nullptr) {
    root = this;
  }

  uiAbstractTreeViewItem &added_item = *children_.last();
  added_item.root = root;
  if (root != this) {
    /* Any item that isn't the root can be assumed to the a #uiAbstractTreeViewItem. Not entirely
     * nice to static_cast this, but well... */
    added_item.parent_ = static_cast<uiAbstractTreeViewItem *>(this);
  }

  return added_item;
}

/* ---------------------------------------------------------------------- */

void uiAbstractTreeView::build_layout_from_tree(const uiTreeViewLayoutBuilder &builder)
{
  uiLayout *prev_layout = builder.current_layout();

  uiLayoutColumn(prev_layout, true);

  build_layout_from_tree_recursive(builder, *this);

  UI_block_layout_set_current(&builder.block(), prev_layout);
}

void uiAbstractTreeView::build_layout_from_tree_recursive(const uiTreeViewLayoutBuilder &builder,
                                                          const uiTreeViewItemContainer &items)
{
  for (const auto &item : items.children_) {
    builder.build_row(*item);
    build_layout_from_tree_recursive(builder, *item);
  }
}

/* ---------------------------------------------------------------------- */

int uiAbstractTreeViewItem::count_parents() const
{
  int i = 0;
  for (uiTreeViewItemContainer *parent = parent_; parent; parent = parent->parent_) {
    i++;
  }
  return i;
}

void uiAbstractTreeViewItem::toggle_collapsed()
{
  if (is_collapsible()) {
    is_open_ = !is_open_;
  }
}

bool uiAbstractTreeViewItem::is_collapsible() const
{
  return children_.is_empty();
}

/* ---------------------------------------------------------------------- */

uiTreeViewLayoutBuilder::uiTreeViewLayoutBuilder(uiBlock &block) : block_(block)
{
}

void uiTreeViewLayoutBuilder::build_row(uiAbstractTreeViewItem &item) const
{
  uiLayout *prev_layout = current_layout();
  uiLayout *row = uiLayoutRow(prev_layout, false);

  item.build_row(*row);

  UI_block_layout_set_current(&block_, prev_layout);
}

uiBlock &uiTreeViewLayoutBuilder::block() const
{
  return block_;
}

uiLayout *uiTreeViewLayoutBuilder::current_layout() const
{
  return block_.curlayout;
}

/* ---------------------------------------------------------------------- */

uiBasicTreeViewItem::uiBasicTreeViewItem(StringRef label_, BIFIconID icon_)
    : label(label_), icon(icon_)
{
}

void uiBasicTreeViewItem::build_row(uiLayout &row)
{
  uiBlock *block = uiLayoutGetBlock(&row);
  tree_row_but_ = (uiButTreeRow *)uiDefIconTextBut(block,
                                                   UI_BTYPE_TREEROW,
                                                   0,
                                                   /* TODO set open/closed icon here? */
                                                   icon,
                                                   label.data(),
                                                   0,
                                                   0,
                                                   UI_UNIT_X,
                                                   UI_UNIT_Y,
                                                   nullptr,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   nullptr);

  UI_but_treerow_indentation_set(&tree_row_but_->but, count_parents());
}

uiBut *uiBasicTreeViewItem::button()
{
  return &tree_row_but_->but;
}

}  // namespace blender::ui
