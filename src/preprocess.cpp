#include "omg/preprocess.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include "omg/input.hpp"
#include "omg/manifest.hpp"
#include "omg/types.hpp"

namespace omg {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kMiB = 1ULL << 20U;
constexpr std::size_t kInputBufferBytes = 1U << 20U;
constexpr std::size_t kRunBufferBytes = 64U << 10U;
constexpr std::size_t kOutputBufferBytes = 1U << 20U;
constexpr std::size_t kMergeFanIn = 64;
constexpr std::size_t kMinimumSortPartitionBytes = 256U << 10U;
constexpr std::size_t kMaximumSortWorkers = 64;
constexpr std::uint64_t kMergeRuntimeReserveBytes = 8ULL << 20U;

class ScopedDirectoryCleanup {
public:
  explicit ScopedDirectoryCleanup(fs::path path) : path_(std::move(path)) {}
  ScopedDirectoryCleanup(const ScopedDirectoryCleanup&) = delete;
  auto operator=(const ScopedDirectoryCleanup&) -> ScopedDirectoryCleanup& = delete;

  ~ScopedDirectoryCleanup() {
    if (!keep_) {
      std::error_code ignored;
      fs::remove_all(path_, ignored);
    }
  }

  void keep() noexcept { keep_ = true; }

private:
  fs::path path_;
  bool keep_{};
};

template <typename Record> class BinaryRunReader {
public:
  explicit BinaryRunReader(const fs::path& path, const std::size_t buffer_bytes = kRunBufferBytes)
      : path_(path), buffer_(std::max<std::size_t>(1, buffer_bytes / sizeof(Record))) {
    input_.open(path_, std::ios::binary);
    if (!input_) {
      throw std::runtime_error("cannot open binary run: " + path_.string());
    }
  }

  BinaryRunReader(const BinaryRunReader&) = delete;
  auto operator=(const BinaryRunReader&) -> BinaryRunReader& = delete;

  auto next(Record& record) -> bool {
    if (position_ == size_) {
      input_.read(reinterpret_cast<char*>(buffer_.data()),
                  static_cast<std::streamsize>(buffer_.size() * sizeof(Record)));
      const auto bytes = input_.gcount();
      if (bytes == 0) {
        if (!input_.eof()) {
          throw std::runtime_error("failed while reading binary run: " + path_.string());
        }
        return false;
      }
      if (bytes % static_cast<std::streamsize>(sizeof(Record)) != 0) {
        throw std::runtime_error("truncated record in binary run: " + path_.string());
      }
      size_ = static_cast<std::size_t>(bytes) / sizeof(Record);
      position_ = 0;
    }
    record = buffer_[position_++];
    return true;
  }

private:
  fs::path path_;
  std::ifstream input_;
  std::vector<Record> buffer_;
  std::size_t position_{};
  std::size_t size_{};
};

template <typename Record> class BufferedBinaryWriter {
public:
  explicit BufferedBinaryWriter(const fs::path& path,
                                const std::size_t buffer_bytes = kOutputBufferBytes)
      : path_(path), buffer_(std::max<std::size_t>(1, buffer_bytes / sizeof(Record))) {
    output_.open(path_, std::ios::binary | std::ios::trunc);
    if (!output_) {
      throw std::runtime_error("cannot create binary file: " + path_.string());
    }
  }

  BufferedBinaryWriter(const BufferedBinaryWriter&) = delete;
  auto operator=(const BufferedBinaryWriter&) -> BufferedBinaryWriter& = delete;

  ~BufferedBinaryWriter() {
    try {
      close();
    } catch (...) {
    }
  }

  void append(const Record& record) {
    if (closed_) {
      throw std::logic_error("append to closed binary writer");
    }
    buffer_[size_++] = record;
    if (size_ == buffer_.size()) {
      flush_buffer();
    }
  }

  void close() {
    if (closed_) {
      return;
    }
    flush_buffer();
    output_.close();
    if (!output_) {
      throw std::runtime_error("cannot finish binary file: " + path_.string());
    }
    closed_ = true;
  }

private:
  void flush_buffer() {
    if (size_ == 0) {
      return;
    }
    output_.write(reinterpret_cast<const char*>(buffer_.data()),
                  static_cast<std::streamsize>(size_ * sizeof(Record)));
    if (!output_) {
      throw std::runtime_error("cannot write binary file: " + path_.string());
    }
    size_ = 0;
  }

  fs::path path_;
  std::ofstream output_;
  std::vector<Record> buffer_;
  std::size_t size_{};
  bool closed_{};
};

struct VertexRunResult {
  std::vector<fs::path> runs;
  EdgeCount input_edges{};
  std::uint64_t input_fingerprint{};
  std::size_t maximum_sort_workers{};
  OriginalVertexId minimum_id{};
  OriginalVertexId maximum_id{};
};

struct EdgeRunResult {
  std::vector<fs::path> runs;
  EdgeCount input_edges{};
  std::uint64_t input_fingerprint{};
  std::size_t maximum_sort_workers{};
};

struct MergeResult {
  EdgeCount edges{};
  EdgeCount self_loops{};
  std::vector<ShardMetadata> shards;
};

auto unique_suffix() -> std::string {
  const auto ticks = Clock::now().time_since_epoch().count();
  return std::to_string(static_cast<long long>(::getpid())) + '-' +
         std::to_string(static_cast<long long>(ticks));
}

auto run_name(const std::string_view prefix, const std::size_t index) -> std::string {
  std::ostringstream name;
  name << prefix << '-' << std::setw(5) << std::setfill('0') << index << ".bin";
  return name.str();
}

auto elapsed_seconds(const Clock::time_point start) -> double {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void require_nonempty_options(const PreprocessOptions& options) {
  if (options.input.empty() || options.graph_directory.empty() || options.work_directory.empty()) {
    throw std::invalid_argument("input, graph directory, and work directory must be set");
  }
  if (options.memory_budget_bytes < 32ULL * kMiB) {
    throw std::invalid_argument("preprocessing requires a memory budget of at least 32 MiB");
  }
  if (options.threads == 0 || options.requested_shards == 0) {
    throw std::invalid_argument("thread and shard counts must be positive");
  }
  if (options.requested_shards > 1'000'000) {
    throw std::invalid_argument("requested shard count is unreasonably large");
  }
  if (options.graph_directory.filename().empty()) {
    throw std::invalid_argument("graph directory must have a final path component");
  }
  const auto graph_path = fs::absolute(options.graph_directory).lexically_normal();
  const auto work_path = fs::absolute(options.work_directory).lexically_normal();
  const auto mismatch =
      std::mismatch(graph_path.begin(), graph_path.end(), work_path.begin(), work_path.end());
  if (mismatch.first == graph_path.end()) {
    throw std::invalid_argument("work directory must not be the graph directory or its child");
  }
  if (!fs::is_regular_file(options.input)) {
    throw std::invalid_argument("input is not a regular file: " + options.input.string());
  }
  if (fs::exists(options.graph_directory)) {
    throw std::invalid_argument("graph directory already exists: " +
                                options.graph_directory.string());
  }
}

auto working_memory_limit(const std::uint64_t budget) -> std::uint64_t {
  // Leave 20% for the executable, stream objects, allocator metadata, and
  // temporary sorting state that is not represented by vector capacities.
  return budget - budget / 5U;
}

template <typename Record>
void write_records(const fs::path& path, const std::vector<Record>& records) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create run: " + path.string());
  }
  output.write(reinterpret_cast<const char*>(records.data()),
               static_cast<std::streamsize>(records.size() * sizeof(Record)));
  if (!output) {
    throw std::runtime_error("cannot write run: " + path.string());
  }
}

template <typename Record, typename Less>
auto sort_and_write_run(const fs::path& path, std::vector<Record>& records,
                        const std::size_t requested_threads, Less less) -> std::size_t {
  if (records.empty()) {
    throw std::invalid_argument("cannot create an empty sorted run");
  }

  const auto minimum_partition_records =
      std::max<std::size_t>(1, kMinimumSortPartitionBytes / sizeof(Record));
  const auto useful_workers =
      (records.size() + minimum_partition_records - 1U) / minimum_partition_records;
  const auto worker_count =
      std::min({requested_threads, useful_workers, kMaximumSortWorkers, records.size()});

  std::vector<std::size_t> partition_begins(worker_count);
  std::vector<std::size_t> unique_ends(worker_count);
  std::exception_ptr failure;
  std::mutex failure_mutex;

  auto sort_partition = [&](const std::size_t partition) {
    try {
      const auto begin = partition * records.size() / worker_count;
      const auto end = (partition + 1U) * records.size() / worker_count;
      partition_begins[partition] = begin;
      auto first = records.begin() + static_cast<std::ptrdiff_t>(begin);
      const auto last = records.begin() + static_cast<std::ptrdiff_t>(end);
      std::sort(first, last, less);
      unique_ends[partition] = static_cast<std::size_t>(std::unique(first, last) - records.begin());
    } catch (...) {
      std::lock_guard lock(failure_mutex);
      if (!failure) {
        failure = std::current_exception();
      }
    }
  };

  std::vector<std::jthread> workers;
  workers.reserve(worker_count);
  for (std::size_t partition = 0; partition < worker_count; ++partition) {
    workers.emplace_back(sort_partition, partition);
  }
  for (auto& worker : workers) {
    worker.join();
  }
  if (failure) {
    std::rethrow_exception(failure);
  }

  struct PartitionHead {
    std::size_t partition{};
    std::size_t position{};
  };
  const auto head_greater = [&](const PartitionHead& left, const PartitionHead& right) {
    const auto& left_record = records[left.position];
    const auto& right_record = records[right.position];
    if (less(right_record, left_record)) {
      return true;
    }
    if (less(left_record, right_record)) {
      return false;
    }
    return left.partition > right.partition;
  };
  std::priority_queue<PartitionHead, std::vector<PartitionHead>, decltype(head_greater)> heap(
      head_greater);
  for (std::size_t partition = 0; partition < worker_count; ++partition) {
    if (partition_begins[partition] < unique_ends[partition]) {
      heap.push({partition, partition_begins[partition]});
    }
  }

  BufferedBinaryWriter<Record> output(path);
  std::optional<Record> previous;
  while (!heap.empty()) {
    auto head = heap.top();
    heap.pop();
    const auto& record = records[head.position];
    if (!previous || *previous != record) {
      output.append(record);
      previous = record;
    }
    ++head.position;
    if (head.position < unique_ends[head.partition]) {
      heap.push(head);
    }
  }
  output.close();
  return worker_count;
}

auto build_vertex_runs(const PreprocessOptions& options, const fs::path& session_directory)
    -> VertexRunResult {
  const auto working_bytes = working_memory_limit(options.memory_budget_bytes);
  if (working_bytes <= 2ULL * kMiB) {
    throw std::invalid_argument("memory budget is too small for vertex runs");
  }
  const auto capacity_u64 = (working_bytes - 2ULL * kMiB) / sizeof(OriginalVertexId);
  const auto capacity = static_cast<std::size_t>(
      std::min<std::uint64_t>(capacity_u64, std::numeric_limits<std::size_t>::max()));

  std::vector<OriginalVertexId> identifiers;
  identifiers.reserve(capacity);
  VertexRunResult result;
  bool have_identifier = false;

  auto flush = [&]() {
    if (identifiers.empty()) {
      return;
    }
    const auto path = session_directory / run_name("vertices-run", result.runs.size());
    result.maximum_sort_workers =
        std::max(result.maximum_sort_workers, sort_and_write_run(path, identifiers, options.threads,
                                                                 std::less<OriginalVertexId>{}));
    result.runs.push_back(path);
    identifiers.clear();
  };

  EdgeListReader reader(options.input, kInputBufferBytes);
  OriginalEdge edge;
  while (reader.next(edge)) {
    if (identifiers.size() + 2U > identifiers.capacity()) {
      flush();
    }
    identifiers.push_back(edge.source);
    identifiers.push_back(edge.destination);
    if (!have_identifier) {
      result.minimum_id = std::min(edge.source, edge.destination);
      result.maximum_id = std::max(edge.source, edge.destination);
      have_identifier = true;
    } else {
      result.minimum_id = std::min({result.minimum_id, edge.source, edge.destination});
      result.maximum_id = std::max({result.maximum_id, edge.source, edge.destination});
    }
  }
  flush();
  result.input_edges = reader.stats().edges;
  result.input_fingerprint = reader.stats().edge_fingerprint;

  if (!have_identifier || result.input_edges == 0) {
    throw std::invalid_argument("the input graph has no edges");
  }
  return result;
}

struct VertexHead {
  OriginalVertexId identifier{};
  std::size_t run{};
};

struct VertexHeadGreater {
  auto operator()(const VertexHead& left, const VertexHead& right) const -> bool {
    return std::tie(left.identifier, left.run) > std::tie(right.identifier, right.run);
  }
};

auto merge_vertex_runs(const std::vector<fs::path>& runs, const fs::path& output_path)
    -> VertexCount {
  if (runs.empty()) {
    throw std::invalid_argument("no vertex runs to merge");
  }

  std::vector<std::unique_ptr<BinaryRunReader<OriginalVertexId>>> readers;
  readers.reserve(runs.size());
  std::priority_queue<VertexHead, std::vector<VertexHead>, VertexHeadGreater> heap;
  for (std::size_t index = 0; index < runs.size(); ++index) {
    readers.push_back(std::make_unique<BinaryRunReader<OriginalVertexId>>(runs[index]));
    OriginalVertexId identifier;
    if (readers.back()->next(identifier)) {
      heap.push({identifier, index});
    }
  }

  BufferedBinaryWriter<OriginalVertexId> output(output_path);
  std::optional<OriginalVertexId> previous;
  VertexCount count = 0;
  while (!heap.empty()) {
    const auto head = heap.top();
    heap.pop();
    if (!previous || *previous != head.identifier) {
      if (count >= kInvalidDenseVertex) {
        throw std::runtime_error("v1 format supports fewer than 2^32-1 vertices");
      }
      output.append(head.identifier);
      previous = head.identifier;
      ++count;
    }

    OriginalVertexId next_identifier;
    if (readers[head.run]->next(next_identifier)) {
      heap.push({next_identifier, head.run});
    }
  }
  output.close();
  return count;
}

void remove_merged_runs(const std::vector<fs::path>& runs) {
  for (const auto& run : runs) {
    std::error_code error;
    if (!fs::remove(run, error) || error) {
      throw std::runtime_error("cannot remove merged run: " + run.string() +
                               (error ? ": " + error.message() : ""));
    }
  }
}

auto compact_vertex_runs(std::vector<fs::path> runs, const fs::path& session_directory)
    -> std::vector<fs::path> {
  std::size_t pass = 0;
  while (runs.size() > kMergeFanIn) {
    std::cerr << "preprocess: compacting vertex runs pass=" << pass << " input_runs=" << runs.size()
              << '\n';
    std::vector<fs::path> next_runs;
    next_runs.reserve((runs.size() + kMergeFanIn - 1U) / kMergeFanIn);
    for (std::size_t begin = 0; begin < runs.size(); begin += kMergeFanIn) {
      const auto end = std::min(runs.size(), begin + kMergeFanIn);
      if (end - begin == 1U) {
        next_runs.push_back(runs[begin]);
        continue;
      }

      const std::vector<fs::path> group(runs.begin() + static_cast<std::ptrdiff_t>(begin),
                                        runs.begin() + static_cast<std::ptrdiff_t>(end));
      const auto output =
          session_directory / run_name("vertices-merge-" + std::to_string(pass), next_runs.size());
      static_cast<void>(merge_vertex_runs(group, output));
      remove_merged_runs(group);
      next_runs.push_back(output);
    }
    runs = std::move(next_runs);
    ++pass;
  }
  return runs;
}

class IdMapper {
public:
  IdMapper(const fs::path& vertices_path, const VertexCount vertex_count,
           const OriginalVertexId minimum_id, const OriginalVertexId maximum_id,
           const std::uint64_t working_bytes)
      : minimum_id_(minimum_id) {
    const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum_id) -
                                                 static_cast<std::int64_t>(minimum_id)) +
                      1U;
    const auto direct_bytes = span * sizeof(DenseVertexId);
    const bool compact_range = span <= vertex_count * 2U;
    if (compact_range && direct_bytes + 8ULL * kMiB < working_bytes &&
        span <= std::numeric_limits<std::size_t>::max()) {
      direct_.assign(static_cast<std::size_t>(span), kInvalidDenseVertex);
      BinaryRunReader<OriginalVertexId> reader(vertices_path);
      OriginalVertexId identifier;
      DenseVertexId dense = 0;
      while (reader.next(identifier)) {
        const auto index = static_cast<std::size_t>(static_cast<std::int64_t>(identifier) -
                                                    static_cast<std::int64_t>(minimum_id_));
        direct_[index] = dense++;
      }
      if (dense != vertex_count) {
        throw std::runtime_error("vertex count changed while building direct ID map");
      }
      std::cerr << "preprocess: ID mapping=direct span=" << span << '\n';
      return;
    }

    if (vertex_count > std::numeric_limits<std::size_t>::max()) {
      throw std::runtime_error("vertex table is too large for this host");
    }
    const auto sorted_bytes = vertex_count * sizeof(OriginalVertexId);
    if (sorted_bytes + 8ULL * kMiB >= working_bytes) {
      throw std::runtime_error(
          "memory budget is too small for the sparse original-to-dense ID mapping");
    }
    sorted_.resize(static_cast<std::size_t>(vertex_count));
    std::ifstream input(vertices_path, std::ios::binary);
    input.read(reinterpret_cast<char*>(sorted_.data()),
               static_cast<std::streamsize>(sorted_.size() * sizeof(OriginalVertexId)));
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
      throw std::runtime_error("cannot load sorted vertex mapping");
    }
    std::cerr << "preprocess: ID mapping=sorted-binary-search\n";
  }

  [[nodiscard]] auto allocated_bytes() const -> std::uint64_t {
    return direct_.capacity() * sizeof(DenseVertexId) +
           sorted_.capacity() * sizeof(OriginalVertexId);
  }

  [[nodiscard]] auto map(const OriginalVertexId identifier) const -> DenseVertexId {
    if (!direct_.empty()) {
      const auto signed_index =
          static_cast<std::int64_t>(identifier) - static_cast<std::int64_t>(minimum_id_);
      if (signed_index < 0 || static_cast<std::uint64_t>(signed_index) >= direct_.size()) {
        throw std::runtime_error("identifier is missing from direct ID mapping");
      }
      const auto dense = direct_[static_cast<std::size_t>(signed_index)];
      if (dense == kInvalidDenseVertex) {
        throw std::runtime_error("identifier is missing from direct ID mapping");
      }
      return dense;
    }

    const auto iterator = std::lower_bound(sorted_.begin(), sorted_.end(), identifier);
    if (iterator == sorted_.end() || *iterator != identifier) {
      throw std::runtime_error("identifier is missing from sorted ID mapping");
    }
    return static_cast<DenseVertexId>(iterator - sorted_.begin());
  }

private:
  OriginalVertexId minimum_id_{};
  std::vector<DenseVertexId> direct_;
  std::vector<OriginalVertexId> sorted_;
};

auto disk_edge_less(const DiskEdge& left, const DiskEdge& right) -> bool {
  return std::tie(left.destination, left.source) < std::tie(right.destination, right.source);
}

auto build_edge_runs(const PreprocessOptions& options, const fs::path& session_directory,
                     const IdMapper& mapper, const std::uint64_t working_bytes) -> EdgeRunResult {
  const auto reserved_bytes = mapper.allocated_bytes() + 2ULL * kMiB;
  if (reserved_bytes + 4ULL * kMiB >= working_bytes) {
    throw std::runtime_error("memory budget leaves less than 4 MiB for edge sorting");
  }
  const auto capacity_u64 = (working_bytes - reserved_bytes) / sizeof(DiskEdge);
  const auto capacity = static_cast<std::size_t>(
      std::min<std::uint64_t>(capacity_u64, std::numeric_limits<std::size_t>::max()));

  std::vector<DiskEdge> edges;
  edges.reserve(capacity);
  EdgeRunResult result;

  auto flush = [&]() {
    if (edges.empty()) {
      return;
    }
    const auto path = session_directory / run_name("edges-run", result.runs.size());
    result.maximum_sort_workers =
        std::max(result.maximum_sort_workers,
                 sort_and_write_run(path, edges, options.threads, disk_edge_less));
    result.runs.push_back(path);
    edges.clear();
  };

  EdgeListReader reader(options.input, kInputBufferBytes);
  OriginalEdge edge;
  while (reader.next(edge)) {
    if (edges.size() == edges.capacity()) {
      flush();
    }
    edges.push_back({mapper.map(edge.destination), mapper.map(edge.source)});
  }
  flush();
  result.input_edges = reader.stats().edges;
  result.input_fingerprint = reader.stats().edge_fingerprint;
  return result;
}

struct EdgeHead {
  DiskEdge edge;
  std::size_t run{};
};

struct EdgeHeadGreater {
  auto operator()(const EdgeHead& left, const EdgeHead& right) const -> bool {
    return std::tie(left.edge.destination, left.edge.source, left.run) >
           std::tie(right.edge.destination, right.edge.source, right.run);
  }
};

void merge_edge_run_group(const std::vector<fs::path>& runs, const fs::path& output_path) {
  std::vector<std::unique_ptr<BinaryRunReader<DiskEdge>>> readers;
  readers.reserve(runs.size());
  std::priority_queue<EdgeHead, std::vector<EdgeHead>, EdgeHeadGreater> heap;
  for (std::size_t index = 0; index < runs.size(); ++index) {
    readers.push_back(std::make_unique<BinaryRunReader<DiskEdge>>(runs[index]));
    DiskEdge edge;
    if (readers.back()->next(edge)) {
      heap.push({edge, index});
    }
  }

  BufferedBinaryWriter<DiskEdge> output(output_path);
  std::optional<DiskEdge> previous;
  while (!heap.empty()) {
    const auto head = heap.top();
    heap.pop();
    if (!previous || *previous != head.edge) {
      output.append(head.edge);
      previous = head.edge;
    }

    DiskEdge next_edge;
    if (readers[head.run]->next(next_edge)) {
      heap.push({next_edge, head.run});
    }
  }
  output.close();
}

auto compact_edge_runs(std::vector<fs::path> runs, const fs::path& session_directory)
    -> std::vector<fs::path> {
  std::size_t pass = 0;
  while (runs.size() > kMergeFanIn) {
    std::cerr << "preprocess: compacting edge runs pass=" << pass << " input_runs=" << runs.size()
              << '\n';
    std::vector<fs::path> next_runs;
    next_runs.reserve((runs.size() + kMergeFanIn - 1U) / kMergeFanIn);
    for (std::size_t begin = 0; begin < runs.size(); begin += kMergeFanIn) {
      const auto end = std::min(runs.size(), begin + kMergeFanIn);
      if (end - begin == 1U) {
        next_runs.push_back(runs[begin]);
        continue;
      }

      const std::vector<fs::path> group(runs.begin() + static_cast<std::ptrdiff_t>(begin),
                                        runs.begin() + static_cast<std::ptrdiff_t>(end));
      const auto output =
          session_directory / run_name("edges-merge-" + std::to_string(pass), next_runs.size());
      merge_edge_run_group(group, output);
      remove_merged_runs(group);
      next_runs.push_back(output);
    }
    runs = std::move(next_runs);
    ++pass;
  }
  return runs;
}

class ShardWriter {
public:
  ShardWriter(const fs::path& graph_directory, const std::size_t index,
              const DenseVertexId vertex_begin)
      : file_(run_name("shard", index)), vertex_begin_(vertex_begin),
        output_(graph_directory / file_) {}

  void append(const DiskEdge& edge) {
    output_.append(edge);
    ++edge_count_;
  }

  auto finish(const std::uint64_t vertex_end) -> ShardMetadata {
    output_.close();
    return {file_, vertex_begin_, vertex_end, edge_count_};
  }

  [[nodiscard]] auto edge_count() const noexcept -> EdgeCount { return edge_count_; }

private:
  std::string file_;
  DenseVertexId vertex_begin_{};
  BufferedBinaryWriter<DiskEdge> output_;
  EdgeCount edge_count_{};
};

auto merge_edge_runs(const std::vector<fs::path>& runs, const fs::path& graph_directory,
                     const VertexCount vertex_count, const EdgeCount input_edge_count,
                     const std::size_t requested_shards, std::vector<std::uint32_t>& outdegrees)
    -> MergeResult {
  if (runs.empty()) {
    throw std::invalid_argument("no edge runs to merge");
  }

  std::vector<std::unique_ptr<BinaryRunReader<DiskEdge>>> readers;
  readers.reserve(runs.size());
  std::priority_queue<EdgeHead, std::vector<EdgeHead>, EdgeHeadGreater> heap;
  for (std::size_t index = 0; index < runs.size(); ++index) {
    readers.push_back(std::make_unique<BinaryRunReader<DiskEdge>>(runs[index]));
    DiskEdge edge;
    if (readers.back()->next(edge)) {
      heap.push({edge, index});
    }
  }

  const EdgeCount target_edges =
      std::max<EdgeCount>(1, input_edge_count / requested_shards +
                                 (input_edge_count % requested_shards == 0 ? 0U : 1U));
  MergeResult result;
  std::optional<DiskEdge> previous;
  DenseVertexId range_begin = 0;
  auto shard = std::make_unique<ShardWriter>(graph_directory, 0, range_begin);

  while (!heap.empty()) {
    const auto head = heap.top();
    heap.pop();

    if (!previous || *previous != head.edge) {
      if (head.edge.destination >= vertex_count || head.edge.source >= vertex_count) {
        throw std::runtime_error("edge run contains an out-of-range dense ID");
      }
      if (shard->edge_count() >= target_edges && previous &&
          previous->destination != head.edge.destination &&
          result.shards.size() + 1U < requested_shards) {
        result.shards.push_back(shard->finish(head.edge.destination));
        range_begin = head.edge.destination;
        shard = std::make_unique<ShardWriter>(graph_directory, result.shards.size(), range_begin);
      }

      shard->append(head.edge);
      auto& degree = outdegrees[head.edge.source];
      if (degree == std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("outdegree exceeds uint32 range");
      }
      ++degree;
      if (head.edge.source == head.edge.destination) {
        ++result.self_loops;
      }
      ++result.edges;
      previous = head.edge;
    }

    DiskEdge next_edge;
    if (readers[head.run]->next(next_edge)) {
      heap.push({next_edge, head.run});
    }
  }

  result.shards.push_back(shard->finish(vertex_count));
  return result;
}

void write_outdegrees(const fs::path& path, const std::vector<std::uint32_t>& outdegrees) {
  write_records(path, outdegrees);
}

auto required_merge_bytes(const VertexCount vertex_count, const std::size_t run_count,
                          const std::size_t requested_shards) -> std::uint64_t {
  const auto outdegree_bytes = vertex_count * sizeof(std::uint32_t);
  const auto reader_bytes = static_cast<std::uint64_t>(run_count) * kRunBufferBytes;
  const auto possible_shards = std::min(static_cast<std::uint64_t>(requested_shards), vertex_count);
  const auto shard_metadata_bytes = possible_shards * sizeof(ShardMetadata);
  const auto fixed_bytes =
      static_cast<std::uint64_t>(kOutputBufferBytes) + kMergeRuntimeReserveBytes;
  if (outdegree_bytes > std::numeric_limits<std::uint64_t>::max() - reader_bytes ||
      outdegree_bytes + reader_bytes >
          std::numeric_limits<std::uint64_t>::max() - shard_metadata_bytes ||
      outdegree_bytes + reader_bytes + shard_metadata_bytes >
          std::numeric_limits<std::uint64_t>::max() - fixed_bytes) {
    throw std::runtime_error("preprocessing merge memory estimate overflow");
  }
  return outdegree_bytes + reader_bytes + shard_metadata_bytes + fixed_bytes;
}

} // namespace

void preprocess(const PreprocessOptions& options) {
  require_nonempty_options(options);

  const auto target_parent = options.graph_directory.parent_path().empty()
                                 ? fs::path(".")
                                 : options.graph_directory.parent_path();
  fs::create_directories(target_parent);
  fs::create_directories(options.work_directory);

  const auto suffix = unique_suffix();
  const auto staging_directory =
      target_parent / (options.graph_directory.filename().string() + ".building-" + suffix);
  const auto session_directory = options.work_directory / ("run-" + suffix);
  if (!fs::create_directory(staging_directory)) {
    throw std::runtime_error("cannot create graph staging directory: " +
                             staging_directory.string());
  }
  ScopedDirectoryCleanup staging_cleanup(staging_directory);
  if (!fs::create_directory(session_directory)) {
    throw std::runtime_error("cannot create preprocessing session directory: " +
                             session_directory.string());
  }
  ScopedDirectoryCleanup session_cleanup(session_directory);

  const auto total_start = Clock::now();
  auto phase_start = Clock::now();
  std::cerr << "preprocess: phase=vertex-runs memory_budget_bytes=" << options.memory_budget_bytes
            << '\n';
  auto vertex_runs = build_vertex_runs(options, session_directory);
  std::cerr << "preprocess: vertex_runs=" << vertex_runs.runs.size()
            << " input_edges=" << vertex_runs.input_edges
            << " sort_workers=" << vertex_runs.maximum_sort_workers
            << " seconds=" << elapsed_seconds(phase_start) << '\n';

  phase_start = Clock::now();
  vertex_runs.runs = compact_vertex_runs(std::move(vertex_runs.runs), session_directory);
  const auto vertices_path = staging_directory / kVerticesFileName;
  const auto vertex_count = merge_vertex_runs(vertex_runs.runs, vertices_path);
  std::cerr << "preprocess: vertices=" << vertex_count
            << " seconds=" << elapsed_seconds(phase_start) << '\n';

  phase_start = Clock::now();
  EdgeRunResult edge_runs;
  {
    IdMapper mapper(vertices_path, vertex_count, vertex_runs.minimum_id, vertex_runs.maximum_id,
                    working_memory_limit(options.memory_budget_bytes));
    edge_runs = build_edge_runs(options, session_directory, mapper,
                                working_memory_limit(options.memory_budget_bytes));
  }
  if (edge_runs.input_edges != vertex_runs.input_edges ||
      edge_runs.input_fingerprint != vertex_runs.input_fingerprint) {
    throw std::runtime_error("input edge list changed between preprocessing passes");
  }
  std::cerr << "preprocess: edge_runs=" << edge_runs.runs.size()
            << " sort_workers=" << edge_runs.maximum_sort_workers
            << " seconds=" << elapsed_seconds(phase_start) << '\n';

  phase_start = Clock::now();
  edge_runs.runs = compact_edge_runs(std::move(edge_runs.runs), session_directory);
  if (vertex_count > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("outdegree vector is too large for this host");
  }
  const auto merge_memory =
      required_merge_bytes(vertex_count, edge_runs.runs.size(), options.requested_shards);
  if (merge_memory > options.memory_budget_bytes) {
    throw std::runtime_error("preprocessing merge requires approximately " +
                             std::to_string((merge_memory + kMiB - 1U) / kMiB) +
                             " MiB, exceeding the configured budget");
  }
  std::cerr << "preprocess: phase=edge-merge estimated_memory_bytes=" << merge_memory << '\n';
  std::vector<std::uint32_t> outdegrees(static_cast<std::size_t>(vertex_count), 0);
  auto merge = merge_edge_runs(edge_runs.runs, staging_directory, vertex_count,
                               vertex_runs.input_edges, options.requested_shards, outdegrees);
  write_outdegrees(staging_directory / kOutdegreesFileName, outdegrees);
  outdegrees.clear();
  outdegrees.shrink_to_fit();

  GraphManifest manifest;
  manifest.input_file = fs::absolute(options.input).string();
  manifest.vertex_count = vertex_count;
  manifest.input_edge_count = vertex_runs.input_edges;
  manifest.edge_count = merge.edges;
  manifest.duplicate_edge_count = vertex_runs.input_edges - merge.edges;
  manifest.self_loop_count = merge.self_loops;
  manifest.shards = std::move(merge.shards);
  save_manifest(staging_directory, manifest);
  validate_graph_files(staging_directory, manifest);
  std::cerr << "preprocess: unique_edges=" << manifest.edge_count
            << " duplicates=" << manifest.duplicate_edge_count
            << " shards=" << manifest.shards.size() << " seconds=" << elapsed_seconds(phase_start)
            << '\n';

  fs::rename(staging_directory, options.graph_directory);
  staging_cleanup.keep();
  std::cerr << "preprocess: complete graph_dir=" << options.graph_directory
            << " total_seconds=" << elapsed_seconds(total_start) << '\n';
}

} // namespace omg
