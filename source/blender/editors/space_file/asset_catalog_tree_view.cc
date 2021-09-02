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

#include "RNA_access.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_types.h"

#include "file_intern.h"

using namespace blender;

static uiBut *add_row_button(uiBlock *block, StringRef name, BIFIconID icon)
{
  return uiDefIconTextBut(block,
                          UI_BTYPE_DATASETROW,
                          0,
                          icon,
                          name.data(),
                          0,
                          0,
                          0,
                          UI_UNIT_Y,
                          nullptr,
                          0,
                          0,
                          0,
                          0,
                          nullptr);
}

void file_draw_asset_catalog_tree_view_in_layout(::AssetLibrary *asset_library_c, uiLayout *layout)
{
  using namespace blender::bke;

  uiBlock *block = uiLayoutGetBlock(layout);

  add_row_button(block, "All", ICON_HOME);

  if (asset_library_c) {
    auto *asset_library = reinterpret_cast<blender::bke::AssetLibrary *>(asset_library_c);
    AssetCatalogTree *catalog_tree = asset_library->catalog_service->get_catalog_tree();

    catalog_tree->foreach_item([&](const AssetCatalogTreeItem &item) {
      uiBut *but = add_row_button(
          block, item.get_name(), item.has_children() ? ICON_TRIA_DOWN : ICON_NONE);
      UI_but_datasetrow_indentation_set(but, item.count_parents());

      PointerRNA *ptr_props = UI_but_extra_operator_icon_add(
          but, "ASSET_OT_catalog_new", WM_OP_EXEC_DEFAULT, ICON_ADD);

      RNA_string_set(ptr_props, "parent_path", item.catalog_path().c_str());

      UI_block_layout_set_current(block, layout);
    });
  }

  add_row_button(block, "Unassigned", ICON_FILE_HIDDEN);
}
