#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>

#include "omg/manifest.hpp"
#include "omg/pagerank.hpp"
#include "omg/preprocess.hpp"
#include "omg/types.hpp"
#include "temp_path.hpp"

namespace {

auto read_ranks(const std::filesystem::path& path) -> std::map<omg::OriginalVertexId, double> {
  std::ifstream input(path);
  std::string line;
  REQUIRE(std::getline(input, line));
  REQUIRE(line == "vertex,rank");

  std::map<omg::OriginalVertexId, double> ranks;
  while (std::getline(input, line)) {
    const auto comma = line.find(',');
    REQUIRE(comma != std::string::npos);
    const auto identifier = static_cast<omg::OriginalVertexId>(std::stol(line.substr(0, comma)));
    const auto rank = std::stod(line.substr(comma + 1));
    ranks.emplace(identifier, rank);
  }
  return ranks;
}

auto read_text(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

template <typename Record>
auto read_records(const std::filesystem::path& path) -> std::vector<Record> {
  const auto bytes = std::filesystem::file_size(path);
  if (bytes % sizeof(Record) != 0) {
    throw std::runtime_error("invalid test record file size");
  }
  std::vector<Record> records(static_cast<std::size_t>(bytes / sizeof(Record)));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(records.data()),
             static_cast<std::streamsize>(records.size() * sizeof(Record)));
  if (!input) {
    throw std::runtime_error("cannot read test record file");
  }
  return records;
}

struct TestGraph {
  std::filesystem::path root;
  std::filesystem::path graph;

  explicit TestGraph(const std::string_view edges, const std::size_t shards = 4) {
    root = omg::test::unique_temp_directory("omg-pagerank-test");
    graph = root / "graph.omg";
    const auto input_path = root / "edges.csv";
    {
      std::ofstream input(input_path);
      input << "from,to\n" << edges;
    }

    omg::PreprocessOptions options;
    options.input = input_path;
    options.graph_directory = graph;
    options.work_directory = root / "work";
    options.memory_budget_bytes = 32ULL << 20U;
    options.threads = 2;
    options.requested_shards = shards;
    omg::preprocess(options);
  }

  ~TestGraph() { std::filesystem::remove_all(root); }
};

} // namespace

TEST_CASE("PageRank redistributes dangling mass") {
  TestGraph graph("1,2\n", 2);
  const auto output = graph.root / "ranks.csv";

  omg::PageRankOptions options;
  options.graph_directory = graph.graph;
  options.output = output;
  options.memory_budget_bytes = 32ULL << 20U;
  options.threads = 2;
  options.damping = 0.85;
  options.tolerance = 1e-13;
  options.max_iterations = 200;
  const auto result = omg::run_pagerank(options);

  CHECK(result.residual_l1 <= options.tolerance);
  CHECK(result.rank_sum == Catch::Approx(1.0).margin(1e-14));
  const auto ranks = read_ranks(output);
  REQUIRE(ranks.size() == 2);
  CHECK(ranks.at(1) == Catch::Approx(0.3508771929824561).margin(1e-12));
  CHECK(ranks.at(2) == Catch::Approx(0.6491228070175439).margin(1e-12));
}

TEST_CASE("Shard scheduling is deterministic across thread counts") {
  TestGraph graph("1,2\n2,1\n3,1\n3,2\n", 2);
  const auto single_output = graph.root / "single.csv";
  const auto parallel_output = graph.root / "parallel.csv";

  omg::PageRankOptions options;
  options.graph_directory = graph.graph;
  options.output = single_output;
  options.memory_budget_bytes = 32ULL << 20U;
  options.threads = 1;
  options.fixed_iterations = 30;
  omg::run_pagerank(options);

  options.output = parallel_output;
  options.threads = 4;
  omg::run_pagerank(options);

  CHECK(read_text(single_output) == read_text(parallel_output));
}

TEST_CASE("PageRank matches the official LDBC Graphalytics reference") {
  const auto root = omg::test::unique_temp_directory("omg-graphalytics-test");
  const auto graph_directory = root / "graph.omg";
  const auto output = root / "pagerank.csv";
  const auto fixture = std::filesystem::path(OMG_TEST_DATA_DIR) / "graphalytics";
  omg::PreprocessOptions preprocess_options;
  preprocess_options.input = fixture / "test-pr-directed.e";
  preprocess_options.graph_directory = graph_directory;
  preprocess_options.work_directory = root / "work";
  preprocess_options.memory_budget_bytes = 32ULL << 20U;
  preprocess_options.threads = 4;
  preprocess_options.requested_shards = 8;
  omg::preprocess(preprocess_options);

  omg::PageRankOptions pagerank_options;
  pagerank_options.graph_directory = graph_directory;
  pagerank_options.output = output;
  pagerank_options.memory_budget_bytes = 32ULL << 20U;
  pagerank_options.threads = 4;
  pagerank_options.damping = 0.85;
  pagerank_options.fixed_iterations = 14;
  omg::run_pagerank(pagerank_options);

  const auto actual = read_ranks(output);
  std::ifstream expected_input(fixture / "test-pr-directed-PR");
  omg::OriginalVertexId identifier;
  double expected_rank = 0.0;
  std::size_t compared = 0;
  double maximum_relative_error = 0.0;
  while (expected_input >> identifier >> expected_rank) {
    REQUIRE(actual.contains(identifier));
    const double relative_error =
        std::abs(actual.at(identifier) - expected_rank) / std::abs(expected_rank);
    maximum_relative_error = std::max(maximum_relative_error, relative_error);
    ++compared;
  }
  CHECK(compared == 50);
  CHECK(actual.size() == compared);
  CHECK(maximum_relative_error <= 1e-4);

  std::filesystem::remove_all(root);
}

TEST_CASE("A 50000-degree hyper-node is streamed without a large adjacency "
          "allocation") {
  const auto root = omg::test::unique_temp_directory("omg-hypernode-test");
  const auto input_path = root / "hypernode.csv";
  const auto graph_directory = root / "graph.omg";
  const auto output = root / "pagerank.csv";
  {
    std::ofstream input(input_path);
    input << "from,to\n";
    for (std::uint32_t vertex = 1; vertex <= 50000; ++vertex) {
      input << 0 << ',' << vertex << '\n';
      input << vertex << ',' << 0 << '\n';
    }
  }

  omg::PreprocessOptions preprocess_options;
  preprocess_options.input = input_path;
  preprocess_options.graph_directory = graph_directory;
  preprocess_options.work_directory = root / "work";
  preprocess_options.memory_budget_bytes = 32ULL << 20U;
  preprocess_options.threads = 8;
  preprocess_options.requested_shards = 8;
  omg::preprocess(preprocess_options);

  const auto outdegrees = read_records<std::uint32_t>(graph_directory / omg::kOutdegreesFileName);
  REQUIRE(outdegrees.size() == 50001);
  CHECK(outdegrees[0] == 50000);

  omg::PageRankOptions pagerank_options;
  pagerank_options.graph_directory = graph_directory;
  pagerank_options.output = output;
  pagerank_options.memory_budget_bytes = 32ULL << 20U;
  pagerank_options.threads = 8;
  pagerank_options.fixed_iterations = 3;
  const auto result = omg::run_pagerank(pagerank_options);
  CHECK(result.rank_sum == Catch::Approx(1.0).margin(1e-12));

  std::filesystem::remove_all(root);
}
