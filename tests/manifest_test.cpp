#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "omg/manifest.hpp"
#include "temp_path.hpp"

TEST_CASE("Manifest JSON round-trip preserves metadata") {
  const auto directory = omg::test::unique_temp_directory("omg-manifest-test");

  omg::GraphManifest expected;
  expected.input_file = "edges.csv";
  expected.vertex_count = 5;
  expected.input_edge_count = 7;
  expected.edge_count = 6;
  expected.duplicate_edge_count = 1;
  expected.self_loop_count = 2;
  expected.shards = {{"shard-00000.bin", 0, 3, 4}, {"shard-00001.bin", 3, 5, 2}};

  omg::save_manifest(directory, expected);
  CHECK(omg::load_manifest(directory) == expected);

  std::filesystem::remove_all(directory);
}
