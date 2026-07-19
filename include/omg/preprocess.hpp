#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace omg {

struct PreprocessOptions {
  std::filesystem::path input;
  std::filesystem::path graph_directory;
  std::filesystem::path work_directory;
  std::uint64_t memory_budget_bytes{128ULL << 20U};
  std::size_t threads{1};
  std::size_t requested_shards{16};
};

void preprocess(const PreprocessOptions& options);

} // namespace omg
