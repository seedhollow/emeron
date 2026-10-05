#include "TestHarness.h"

#include "core/RingBuffer.h"
#include "core/StringUtil.h"

using namespace em;

TEST_CASE("trim handles whitespace and empty input") {
    CHECK_EQ(std::string{trim("  hello  ")}, std::string{"hello"});
    CHECK_EQ(std::string{trim("\t\r\nvalue\n")}, std::string{"value"});
    CHECK(trim("   ").empty());
    CHECK(trim("").empty());
}

TEST_CASE("tokenize collapses runs of whitespace") {
    const auto fields = tokenize("cpu   1234\t5678  9");
    CHECK_EQ(fields.size(), std::size_t{4});
    CHECK_EQ(std::string{fields[0]}, std::string{"cpu"});
    CHECK_EQ(std::string{fields[3]}, std::string{"9"});
}

TEST_CASE("splitLines tolerates CRLF and a trailing newline") {
    const auto lines = splitLines("a\r\nb\nc\n");
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK_EQ(std::string{lines[0]}, std::string{"a"});
    CHECK_EQ(std::string{lines[1]}, std::string{"b"});
    CHECK_EQ(std::string{lines[2]}, std::string{"c"});
}

TEST_CASE("split can keep or drop empty fields") {
    CHECK_EQ(split("a,,b", ',', true).size(), std::size_t{3});
    CHECK_EQ(split("a,,b", ',', false).size(), std::size_t{2});
}

TEST_CASE("parseNumber ignores units and surrounding space") {
    CHECK_EQ(parseNumber<int>("  42  ").value_or(-1), 42);
    CHECK_EQ(parseNumber<int>("+7").value_or(-1), 7);
    CHECK_NEAR(parseNumber<double>("16227664 kB").value_or(-1.0), 16227664.0, 0.001);
    CHECK_NEAR(parseNumber<double>("-3.5").value_or(0.0), -3.5, 1e-9);
    CHECK(!parseNumber<int>("abc").has_value());
    CHECK(!parseNumber<int>("").has_value());
}

TEST_CASE("fieldAfter extracts key: value and key=value") {
    CHECK_EQ(std::string{fieldAfter("  level: 87", "level").value_or("")}, std::string{"87"});
    CHECK_EQ(std::string{fieldAfter("Threads:\t42", "Threads").value_or("")},
             std::string{"42"});
    CHECK_EQ(std::string{fieldAfter("minRate=12.50Hz", "minRate").value_or("")},
             std::string{"12.50Hz"});
    CHECK(!fieldAfter("nothing here", "missing").has_value());
}

TEST_CASE("stripPrefix only matches at the start") {
    CHECK_EQ(std::string{stripPrefix("package:com.example", "package:").value_or("")},
             std::string{"com.example"});
    CHECK(!stripPrefix("x package:y", "package:").has_value());
}

TEST_CASE("containsIgnoreCase is case insensitive both ways") {
    CHECK(containsIgnoreCase("Permission Denied", "permission denied"));
    CHECK(containsIgnoreCase("abc", ""));
    CHECK(!containsIgnoreCase("abc", "abcd"));
}

TEST_CASE("shellQuote survives embedded single quotes") {
    CHECK_EQ(shellQuote("simple"), std::string{"'simple'"});
    CHECK_EQ(shellQuote("it's"), std::string{"'it'\\''s'"});
}

TEST_CASE("format grows past the stack buffer") {
    CHECK_EQ(format("%d-%s", 7, "x"), std::string{"7-x"});
    const std::string long_ = format("%s", std::string(2000, 'a').c_str());
    CHECK_EQ(long_.size(), std::size_t{2000});
}

TEST_CASE("humanBytes picks a sensible unit") {
    CHECK_EQ(humanBytes(512.0), std::string{"512 B"});
    CHECK_EQ(humanBytes(1536.0), std::string{"1.5 KiB"});
    CHECK_EQ(humanBytes(5.0 * 1024 * 1024), std::string{"5.0 MiB"});
}

TEST_CASE("RingBuffer wraps and reports an ImPlot offset") {
    RingBuffer<double> ring{4};
    for (int i = 1; i <= 4; ++i) ring.push(static_cast<double>(i));

    CHECK_EQ(ring.size(), std::size_t{4});
    CHECK(ring.full());
    CHECK_EQ(ring.plotOffset(), 0);
    CHECK_NEAR(ring[0], 1.0, 1e-9);
    CHECK_NEAR(ring.back(), 4.0, 1e-9);

    ring.push(5.0);
    CHECK_EQ(ring.size(), std::size_t{4});
    CHECK_EQ(ring.plotOffset(), 1);
    // Oldest-first indexing must now start at 2.
    CHECK_NEAR(ring[0], 2.0, 1e-9);
    CHECK_NEAR(ring[3], 5.0, 1e-9);
}

TEST_CASE("RingBuffer preserves recent samples when resized") {
    RingBuffer<double> ring{3};
    ring.push(1.0);
    ring.push(2.0);
    ring.push(3.0);
    ring.reserveCapacity(5);

    CHECK_EQ(ring.capacity(), std::size_t{5});
    CHECK_EQ(ring.size(), std::size_t{3});
    CHECK_NEAR(ring[0], 1.0, 1e-9);
    CHECK_NEAR(ring[2], 3.0, 1e-9);

    // Shrinking keeps the newest.
    ring.reserveCapacity(2);
    CHECK_EQ(ring.size(), std::size_t{2});
    CHECK_NEAR(ring[0], 2.0, 1e-9);
    CHECK_NEAR(ring[1], 3.0, 1e-9);
}

TEST_CASE("percentileInPlace interpolates") {
    std::vector<double> values{1.0, 2.0, 3.0, 4.0};
    CHECK_NEAR(percentileInPlace(values, 0.0), 1.0, 1e-9);
    std::vector<double> again{1.0, 2.0, 3.0, 4.0};
    CHECK_NEAR(percentileInPlace(again, 100.0), 4.0, 1e-9);
    std::vector<double> mid{1.0, 2.0, 3.0, 4.0};
    CHECK_NEAR(percentileInPlace(mid, 50.0), 2.5, 1e-9);

    std::vector<double> empty;
    CHECK_NEAR(percentileInPlace(empty, 50.0), 0.0, 1e-9);
}
