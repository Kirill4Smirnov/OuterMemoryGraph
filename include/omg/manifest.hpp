#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "omg/types.hpp"

namespace omg {

inline constexpr std::uint32_t kGraphFormatVersion = 1;
inline constexpr const char* kGraphFormatName = "omg-destination-sharded";
inline constexpr const char* kManifestFileName = "manifest.json";
inline constexpr const char* kVerticesFileName = "vertices.bin";
inline constexpr const char* kOutdegreesFileName = "outdegrees.bin";

struct ShardMetadata {
  std::string file;
  DenseVertexId vertex_begin{};
  std::uint64_t vertex_end{}; // Exclusive; uint64 permits the 2^32 boundary.
  EdgeCount edge_count{};

  auto operator==(const ShardMetadata&) const -> bool = default;
};

struct GraphManifest {
  std::uint32_t version{kGraphFormatVersion};
  std::string format{kGraphFormatName};
  std::string byte_order{"little"};
  std::string input_file;
  VertexCount vertex_count{};
  EdgeCount input_edge_count{};
  EdgeCount edge_count{};
  EdgeCount duplicate_edge_count{};
  EdgeCount self_loop_count{};
  std::vector<ShardMetadata> shards;

  auto operator==(const GraphManifest&) const -> bool = default;
};

void save_manifest(const std::filesystem::path& graph_directory, const GraphManifest& manifest);
auto load_manifest(const std::filesystem::path& graph_directory) -> GraphManifest;
void validate_graph_files(const std::filesystem::path& graph_directory,
                          const GraphManifest& manifest);

} // namespace omg
