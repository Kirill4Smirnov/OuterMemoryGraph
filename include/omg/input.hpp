#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "omg/types.hpp"

namespace omg {

struct InputStats {
  std::uint64_t lines{};
  EdgeCount edges{};
  std::uint64_t comments{};
  std::uint64_t headers{};
};

// Streaming reader for both the assignment CSV format and SNAP-style
// whitespace/tab-separated files. The reader keeps only one input line in
// memory and therefore does not scale with the dataset size.
class EdgeListReader {
public:
  explicit EdgeListReader(const std::filesystem::path& path, std::size_t buffer_bytes = 1U << 20U);

  EdgeListReader(const EdgeListReader&) = delete;
  auto operator=(const EdgeListReader&) -> EdgeListReader& = delete;

  auto next(OriginalEdge& edge) -> bool;
  [[nodiscard]] auto stats() const noexcept -> const InputStats& { return stats_; }

private:
  std::filesystem::path path_;
  std::vector<char> stream_buffer_;
  std::ifstream input_;
  std::string line_;
  InputStats stats_;
  bool skipped_header_{};
};

} // namespace omg
