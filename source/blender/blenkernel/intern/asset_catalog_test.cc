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

#include "BKE_appdir.h"
#include "BKE_asset_catalog.hh"

#include "BLI_fileops.h"

#include "testing/testing.h"

#include <filesystem>

namespace fs = blender::filesystem;

namespace blender::bke::tests {

/* UUIDs from lib/tests/asset_library/blender_assets.cats.txt */
const UUID UUID_ID_WITHOUT_PATH("e34dd2c5-5d2e-4668-9794-1db5de2a4f71");
const UUID UUID_POSES_ELLIE("df60e1f6-2259-475b-93d9-69a1b4a8db78");
const UUID UUID_POSES_ELLIE_WHITESPACE("b06132f6-5687-4751-a6dd-392740eb3c46");
const UUID UUID_POSES_ELLIE_TRAILING_SLASH("3376b94b-a28d-4d05-86c1-bf30b937130d");
const UUID UUID_POSES_RUZENA("79a4f887-ab60-4bd4-94da-d572e27d6aed");
const UUID UUID_POSES_RUZENA_HAND("81811c31-1a88-4bd7-bb34-c6fc2607a12e");
const UUID UUID_POSES_RUZENA_FACE("82162c1f-06cc-4d91-a9bf-4f72c104e348");
const UUID UUID_WITHOUT_SIMPLENAME("d7916a31-6ca9-4909-955f-182ca2b81fa3");

/* Subclass that adds accessors such that protected fields can be used in tests. */
class TestableAssetCatalogService : public AssetCatalogService {
 public:
  explicit TestableAssetCatalogService(const CatalogFilePath &asset_library_root)
      : AssetCatalogService(asset_library_root)
  {
  }

  AssetCatalogDefinitionFile *get_catalog_definition_file()
  {
    return catalog_definition_file_.get();
  }
};

class AssetCatalogTest : public testing::Test {
 protected:
  CatalogFilePath asset_library_root_;
  CatalogFilePath temp_library_path_;

  void SetUp() override
  {
    const fs::path test_files_dir = blender::tests::flags_test_asset_dir();
    if (test_files_dir.empty()) {
      FAIL();
    }

    asset_library_root_ = test_files_dir / "asset_library";
    temp_library_path_ = "";
  }

  /* Register a temporary path, which will be removed at the end of the test. */
  CatalogFilePath use_temp_path()
  {
    const CatalogFilePath tempdir = BKE_tempdir_session();
    temp_library_path_ = tempdir / "test-temporary-path";
    return temp_library_path_;
  }

  void TearDown() override
  {
    if (!temp_library_path_.empty()) {
      fs::remove_all(temp_library_path_);
      temp_library_path_ = "";
    }
  }
};

TEST_F(AssetCatalogTest, load_single_file)
{
  AssetCatalogService service(asset_library_root_);
  service.load_from_disk(asset_library_root_ / "blender_assets.cats.txt");

  // Test getting a non-existant catalog ID.
  EXPECT_EQ(nullptr, service.find_catalog(BLI_uuid_generate_random()));

  // Test getting an invalid catalog (without path definition).
  AssetCatalog *cat_without_path = service.find_catalog(UUID_ID_WITHOUT_PATH);
  ASSERT_EQ(nullptr, cat_without_path);

  // Test getting a regular catalog.
  AssetCatalog *poses_ellie = service.find_catalog(UUID_POSES_ELLIE);
  ASSERT_NE(nullptr, poses_ellie);
  EXPECT_EQ(UUID_POSES_ELLIE, poses_ellie->catalog_id);
  EXPECT_EQ("character/Ellie/poselib", poses_ellie->path);
  EXPECT_EQ("POSES_ELLIE", poses_ellie->simple_name);

  // Test whitespace stripping and support in the path.
  AssetCatalog *poses_whitespace = service.find_catalog(UUID_POSES_ELLIE_WHITESPACE);
  ASSERT_NE(nullptr, poses_whitespace);
  EXPECT_EQ(UUID_POSES_ELLIE_WHITESPACE, poses_whitespace->catalog_id);
  EXPECT_EQ("character/Ellie/poselib/white space", poses_whitespace->path);
  EXPECT_EQ("POSES_ELLIE WHITESPACE", poses_whitespace->simple_name);

  // Test getting a UTF-8 catalog ID.
  AssetCatalog *poses_ruzena = service.find_catalog(UUID_POSES_RUZENA);
  ASSERT_NE(nullptr, poses_ruzena);
  EXPECT_EQ(UUID_POSES_RUZENA, poses_ruzena->catalog_id);
  EXPECT_EQ("character/Ružena/poselib", poses_ruzena->path);
  EXPECT_EQ("POSES_RUŽENA", poses_ruzena->simple_name);
}

static int count_path_parents(const fs::path &path)
{
  int counter = 0;
  for (const fs::path &segment : path.parent_path()) {
    counter++;
    UNUSED_VARS(segment);
  }
  return counter;
}

TEST_F(AssetCatalogTest, load_single_file_into_tree)
{
  AssetCatalogService service(asset_library_root_);
  service.load_from_disk(asset_library_root_ / "blender_assets.cats.txt");

  /* Contains not only paths from the CDF but also the missing parents (implicitly defined
   * catalogs). */
  std::vector<fs::path> expected_paths{
      "character",
      "character/Ellie",
      "character/Ellie/poselib",
      "character/Ellie/poselib/white space",
      "character/Ružena",
      "character/Ružena/poselib",
      "character/Ružena/poselib/face",
      "character/Ružena/poselib/hand",
      "path",                     // Implicit.
      "path/without",             // Implicit.
      "path/without/simplename",  // From CDF.
  };

  AssetCatalogTree *tree = service.get_catalog_tree();

  int i = 0;
  tree->foreach_item([&](const AssetCatalogTreeItem &actual_item) {
    ASSERT_LT(i, expected_paths.size())
        << "More catalogs in tree than expected; did not expect " << actual_item.catalog_path();

    /* Is the catalog name as expected? "character", "Ellie", ... */
    EXPECT_EQ(expected_paths[i].filename().string(), actual_item.get_name());
    /* Does the number of parents match? */
    EXPECT_EQ(count_path_parents(expected_paths[i]), actual_item.count_parents());
    EXPECT_EQ(expected_paths[i].generic_string(), actual_item.catalog_path());

    i++;
  });
}

TEST_F(AssetCatalogTest, write_single_file)
{
  TestableAssetCatalogService service(asset_library_root_);
  service.load_from_disk(asset_library_root_ / "blender_assets.cats.txt");

  const CatalogFilePath save_to_path = use_temp_path();
  AssetCatalogDefinitionFile *cdf = service.get_catalog_definition_file();
  cdf->write_to_disk(save_to_path);

  AssetCatalogService loaded_service(save_to_path);
  loaded_service.load_from_disk();

  // Test that the expected catalogs are there.
  EXPECT_NE(nullptr, loaded_service.find_catalog(UUID_POSES_ELLIE));
  EXPECT_NE(nullptr, loaded_service.find_catalog(UUID_POSES_ELLIE_WHITESPACE));
  EXPECT_NE(nullptr, loaded_service.find_catalog(UUID_POSES_ELLIE_TRAILING_SLASH));
  EXPECT_NE(nullptr, loaded_service.find_catalog(UUID_POSES_RUZENA));
  EXPECT_NE(nullptr, loaded_service.find_catalog(UUID_POSES_RUZENA_HAND));
  EXPECT_NE(nullptr, loaded_service.find_catalog(UUID_POSES_RUZENA_FACE));

  // Test that the invalid catalog definition wasn't copied.
  EXPECT_EQ(nullptr, loaded_service.find_catalog(UUID_ID_WITHOUT_PATH));

  // TODO(@sybren): test ordering of catalogs in the file.
}

TEST_F(AssetCatalogTest, create_first_catalog_from_scratch)
{
  /* Even from scratch a root directory should be known. */
  const CatalogFilePath temp_lib_root = use_temp_path();
  AssetCatalogService service(temp_lib_root);

  /* Just creating the service should NOT create the path. */
  EXPECT_FALSE(fs::exists(temp_lib_root));

  AssetCatalog *cat = service.create_catalog("some/catalog/path");
  ASSERT_NE(nullptr, cat);
  EXPECT_EQ(cat->path, "some/catalog/path");
  EXPECT_EQ(cat->simple_name, "some-catalog-path");

  /* Creating a new catalog should create the directory + the default file. */
  EXPECT_TRUE(fs::is_directory(temp_lib_root));

  const CatalogFilePath definition_file_path = temp_lib_root /
                                               AssetCatalogService::DEFAULT_CATALOG_FILENAME;
  EXPECT_TRUE(fs::is_regular_file(definition_file_path));

  AssetCatalogService loaded_service(temp_lib_root);
  loaded_service.load_from_disk();

  // Test that the expected catalog is there.
  AssetCatalog *written_cat = loaded_service.find_catalog(cat->catalog_id);
  ASSERT_NE(nullptr, written_cat);
  EXPECT_EQ(written_cat->catalog_id, cat->catalog_id);
  EXPECT_EQ(written_cat->path, cat->path);
}

TEST_F(AssetCatalogTest, create_catalog_after_loading_file)
{
  const CatalogFilePath temp_lib_root = use_temp_path();

  /* Copy the asset catalog definition files to a separate location, so that we can test without
   * overwriting the test file in SVN. */
  fs::copy(asset_library_root_, temp_lib_root, fs::copy_options::recursive);

  AssetCatalogService service(temp_lib_root);
  service.load_from_disk();
  EXPECT_NE(nullptr, service.find_catalog(UUID_POSES_ELLIE)) << "expected catalogs to be loaded";

  /* This should create a new catalog and write to disk. */
  const AssetCatalog *new_catalog = service.create_catalog("new/catalog");

  /* Reload the written catalog files. */
  AssetCatalogService loaded_service(temp_lib_root);
  loaded_service.load_from_disk();

  EXPECT_NE(nullptr, service.find_catalog(UUID_POSES_ELLIE))
      << "expected pre-existing catalogs to be kept in the file";
  EXPECT_NE(nullptr, service.find_catalog(new_catalog->catalog_id))
      << "expecting newly added catalog to exist in the file";
}

TEST_F(AssetCatalogTest, create_catalog_path_cleanup)
{
  const CatalogFilePath temp_lib_root = use_temp_path();
  AssetCatalogService service(temp_lib_root);
  AssetCatalog *cat = service.create_catalog(" /some/path  /  ");

  EXPECT_FALSE(BLI_uuid_is_nil(cat->catalog_id));
  EXPECT_EQ("some/path", cat->path);
  EXPECT_EQ("some-path", cat->simple_name);
}

TEST_F(AssetCatalogTest, create_catalog_simple_name)
{
  const CatalogFilePath temp_lib_root = use_temp_path();
  AssetCatalogService service(temp_lib_root);
  AssetCatalog *cat = service.create_catalog(
      "production/Spite Fright/Characters/Victora/Pose Library/Approved/Body Parts/Hands");

  EXPECT_FALSE(BLI_uuid_is_nil(cat->catalog_id));
  EXPECT_EQ("production/Spite Fright/Characters/Victora/Pose Library/Approved/Body Parts/Hands",
            cat->path);
  EXPECT_EQ("...ht-Characters-Victora-Pose Library-Approved-Body Parts-Hands", cat->simple_name);
}

}  // namespace blender::bke::tests
