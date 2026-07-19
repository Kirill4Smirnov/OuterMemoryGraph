#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "omg/pagerank.hpp"
#include "omg/preprocess.hpp"

namespace {

using Options = std::unordered_map<std::string, std::string>;

void print_help(std::ostream& output) {
  output << R"(Outer Memory Graph

Usage:
  omg preprocess --input FILE --graph-dir DIR [options]
  omg pagerank  --graph-dir DIR --output FILE [options]

Preprocess options:
  --work-dir DIR       Temporary run directory (default: DIR.work)
  --memory-mb N        Memory budget in MiB (default: 128)
  --threads N          CPU count used for shard planning (default: hardware concurrency)
  --shards N           Requested destination shards (default: 4 * threads)

PageRank options:
  --memory-mb N        Memory budget in MiB (default: 128)
  --threads N          Worker threads (default: hardware concurrency)
  --damping X          Damping factor (default: 0.85)
  --iterations N       Run exactly N iterations
  --tolerance X        L1 stopping tolerance (default: 1e-8)
  --max-iterations N   Iteration limit in convergence mode (default: 100)
)";
}

auto parse_options(const int argc, char** argv, const int begin) -> Options {
  Options options;
  for (int index = begin; index < argc; index += 2) {
    const std::string_view name(argv[index]);
    if (!name.starts_with("--") || index + 1 >= argc) {
      throw std::invalid_argument("expected --option VALUE pairs");
    }
    const auto [iterator, inserted] =
        options.emplace(std::string(name.substr(2)), std::string(argv[index + 1]));
    if (!inserted) {
      throw std::invalid_argument("duplicate option: --" + iterator->first);
    }
  }
  return options;
}

auto require_option(const Options& options, const std::string& name) -> std::string {
  const auto iterator = options.find(name);
  if (iterator == options.end()) {
    throw std::invalid_argument("missing required option: --" + name);
  }
  return iterator->second;
}

auto option_or(const Options& options, const std::string& name, std::string fallback)
    -> std::string {
  const auto iterator = options.find(name);
  return iterator == options.end() ? std::move(fallback) : iterator->second;
}

void reject_unknown_options(const Options& options,
                            const std::initializer_list<std::string_view> allowed_options) {
  const std::unordered_set<std::string_view> allowed(allowed_options);
  for (const auto& [name, value] : options) {
    static_cast<void>(value);
    if (!allowed.contains(name)) {
      throw std::invalid_argument("unknown option: --" + name);
    }
  }
}

template <typename Number>
auto parse_number(const std::string& text, const std::string& option_name) -> Number {
  Number value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid value for --" + option_name + ": " + text);
  }
  return value;
}

auto parse_double(const std::string& text, const std::string& option_name) -> double {
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size()) {
    throw std::invalid_argument("invalid value for --" + option_name + ": " + text);
  }
  return value;
}

auto default_threads() -> std::size_t {
  const auto detected = std::thread::hardware_concurrency();
  return detected == 0 ? 1 : static_cast<std::size_t>(detected);
}

auto memory_bytes(const Options& options) -> std::uint64_t {
  const auto mib = parse_number<std::uint64_t>(option_or(options, "memory-mb", "128"), "memory-mb");
  if (mib == 0 || mib > (std::numeric_limits<std::uint64_t>::max() >> 20U)) {
    throw std::invalid_argument("--memory-mb is out of range");
  }
  return mib << 20U;
}

void run_preprocess_command(const Options& options) {
  reject_unknown_options(options,
                         {"input", "graph-dir", "work-dir", "memory-mb", "threads", "shards"});
  omg::PreprocessOptions config;
  config.input = require_option(options, "input");
  config.graph_directory = require_option(options, "graph-dir");
  config.work_directory = option_or(options, "work-dir", config.graph_directory.string() + ".work");
  config.memory_budget_bytes = memory_bytes(options);
  config.threads = parse_number<std::size_t>(
      option_or(options, "threads", std::to_string(default_threads())), "threads");
  if (config.threads > std::numeric_limits<std::size_t>::max() / 4U) {
    throw std::invalid_argument("--threads is out of range");
  }
  config.requested_shards = parse_number<std::size_t>(
      option_or(options, "shards", std::to_string(config.threads * 4U)), "shards");
  omg::preprocess(config);
}

void run_pagerank_command(const Options& options) {
  reject_unknown_options(options, {"graph-dir", "output", "memory-mb", "threads", "damping",
                                   "iterations", "tolerance", "max-iterations"});
  omg::PageRankOptions config;
  config.graph_directory = require_option(options, "graph-dir");
  config.output = require_option(options, "output");
  config.memory_budget_bytes = memory_bytes(options);
  config.threads = parse_number<std::size_t>(
      option_or(options, "threads", std::to_string(default_threads())), "threads");
  config.damping = parse_double(option_or(options, "damping", "0.85"), "damping");
  config.max_iterations =
      parse_number<std::size_t>(option_or(options, "max-iterations", "100"), "max-iterations");
  config.tolerance = parse_double(option_or(options, "tolerance", "1e-8"), "tolerance");
  if (const auto iterator = options.find("iterations"); iterator != options.end()) {
    config.fixed_iterations = parse_number<std::size_t>(iterator->second, "iterations");
  }

  const auto result = omg::run_pagerank(config);
  std::cout << "iterations=" << result.iterations << " residual_l1=" << result.residual_l1
            << " rank_sum=" << result.rank_sum << '\n';
}

} // namespace

int main(const int argc, char** argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
      print_help(std::cout);
      return argc < 2 ? 1 : 0;
    }

    const std::string_view command(argv[1]);
    const auto options = parse_options(argc, argv, 2);
    if (command == "preprocess") {
      run_preprocess_command(options);
    } else if (command == "pagerank") {
      run_pagerank_command(options);
    } else {
      throw std::invalid_argument("unknown command: " + std::string(command));
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "error: " << exception.what() << '\n';
    return 2;
  }
}
