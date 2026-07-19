#include <cstdint>
#include <filesystem>
#include <fstream>

#include <catch2/catch_all.hpp>

#include "omg/input.hpp"
#include "temp_path.hpp"

namespace {

class TemporaryFile {
public:
  explicit TemporaryFile(const std::string_view contents) {
    path_ = omg::test::unique_temp_file("omg-input-test");
    std::ofstream output(path_);
    output << contents;
  }

  ~TemporaryFile() { std::filesystem::remove(path_); }
  [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

private:
  std::filesystem::path path_;
};

auto edge_fingerprint(const std::filesystem::path& path) -> std::uint64_t {
  omg::EdgeListReader reader(path, 128);
  omg::OriginalEdge edge;
  while (reader.next(edge)) {
  }
  return reader.stats().edge_fingerprint;
}

} // namespace

TEST_CASE("CSV header and signed int32 identifiers are parsed") {
  const TemporaryFile input("from,to\n-12,42\n42,-7\n");
  omg::EdgeListReader reader(input.path(), 128);

  omg::OriginalEdge edge;
  REQUIRE(reader.next(edge));
  CHECK(edge == omg::OriginalEdge{-12, 42});
  REQUIRE(reader.next(edge));
  CHECK(edge == omg::OriginalEdge{42, -7});
  CHECK_FALSE(reader.next(edge));
  CHECK(reader.stats().edges == 2);
  CHECK(reader.stats().headers == 1);
}

TEST_CASE("SNAP comments and whitespace separated edges are parsed") {
  const TemporaryFile input("# metadata\n# FromNodeId ToNodeId\n1\t2\n  3  4  \n");
  omg::EdgeListReader reader(input.path(), 128);

  omg::OriginalEdge edge;
  REQUIRE(reader.next(edge));
  CHECK(edge == omg::OriginalEdge{1, 2});
  REQUIRE(reader.next(edge));
  CHECK(edge == omg::OriginalEdge{3, 4});
  CHECK_FALSE(reader.next(edge));
  CHECK(reader.stats().comments == 2);
}

TEST_CASE("Malformed input reports the line number") {
  const TemporaryFile input("from,to\n1,2\nnot-an-edge\n");
  omg::EdgeListReader reader(input.path(), 128);
  omg::OriginalEdge edge;
  REQUIRE(reader.next(edge));
  CHECK_THROWS_WITH(reader.next(edge), Catch::Matchers::ContainsSubstring(":3:"));
}

TEST_CASE("Edge fingerprint detects semantic input changes") {
  const TemporaryFile first("from,to\n1,2\n2,3\n");
  const TemporaryFile identical("# comment\n1 2\n2 3\n");
  const TemporaryFile changed("from,to\n1,2\n3,2\n");

  CHECK(edge_fingerprint(first.path()) == edge_fingerprint(identical.path()));
  CHECK(edge_fingerprint(first.path()) != edge_fingerprint(changed.path()));
}
