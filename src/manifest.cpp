#include "omg/manifest.hpp"

#include <cstddef>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace omg {

using Json = nlohmann::json;

void to_json(Json& json, const ShardMetadata& shard) {
  json = Json{{"file", shard.file},
              {"vertex_begin", shard.vertex_begin},
              {"vertex_end", shard.vertex_end},
              {"edge_count", shard.edge_count}};
}

void from_json(const Json& json, ShardMetadata& shard) {
  json.at("file").get_to(shard.file);
  json.at("vertex_begin").get_to(shard.vertex_begin);
  json.at("vertex_end").get_to(shard.vertex_end);
  json.at("edge_count").get_to(shard.edge_count);
}

void to_json(Json& json, const GraphManifest& manifest) {
  json = Json{{"version", manifest.version},
              {"format", manifest.format},
              {"byte_order", manifest.byte_order},
              {"input_file", manifest.input_file},
              {"vertex_count", manifest.vertex_count},
              {"input_edge_count", manifest.input_edge_count},
              {"edge_count", manifest.edge_count},
              {"duplicate_edge_count", manifest.duplicate_edge_count},
              {"self_loop_count", manifest.self_loop_count},
              {"shards", manifest.shards}};
}

void from_json(const Json& json, GraphManifest& manifest) {
  json.at("version").get_to(manifest.version);
  json.at("format").get_to(manifest.format);
  json.at("byte_order").get_to(manifest.byte_order);
  json.at("input_file").get_to(manifest.input_file);
  json.at("vertex_count").get_to(manifest.vertex_count);
  json.at("input_edge_count").get_to(manifest.input_edge_count);
  json.at("edge_count").get_to(manifest.edge_count);
  json.at("duplicate_edge_count").get_to(manifest.duplicate_edge_count);
  json.at("self_loop_count").get_to(manifest.self_loop_count);
  json.at("shards").get_to(manifest.shards);
}

namespace {

auto checked_file_size(const std::filesystem::path& path) -> std::uintmax_t {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::runtime_error("cannot stat graph file " + path.string() + ": " + error.message());
  }
  return size;
}

} // namespace

void save_manifest(const std::filesystem::path& graph_directory, const GraphManifest& manifest) {
  const auto path = graph_directory / kManifestFileName;
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create manifest: " + path.string());
  }
  // Stream the manifest instead of materializing both a JSON DOM and its
  // serialized copy. Shard metadata can be large, so save must remain O(1)
  // beyond the manifest already resident in memory.
  output << "{\n"
         << "  \"byte_order\": " << Json(manifest.byte_order).dump() << ",\n"
         << "  \"duplicate_edge_count\": " << manifest.duplicate_edge_count << ",\n"
         << "  \"edge_count\": " << manifest.edge_count << ",\n"
         << "  \"format\": " << Json(manifest.format).dump() << ",\n"
         << "  \"input_edge_count\": " << manifest.input_edge_count << ",\n"
         << "  \"input_file\": " << Json(manifest.input_file).dump() << ",\n"
         << "  \"self_loop_count\": " << manifest.self_loop_count << ",\n"
         << "  \"shards\": [\n";
  for (std::size_t index = 0; index < manifest.shards.size(); ++index) {
    const auto& shard = manifest.shards[index];
    output << "    {\n"
           << "      \"edge_count\": " << shard.edge_count << ",\n"
           << "      \"file\": " << Json(shard.file).dump() << ",\n"
           << "      \"vertex_begin\": " << shard.vertex_begin << ",\n"
           << "      \"vertex_end\": " << shard.vertex_end << "\n"
           << "    }" << (index + 1U == manifest.shards.size() ? "\n" : ",\n");
  }
  output << "  ],\n"
         << "  \"version\": " << manifest.version << ",\n"
         << "  \"vertex_count\": " << manifest.vertex_count << "\n"
         << "}\n";
  if (!output) {
    throw std::runtime_error("cannot write manifest: " + path.string());
  }
}

auto load_manifest(const std::filesystem::path& graph_directory) -> GraphManifest {
  const auto path = graph_directory / kManifestFileName;
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open manifest: " + path.string());
  }

  GraphManifest manifest;
  try {
    Json json;
    input >> json;
    manifest = json.get<GraphManifest>();
  } catch (const std::exception& exception) {
    throw std::runtime_error("invalid manifest " + path.string() + ": " + exception.what());
  }

  if (manifest.version != kGraphFormatVersion || manifest.format != kGraphFormatName ||
      manifest.byte_order != "little") {
    throw std::runtime_error("unsupported graph format in " + path.string());
  }
  return manifest;
}

void validate_graph_files(const std::filesystem::path& graph_directory,
                          const GraphManifest& manifest) {
  const auto expected_vertices = manifest.vertex_count * sizeof(OriginalVertexId);
  const auto expected_outdegrees = manifest.vertex_count * sizeof(DenseVertexId);
  if (checked_file_size(graph_directory / kVerticesFileName) != expected_vertices) {
    throw std::runtime_error("vertices.bin size does not match manifest");
  }
  if (checked_file_size(graph_directory / kOutdegreesFileName) != expected_outdegrees) {
    throw std::runtime_error("outdegrees.bin size does not match manifest");
  }

  std::uint64_t next_vertex = 0;
  EdgeCount total_edges = 0;
  for (const auto& shard : manifest.shards) {
    if (static_cast<std::uint64_t>(shard.vertex_begin) != next_vertex ||
        shard.vertex_end < shard.vertex_begin || shard.vertex_end > manifest.vertex_count) {
      throw std::runtime_error("shard vertex ranges are not contiguous");
    }
    const auto expected_size = shard.edge_count * sizeof(DiskEdge);
    if (checked_file_size(graph_directory / shard.file) != expected_size) {
      throw std::runtime_error("shard size does not match manifest: " + shard.file);
    }
    next_vertex = shard.vertex_end;
    total_edges += shard.edge_count;
  }
  if (next_vertex != manifest.vertex_count || total_edges != manifest.edge_count) {
    throw std::runtime_error("shard totals do not match manifest");
  }
}

} // namespace omg
