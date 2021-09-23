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
 * The Original Code is Copyright (C) 2007 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup spfile
 */

#include "ED_fileselect.h"

#include "DNA_space_types.h"

#include "BKE_asset_catalog.hh"
#include "BKE_asset_library.hh"

#include "BLI_string_ref.hh"

#include "BLT_translation.h"

#include "RNA_access.h"

#include "UI_interface.h"
#include "UI_interface.hh"
#include "UI_resources.h"
#include "UI_tree_view.hh"

#include "WM_api.h"
#include "WM_types.h"

#include "file_intern.h"

using namespace blender;
using namespace blender::ui;
using namespace blender::bke;

class AssetCatalogTreeView : public uiAbstractTreeView {
  bke::AssetLibrary *library_;
  FileAssetSelectParams *params_;

  friend class AssetCatalogTreeViewItem;

 public:
  AssetCatalogTreeView(bke::AssetLibrary *library, FileAssetSelectParams *params);

  void build_tree() override;

 private:
  static uiBasicTreeViewItem &build_recursive(uiTreeViewItemContainer &view_parent_item,
                                              AssetCatalogTreeItem &catalog);
};
/* ---------------------------------------------------------------------- */

class AssetCatalogTreeViewItem : public uiBasicTreeViewItem {
  AssetCatalogTreeItem &catalog_;

 public:
  AssetCatalogTreeViewItem(AssetCatalogTreeItem &catalog)
      : uiBasicTreeViewItem(catalog.get_name()), catalog_(catalog)
  {
  }

  void onActivate() override
  {
    const AssetCatalogTreeView &tree_view = static_cast<const AssetCatalogTreeView &>(
        get_tree_view());
    tree_view.params_->asset_catalog_visibility = FILE_SHOW_ASSETS_FROM_CATALOG;
    tree_view.params_->catalog_id = catalog_.get_catalog_id();
    WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, NULL);
  }

  void build_row(uiLayout &row) override
  {
    uiBasicTreeViewItem::build_row(row);

    if (!is_active()) {
      return;
    }

    PointerRNA *props;
    char catalog_id_str_buffer[UUID_STRING_LEN] = "";
    CatalogID catalog_id = catalog_.get_catalog_id();

    BLI_uuid_format(catalog_id_str_buffer, catalog_id);

    props = UI_but_extra_operator_icon_add(
        button(), "ASSET_OT_catalog_new", WM_OP_INVOKE_DEFAULT, ICON_ADD);
    RNA_string_set(props, "parent_path", catalog_.catalog_path().c_str());
    /* Can't actually delete items without catalog ID. */
    if (!BLI_uuid_is_nil(catalog_id)) {
      props = UI_but_extra_operator_icon_add(
          button(), "ASSET_OT_catalog_delete", WM_OP_INVOKE_DEFAULT, ICON_X);
      RNA_string_set(props, "catalog_id", catalog_id_str_buffer);
    }
  }
};

AssetCatalogTreeView::AssetCatalogTreeView(bke::AssetLibrary *library,
                                           FileAssetSelectParams *params)
    : library_(library), params_(params)
{
}

void AssetCatalogTreeView::build_tree()
{
  FileAssetSelectParams *params = params_;

  add_tree_item<uiBasicTreeViewItem>(IFACE_("All"), ICON_HOME, [params](uiBasicTreeViewItem &) {
    params->asset_catalog_visibility = FILE_SHOW_ASSETS_ALL_CATALOGS;
    WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, NULL);
  });

  if (library_) {
    AssetCatalogTree *catalog_tree = library_->catalog_service->get_catalog_tree();

    catalog_tree->foreach_child([this](AssetCatalogTreeItem &item) {
      uiBasicTreeViewItem &child_view_item = build_recursive(*this, item);

      /* Open root-level items by default. */
      child_view_item.set_collapsed(false);
    });
  }

  add_tree_item<uiBasicTreeViewItem>(
      IFACE_("Unassigned"), ICON_FILE_HIDDEN, [params](uiBasicTreeViewItem &) {
        params->asset_catalog_visibility = FILE_SHOW_ASSETS_WITHOUT_CATALOG;
        WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, NULL);
      });
}

uiBasicTreeViewItem &AssetCatalogTreeView::build_recursive(
    uiTreeViewItemContainer &view_parent_item, AssetCatalogTreeItem &catalog)
{
  uiBasicTreeViewItem &view_item = view_parent_item.add_tree_item<AssetCatalogTreeViewItem>(
      catalog);

  catalog.foreach_child(
      [&view_item](AssetCatalogTreeItem &child) { build_recursive(view_item, child); });
  return view_item;
}

/* ---------------------------------------------------------------------- */

void file_create_asset_catalog_tree_view_in_layout(::AssetLibrary *asset_library_c,
                                                   uiLayout *layout,
                                                   FileAssetSelectParams *params)
{
  uiBlock *block = uiLayoutGetBlock(layout);

  bke::AssetLibrary *asset_library = reinterpret_cast<blender::bke::AssetLibrary *>(
      asset_library_c);

  uiAbstractTreeView *tree_view = UI_block_add_view(
      block,
      "asset catalog tree view",
      std::make_unique<AssetCatalogTreeView>(asset_library, params));

  uiTreeViewBuilder builder(*block);
  builder.build_tree_view(*tree_view);
}
