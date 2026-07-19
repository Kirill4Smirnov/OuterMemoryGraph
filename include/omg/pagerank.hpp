#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace omg {

struct PageRankOptions {
  std::filesystem::path graph_directory;
  std::filesystem::path output;
  std::uint64_t memory_budget_bytes{128ULL << 20U};
  std::size_t threads{1};
  double damping{0.85};
  std::optional<std::size_t> fixed_iterations;
  std::size_t max_iterations{100};
  double tolerance{1e-8};
};

struct PageRankResult {
  std::size_t iterations{};
  double residual_l1{};
  double rank_sum{};
};

auto run_pagerank(const PageRankOptions& options) -> PageRankResult;

} // namespace omg
