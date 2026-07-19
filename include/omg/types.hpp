#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace omg {

using OriginalVertexId = std::int32_t;
using DenseVertexId = std::uint32_t;
using VertexCount = std::uint64_t;
using EdgeCount = std::uint64_t;

inline constexpr DenseVertexId kInvalidDenseVertex = std::numeric_limits<DenseVertexId>::max();

struct OriginalEdge {
  OriginalVertexId source{};
  OriginalVertexId destination{};

  auto operator==(const OriginalEdge&) const -> bool = default;
};

// The on-disk order deliberately starts with destination: lexicographic sorting
// then produces the pull-oriented adjacency stream used by PageRank.
struct DiskEdge {
  DenseVertexId destination{};
  DenseVertexId source{};

  auto operator==(const DiskEdge&) const -> bool = default;
};

static_assert(sizeof(OriginalVertexId) == 4);
static_assert(sizeof(DenseVertexId) == 4);
static_assert(sizeof(DiskEdge) == 8);
static_assert(std::is_trivially_copyable_v<DiskEdge>);
static_assert(std::endian::native == std::endian::little,
              "The v1 graph format currently supports little-endian hosts only");

} // namespace omg
