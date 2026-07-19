#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "omg/manifest.hpp"
#include "omg/preprocess.hpp"
#include "omg/types.hpp"
#include "temp_path.hpp"

namespace {

template <typename Record>
auto read_records(const std::filesystem::path& path) -> std::vector<Record> {
  const auto bytes = std::filesystem::file_size(path);
  REQUIRE(bytes % sizeof(Record) == 0);
  std::vector<Record> records(static_cast<std::size_t>(bytes / sizeof(Record)));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(records.data()),
             static_cast<std::streamsize>(records.size() * sizeof(Record)));
  REQUIRE(input.good());
  return records;
}

auto read_binary(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Preprocessing remaps IDs, removes duplicates, and builds pull shards") {
  const auto root = omg::test::unique_temp_directory("omg-preprocess-test");
  const auto input_path = root / "edges.csv";
  const auto graph_directory = root / "graph.omg";
  const auto work_directory = root / "work";
  {
    std::ofstream input(input_path);
    input << "from,to\n"
          << "10,-2\n"
          << "10,5\n"
          << "10,5\n"
          << "-2,5\n"
          << "5,5\n"
          << "100,10\n";
  }

  omg::PreprocessOptions options;
  options.input = input_path;
  options.graph_directory = graph_directory;
  options.work_directory = work_directory;
  options.memory_budget_bytes = 32ULL << 20U;
  options.threads = 2;
  options.requested_shards = 3;
  omg::preprocess(options);

  const auto manifest = omg::load_manifest(graph_directory);
  REQUIRE_NOTHROW(omg::validate_graph_files(graph_directory, manifest));
  CHECK(manifest.vertex_count == 4);
  CHECK(manifest.input_edge_count == 6);
  CHECK(manifest.edge_count == 5);
  CHECK(manifest.duplicate_edge_count == 1);
  CHECK(manifest.self_loop_count == 1);

  CHECK(read_records<omg::OriginalVertexId>(graph_directory / omg::kVerticesFileName) ==
        std::vector<omg::OriginalVertexId>{-2, 5, 10, 100});
  CHECK(read_records<std::uint32_t>(graph_directory / omg::kOutdegreesFileName) ==
        std::vector<std::uint32_t>{1, 1, 2, 1});

  std::vector<omg::DiskEdge> actual_edges;
  for (const auto& shard : manifest.shards) {
    const auto shard_edges = read_records<omg::DiskEdge>(graph_directory / shard.file);
    actual_edges.insert(actual_edges.end(), shard_edges.begin(), shard_edges.end());
  }
  CHECK(actual_edges == std::vector<omg::DiskEdge>{{0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 3}});

  std::filesystem::remove_all(root);
}

TEST_CASE("Preprocessing rejects a work directory inside the graph directory") {
  const auto root = omg::test::unique_temp_directory("omg-preprocess-path-test");
  const auto input_path = root / "edges.csv";
  const auto graph_directory = root / "graph.omg";
  {
    std::ofstream input(input_path);
    input << "1,2\n";
  }

  omg::PreprocessOptions options;
  options.input = input_path;
  options.graph_directory = graph_directory;
  options.work_directory = graph_directory / "work";
  options.memory_budget_bytes = 32ULL << 20U;

  REQUIRE_THROWS_AS(omg::preprocess(options), std::invalid_argument);
  CHECK_FALSE(std::filesystem::exists(graph_directory));

  std::filesystem::remove_all(root);
}

TEST_CASE("Parallel run sorting produces the same graph as single-threaded sorting") {
  const auto root = omg::test::unique_temp_directory("omg-preprocess-parallel-test");
  const auto input_path = root / "edges.csv";
  const auto single_graph = root / "single.omg";
  const auto parallel_graph = root / "parallel.omg";
  {
    std::ofstream input(input_path);
    input << "from,to\n";
    for (std::uint32_t vertex = 1; vertex <= 40000; ++vertex) {
      input << 0 << ',' << vertex << '\n';
      input << vertex << ',' << 0 << '\n';
    }
  }

  omg::PreprocessOptions options;
  options.input = input_path;
  options.graph_directory = single_graph;
  options.work_directory = root / "single-work";
  options.memory_budget_bytes = 32ULL << 20U;
  options.threads = 1;
  options.requested_shards = 8;
  omg::preprocess(options);

  options.graph_directory = parallel_graph;
  options.work_directory = root / "parallel-work";
  options.threads = 4;
  omg::preprocess(options);

  const auto single_manifest = omg::load_manifest(single_graph);
  const auto parallel_manifest = omg::load_manifest(parallel_graph);
  REQUIRE(single_manifest == parallel_manifest);
  CHECK(read_binary(single_graph / omg::kVerticesFileName) ==
        read_binary(parallel_graph / omg::kVerticesFileName));
  CHECK(read_binary(single_graph / omg::kOutdegreesFileName) ==
        read_binary(parallel_graph / omg::kOutdegreesFileName));
  for (const auto& shard : single_manifest.shards) {
    CHECK(read_binary(single_graph / shard.file) == read_binary(parallel_graph / shard.file));
  }

  std::filesystem::remove_all(root);
}
