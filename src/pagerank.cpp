#include "omg/pagerank.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "omg/manifest.hpp"
#include "omg/types.hpp"

namespace omg {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kMiB = 1ULL << 20U;
constexpr std::size_t kShardBufferBytes = 128U << 10U;
constexpr std::size_t kOutputBufferBytes = 1U << 20U;
constexpr std::uint64_t kRuntimeReserveBytes = 8ULL << 20U;

class CompensatedSum {
public:
  void add(const long double value) noexcept {
    const long double updated = sum_ + value;
    if (std::abs(sum_) >= std::abs(value)) {
      correction_ += (sum_ - updated) + value;
    } else {
      correction_ += (value - updated) + sum_;
    }
    sum_ = updated;
  }

  [[nodiscard]] auto value() const noexcept -> long double { return sum_ + correction_; }

private:
  long double sum_{};
  long double correction_{};
};

struct ShardIterationStats {
  long double residual_l1{};
  long double rank_sum{};
};

class TemporaryOutput {
public:
  TemporaryOutput(fs::path final_path, fs::path temporary_path)
      : final_path_(std::move(final_path)), temporary_path_(std::move(temporary_path)) {}

  TemporaryOutput(const TemporaryOutput&) = delete;
  auto operator=(const TemporaryOutput&) -> TemporaryOutput& = delete;

  ~TemporaryOutput() {
    if (!committed_) {
      std::error_code ignored;
      fs::remove(temporary_path_, ignored);
    }
  }

  void commit() {
    fs::rename(temporary_path_, final_path_);
    committed_ = true;
  }

private:
  fs::path final_path_;
  fs::path temporary_path_;
  bool committed_{};
};

auto checked_size(const VertexCount count) -> std::size_t {
  if (count == 0 || count > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("vertex count cannot be represented on this host");
  }
  return static_cast<std::size_t>(count);
}

void validate_options(const PageRankOptions& options) {
  if (options.graph_directory.empty() || options.output.empty()) {
    throw std::invalid_argument("graph directory and output path must be set");
  }
  if (options.threads == 0) {
    throw std::invalid_argument("thread count must be positive");
  }
  if (!(options.damping > 0.0 && options.damping < 1.0)) {
    throw std::invalid_argument("damping factor must be strictly between 0 and 1");
  }
  if (options.fixed_iterations && *options.fixed_iterations == 0) {
    throw std::invalid_argument("fixed iteration count must be positive");
  }
  if (!options.fixed_iterations &&
      (options.max_iterations == 0 || !std::isfinite(options.tolerance) ||
       options.tolerance <= 0.0)) {
    throw std::invalid_argument("convergence mode requires positive tolerance and iteration limit");
  }
  if (fs::exists(options.output)) {
    throw std::invalid_argument("output file already exists: " + options.output.string());
  }
  const auto parent = options.output.parent_path();
  if (!parent.empty() && !fs::is_directory(parent)) {
    throw std::invalid_argument("output parent directory does not exist: " + parent.string());
  }
}

template <typename Record>
auto load_raw_vector(const fs::path& path, const std::size_t count) -> std::vector<Record> {
  std::vector<Record> records(count);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open graph file: " + path.string());
  }
  input.read(reinterpret_cast<char*>(records.data()),
             static_cast<std::streamsize>(records.size() * sizeof(Record)));
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    throw std::runtime_error("cannot read graph file exactly: " + path.string());
  }
  return records;
}

auto required_runtime_bytes(const std::size_t vertices, const std::size_t workers)
    -> std::uint64_t {
  const auto vertex_bytes =
      static_cast<std::uint64_t>(vertices) * (2U * sizeof(double) + sizeof(std::uint32_t));
  const auto worker_bytes = static_cast<std::uint64_t>(workers) * kShardBufferBytes;
  if (vertex_bytes >
      std::numeric_limits<std::uint64_t>::max() - worker_bytes - kRuntimeReserveBytes) {
    throw std::runtime_error("PageRank memory estimate overflow");
  }
  return vertex_bytes + worker_bytes + kRuntimeReserveBytes;
}

auto dangling_sum(const std::vector<double>& ranks, const std::vector<std::uint32_t>& outdegrees)
    -> long double {
  CompensatedSum sum;
  for (std::size_t index = 0; index < ranks.size(); ++index) {
    if (outdegrees[index] == 0) {
      sum.add(static_cast<long double>(ranks[index]));
    }
  }
  return sum.value();
}

auto process_shard(const fs::path& graph_directory, const ShardMetadata& shard,
                   const std::vector<double>& ranks, const std::vector<std::uint32_t>& outdegrees,
                   std::vector<double>& next_ranks, const double base, const double damping,
                   std::vector<DiskEdge>& edge_buffer) -> ShardIterationStats {
  const auto begin = static_cast<std::size_t>(shard.vertex_begin);
  const auto end = static_cast<std::size_t>(shard.vertex_end);
  std::fill(next_ranks.begin() + static_cast<std::ptrdiff_t>(begin),
            next_ranks.begin() + static_cast<std::ptrdiff_t>(end), base);

  std::ifstream input(graph_directory / shard.file, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open shard: " + shard.file);
  }

  EdgeCount read_edges = 0;
  std::optional<DiskEdge> previous;
  while (true) {
    input.read(reinterpret_cast<char*>(edge_buffer.data()),
               static_cast<std::streamsize>(edge_buffer.size() * sizeof(DiskEdge)));
    const auto bytes = input.gcount();
    if (bytes == 0) {
      if (!input.eof()) {
        throw std::runtime_error("failed while reading shard: " + shard.file);
      }
      break;
    }
    if (bytes % static_cast<std::streamsize>(sizeof(DiskEdge)) != 0) {
      throw std::runtime_error("truncated edge in shard: " + shard.file);
    }

    const auto count = static_cast<std::size_t>(bytes) / sizeof(DiskEdge);
    for (std::size_t index = 0; index < count; ++index) {
      const auto edge = edge_buffer[index];
      if (edge.destination < shard.vertex_begin || edge.destination >= shard.vertex_end ||
          edge.source >= ranks.size()) {
        throw std::runtime_error("out-of-range edge in shard: " + shard.file);
      }
      if (previous &&
          (edge.destination < previous->destination ||
           (edge.destination == previous->destination && edge.source <= previous->source))) {
        throw std::runtime_error("shard is not strictly sorted: " + shard.file);
      }
      const auto degree = outdegrees[edge.source];
      if (degree == 0) {
        throw std::runtime_error("edge source has zero outdegree in shard: " + shard.file);
      }
      next_ranks[edge.destination] += damping * ranks[edge.source] / static_cast<double>(degree);
      previous = edge;
      ++read_edges;
    }
  }
  if (read_edges != shard.edge_count) {
    throw std::runtime_error("shard edge count changed: " + shard.file);
  }

  CompensatedSum residual;
  CompensatedSum sum;
  for (std::size_t vertex = begin; vertex < end; ++vertex) {
    residual.add(std::abs(static_cast<long double>(next_ranks[vertex]) -
                          static_cast<long double>(ranks[vertex])));
    sum.add(static_cast<long double>(next_ranks[vertex]));
  }
  return {residual.value(), sum.value()};
}

auto run_iteration(const fs::path& graph_directory, const GraphManifest& manifest,
                   const std::vector<double>& ranks, const std::vector<std::uint32_t>& outdegrees,
                   std::vector<double>& next_ranks, const std::size_t requested_threads,
                   const double damping) -> ShardIterationStats {
  const auto dangling = dangling_sum(ranks, outdegrees);
  const double vertices = static_cast<double>(manifest.vertex_count);
  const double base =
      (1.0 - damping) / vertices + damping * static_cast<double>(dangling) / vertices;

  const auto worker_count = std::min(requested_threads, manifest.shards.size());
  std::vector<ShardIterationStats> shard_stats(manifest.shards.size());
  std::atomic<std::size_t> next_job{0};
  std::atomic<bool> stop{false};
  std::exception_ptr failure;
  std::mutex failure_mutex;

  auto worker = [&]() {
    try {
      std::vector<DiskEdge> edge_buffer(kShardBufferBytes / sizeof(DiskEdge));
      while (!stop.load(std::memory_order_relaxed)) {
        const auto job = next_job.fetch_add(1, std::memory_order_relaxed);
        if (job >= manifest.shards.size()) {
          break;
        }
        shard_stats[job] = process_shard(graph_directory, manifest.shards[job], ranks, outdegrees,
                                         next_ranks, base, damping, edge_buffer);
      }
    } catch (...) {
      stop.store(true, std::memory_order_relaxed);
      std::lock_guard lock(failure_mutex);
      if (!failure) {
        failure = std::current_exception();
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    workers.emplace_back(worker);
  }
  for (auto& thread : workers) {
    thread.join();
  }
  if (failure) {
    std::rethrow_exception(failure);
  }

  CompensatedSum residual;
  CompensatedSum rank_sum;
  for (const auto& stats : shard_stats) {
    residual.add(stats.residual_l1);
    rank_sum.add(stats.rank_sum);
  }
  return {residual.value(), rank_sum.value()};
}

void append_number(std::string& buffer, const auto value) {
  char encoded[96];
  const auto result = std::to_chars(std::begin(encoded), std::end(encoded), value);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("cannot format PageRank output");
  }
  buffer.append(encoded, result.ptr);
}

void append_rank(std::string& buffer, const double value) {
  char encoded[96];
  const auto result =
      std::to_chars(std::begin(encoded), std::end(encoded), value, std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("cannot format PageRank value");
  }
  buffer.append(encoded, result.ptr);
}

void write_output(const fs::path& graph_directory, const fs::path& output_path,
                  const std::vector<double>& ranks) {
  const auto temporary_path = output_path.string() + ".tmp";
  if (fs::exists(temporary_path)) {
    throw std::runtime_error("temporary output already exists: " + temporary_path);
  }
  TemporaryOutput output_guard(output_path, temporary_path);

  std::ifstream vertices(graph_directory / kVerticesFileName, std::ios::binary);
  std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
  if (!vertices || !output) {
    throw std::runtime_error("cannot open PageRank input/output files");
  }

  std::vector<OriginalVertexId> identifiers(1U << 14U);
  std::string text;
  text.reserve(kOutputBufferBytes + 256U);
  text.append("vertex,rank\n");

  std::size_t rank_index = 0;
  while (rank_index < ranks.size()) {
    const auto wanted = std::min(identifiers.size(), ranks.size() - rank_index);
    vertices.read(reinterpret_cast<char*>(identifiers.data()),
                  static_cast<std::streamsize>(wanted * sizeof(OriginalVertexId)));
    if (vertices.gcount() != static_cast<std::streamsize>(wanted * sizeof(OriginalVertexId))) {
      throw std::runtime_error("vertices.bin ended while writing PageRank output");
    }
    for (std::size_t index = 0; index < wanted; ++index) {
      append_number(text, identifiers[index]);
      text.push_back(',');
      append_rank(text, ranks[rank_index++]);
      text.push_back('\n');
      if (text.size() >= kOutputBufferBytes) {
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        text.clear();
      }
    }
  }
  if (!text.empty()) {
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  output.close();
  if (!output) {
    throw std::runtime_error("failed while writing PageRank output");
  }
  output_guard.commit();
}

} // namespace

auto run_pagerank(const PageRankOptions& options) -> PageRankResult {
  validate_options(options);
  const auto manifest = load_manifest(options.graph_directory);
  validate_graph_files(options.graph_directory, manifest);
  const auto vertex_count = checked_size(manifest.vertex_count);
  const auto worker_count = std::min(options.threads, manifest.shards.size());
  const auto required_bytes = required_runtime_bytes(vertex_count, worker_count);
  if (required_bytes > options.memory_budget_bytes) {
    throw std::runtime_error("PageRank requires approximately " +
                             std::to_string((required_bytes + kMiB - 1U) / kMiB) +
                             " MiB, exceeding the configured budget");
  }

  std::cerr << "pagerank: vertices=" << manifest.vertex_count << " edges=" << manifest.edge_count
            << " shards=" << manifest.shards.size() << " workers=" << worker_count
            << " estimated_memory_bytes=" << required_bytes << '\n';

  const auto outdegrees =
      load_raw_vector<std::uint32_t>(options.graph_directory / kOutdegreesFileName, vertex_count);
  const double initial_rank = 1.0 / static_cast<double>(manifest.vertex_count);
  std::vector<double> ranks(vertex_count, initial_rank);
  std::vector<double> next_ranks(vertex_count, 0.0);

  const auto iteration_limit = options.fixed_iterations.value_or(options.max_iterations);
  PageRankResult result;
  bool converged = false;
  for (std::size_t iteration = 1; iteration <= iteration_limit; ++iteration) {
    const auto start = Clock::now();
    const auto stats = run_iteration(options.graph_directory, manifest, ranks, outdegrees,
                                     next_ranks, options.threads, options.damping);
    ranks.swap(next_ranks);
    result.iterations = iteration;
    result.residual_l1 = static_cast<double>(stats.residual_l1);
    result.rank_sum = static_cast<double>(stats.rank_sum);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const double throughput =
        seconds == 0.0 ? 0.0 : static_cast<double>(manifest.edge_count) / seconds;
    std::cerr << std::setprecision(17) << "pagerank: iteration=" << iteration
              << " residual_l1=" << result.residual_l1 << " rank_sum=" << result.rank_sum
              << " seconds=" << seconds << " edges_per_second=" << throughput << '\n';

    if (!options.fixed_iterations && result.residual_l1 <= options.tolerance) {
      converged = true;
      break;
    }
  }

  if (!options.fixed_iterations && !converged) {
    throw std::runtime_error("PageRank did not converge within " +
                             std::to_string(options.max_iterations) +
                             " iterations; residual=" + std::to_string(result.residual_l1));
  }
  if (!std::isfinite(result.rank_sum) || std::abs(result.rank_sum - 1.0) > 1e-10) {
    throw std::runtime_error("PageRank invariant failed: rank sum is " +
                             std::to_string(result.rank_sum));
  }

  write_output(options.graph_directory, options.output, ranks);
  return result;
}

} // namespace omg
