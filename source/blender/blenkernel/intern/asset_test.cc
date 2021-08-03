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
 * The Original Code is Copyright (C) 2020 Blender Foundation
 * All rights reserved.
 */

#include "BKE_asset.h"

#include "DNA_asset_types.h"

#include "testing/testing.h"

namespace blender::bke::tests {

TEST(AssetMetadataTest, set_catalog_id)
{
  AssetMetaData meta;

  /* Test trivial values. */
  BKE_asset_metadata_catalog_id_set(&meta, "");
  EXPECT_STREQ("", meta.catalog_id);
  BKE_asset_metadata_catalog_id_set(&meta, "simple");
  EXPECT_STREQ("simple", meta.catalog_id);

  /* Test whitespace trimming & replacement. */
  BKE_asset_metadata_catalog_id_set(&meta, " Govoriš angleško?    ");
  EXPECT_STREQ("Govoriš-angleško?", meta.catalog_id);

  /* Test length trimming to 63 chars + terminating zero. */
  constexpr char len66[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  constexpr char len63[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1";
  BKE_asset_metadata_catalog_id_set(&meta, len66);
  EXPECT_STREQ(len63, meta.catalog_id);

  /* Test length trimming happens after whitespace trimming. */
  constexpr char len68[] = "  000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  BKE_asset_metadata_catalog_id_set(&meta, len68);
  EXPECT_STREQ(len63, meta.catalog_id);

  /* Test length trimming to 63 bytes, and not 63 characters. ✓ in UTF-8 is three bytes long. */
  constexpr char with_utf8[] =
      "00010203040506✓0708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  BKE_asset_metadata_catalog_id_set(&meta, with_utf8);
  EXPECT_STREQ("00010203040506✓0708090a0b0c0d0e0f101112131415161718191a1b1c1d", meta.catalog_id);
}

}  // namespace blender::bke::tests
