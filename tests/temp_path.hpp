#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace omg::test {

inline auto mutable_template(const std::string_view prefix) -> std::vector<char> {
  const auto pattern =
      (std::filesystem::temp_directory_path() / (std::string(prefix) + "-XXXXXX")).string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  return writable;
}

inline auto unique_temp_directory(const std::string_view prefix) -> std::filesystem::path {
  auto pattern = mutable_template(prefix);
  const char* created = ::mkdtemp(pattern.data());
  if (created == nullptr) {
    throw std::system_error(errno, std::generic_category(), "cannot create temporary directory");
  }
  return created;
}

inline auto unique_temp_file(const std::string_view prefix) -> std::filesystem::path {
  auto pattern = mutable_template(prefix);
  const int descriptor = ::mkstemp(pattern.data());
  if (descriptor == -1) {
    throw std::system_error(errno, std::generic_category(), "cannot create temporary file");
  }
  if (::close(descriptor) == -1) {
    const auto error = errno;
    std::filesystem::remove(pattern.data());
    throw std::system_error(error, std::generic_category(), "cannot close temporary file");
  }
  return pattern.data();
}

} // namespace omg::test
