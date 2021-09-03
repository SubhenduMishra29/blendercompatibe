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
 * \ingroup editorui
 */

#pragma once

#include <memory>
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_vector.hh"

#include "UI_resources.h"

struct PointerRNA;
struct uiBlock;
struct uiBut;
struct uiButTreeRow;
struct uiLayout;

namespace blender::ui {

class uiAbstractTreeViewItem;

/* ---------------------------------------------------------------------- */
/** \name Tree-View Item Container
 * \{ */

/**
 * Helper base class to expose common child-item data and functionality to both #uiAbstractTreeView
 * and #uiAbstractTreeViewItem.
 *
 * That means this type can be used whenever either a #uiAbstractTreeView or a
 * #uiAbstractTreeViewItem is needed.
 */
class uiTreeViewItemContainer {
  friend class uiAbstractTreeView;
  friend class uiAbstractTreeViewItem;

  /* Private constructor, so only the friends above can create this! */
  uiTreeViewItemContainer() = default;

 protected:
  Vector<std::unique_ptr<uiAbstractTreeViewItem>> children_;
  /** Adding the first item to the root will set this, then it's passed on to all children. */
  uiTreeViewItemContainer *root = nullptr;
  /** Pointer back to the owning item. */
  uiAbstractTreeViewItem *parent_ = nullptr;

 public:
  /**
   * Convenience wrapper taking the arguments needed to construct an item of type \a ItemT. Calls
   * the version just below.
   */
  template<class ItemT, typename... Args> ItemT &add_tree_item(Args &&...args)
  {
    static_assert(std::is_base_of<uiAbstractTreeViewItem, ItemT>::value,
                  "Type must derive from and implement the uiAbstractTreeViewItem interface");

    return dynamic_cast<ItemT &>(
        add_tree_item(std::make_unique<ItemT>(std::forward<Args>(args)...)));
  }

  uiAbstractTreeViewItem &add_tree_item(std::unique_ptr<uiAbstractTreeViewItem> item);
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Tree-View Base Class
 * \{ */

class uiTreeViewLayoutBuilder {
  uiBlock &block_;

 public:
  uiTreeViewLayoutBuilder(uiBlock &);

  void build_row(uiAbstractTreeViewItem &item) const;
};

class uiAbstractTreeView : public uiTreeViewItemContainer {
 public:
  virtual ~uiAbstractTreeView() = default;

  virtual void build_tree() = 0;

  void build_layout_from_tree(const uiTreeViewLayoutBuilder &builder);

 private:
  void build_layout_from_tree_recursive(const uiTreeViewLayoutBuilder &builder,
                                        const uiTreeViewItemContainer &items);
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Tree-View Item Type
 * \{ */

/* * \brief Abstract base class for defining a customizable tree-view item.
 *
 * The tree-view item defines how to build its data into a tree-row. There are implementations for
 * common layouts, e.g. #uiBasicTreeViewItem.
 * It also stores state information that needs to be persistent over redraws, like the collapsed
 * state.
 */
class uiAbstractTreeViewItem : public uiTreeViewItemContainer {
  friend class uiAbstractTreeView;

  bool is_open_;

 public:
  virtual ~uiAbstractTreeViewItem() = default;

  virtual void build_row(uiLayout &row) = 0;

  int count_parents() const;
  void toggle_collapsed();
  bool is_collapsible() const;
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Predefined Tree-View Item Types
 *
 *  Common, Basic Tree-View Item Types.
 * \{ */

/**
 * The most basic type, just a label with an icon.
 */
class uiBasicTreeViewItem : public uiAbstractTreeViewItem {
 public:
  std::string label;
  BIFIconID icon;

  uiBasicTreeViewItem(StringRef label, BIFIconID icon = ICON_NONE);

  void build_row(uiLayout &row) override;

 protected:
  /** Created in the #build() function. */
  uiButTreeRow *tree_row_but_ = nullptr;
  uiBut *button();
};

/** \} */

}  // namespace blender::ui
