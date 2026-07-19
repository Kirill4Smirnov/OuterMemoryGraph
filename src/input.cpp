#include "omg/input.hpp"

#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace omg {
namespace {

auto trim(std::string_view value) -> std::string_view {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

auto parse_integer(std::string_view& input, OriginalVertexId& value) -> bool {
  input = trim(input);
  if (input.empty()) {
    return false;
  }

  const char* begin = input.data();
  const char* end = input.data() + input.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{}) {
    return false;
  }
  input.remove_prefix(static_cast<std::size_t>(result.ptr - begin));
  return true;
}

auto parse_edge_line(std::string_view line, OriginalEdge& edge) -> bool {
  if (!parse_integer(line, edge.source)) {
    return false;
  }

  line = trim(line);
  if (!line.empty() && line.front() == ',') {
    line.remove_prefix(1);
  }
  if (!parse_integer(line, edge.destination)) {
    return false;
  }

  line = trim(line);
  return line.empty();
}

auto looks_like_header(std::string_view line) -> bool {
  std::string lowered;
  lowered.reserve(line.size());
  for (const char character : line) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  const bool has_source =
      lowered.find("from") != std::string::npos || lowered.find("source") != std::string::npos;
  const bool has_destination = lowered.find("to") != std::string::npos ||
                               lowered.find("target") != std::string::npos ||
                               lowered.find("destination") != std::string::npos;
  return has_source && has_destination;
}

} // namespace

EdgeListReader::EdgeListReader(const std::filesystem::path& path, const std::size_t buffer_bytes)
    : path_(path), stream_buffer_(buffer_bytes) {
  if (stream_buffer_.empty()) {
    throw std::invalid_argument("input buffer size must be positive");
  }
  input_.rdbuf()->pubsetbuf(stream_buffer_.data(),
                            static_cast<std::streamsize>(stream_buffer_.size()));
  input_.open(path_);
  if (!input_) {
    throw std::runtime_error("cannot open edge list: " + path_.string());
  }
}

auto EdgeListReader::next(OriginalEdge& edge) -> bool {
  while (std::getline(input_, line_)) {
    ++stats_.lines;
    std::string_view line = trim(line_);
    if (line.empty()) {
      continue;
    }
    if (line.front() == '#') {
      ++stats_.comments;
      continue;
    }
    if (parse_edge_line(line, edge)) {
      ++stats_.edges;
      return true;
    }
    if (!skipped_header_ && stats_.edges == 0 && looks_like_header(line)) {
      skipped_header_ = true;
      ++stats_.headers;
      continue;
    }
    throw std::runtime_error("invalid edge at " + path_.string() + ':' +
                             std::to_string(stats_.lines) + ": " + line_);
  }

  if (!input_.eof()) {
    throw std::runtime_error("failed while reading edge list: " + path_.string());
  }
  return false;
}

} // namespace omg
