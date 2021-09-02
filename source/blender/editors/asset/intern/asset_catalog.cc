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
 * \ingroup edasset
 */

#include "BKE_asset_catalog.hh"
#include "BKE_asset_library.hh"

#include "ED_asset_catalog.hh"

using namespace blender;
using namespace blender::bke;

AssetCatalog *ED_asset_catalog_add(blender::bke::AssetLibrary *library,
                                   StringRef name,
                                   StringRef parent_path)
{
  std::string fullpath = parent_path.is_empty() ?
                             std::string(name) :
                             std::string(parent_path) + AssetCatalogService::PATH_SEPARATOR + name;
  return library->catalog_service->create_catalog(fullpath);
}
