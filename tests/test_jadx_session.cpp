#include "TestHarness.h"

#include <filesystem>
#include <fstream>

#include "collect/JadxSession.h"

using namespace em;
namespace fs = std::filesystem;

namespace {

// Same file, whatever symlinks (macOS /var -> /private/var) the paths went through.
bool same(const std::optional<fs::path>& a, const fs::path& b) {
    std::error_code ec;
    return a.has_value() && fs::equivalent(*a, b, ec);
}

std::string frame(int id, const char* status, std::string_view payload) {
    return "@" + std::to_string(id) + " " + status + " " + std::to_string(payload.size()) +
           "\n" + std::string{payload};
}

}  // namespace

TEST_CASE("JadxFrameReader reassembles frames split across reads") {
    const std::string bytes =
        frame(3, "part", "a\nb") + frame(3, "ok", "") + frame(7, "err", "boom");
    JadxFrameReader reader;
    std::vector<JadxFrame> frames;
    for (char c : bytes) {  // the worst chunking: one byte at a time
        reader.feed(std::string_view{&c, 1});
        while (auto f = reader.next()) frames.push_back(std::move(*f));
    }
    REQUIRE(frames.size() == std::size_t{3});
    CHECK_EQ(frames[0].id, 3);
    CHECK(frames[0].status == JadxFrame::Status::Part);
    CHECK(!frames[0].final());
    CHECK_EQ(frames[0].payload, std::string{"a\nb"});
    CHECK(frames[1].status == JadxFrame::Status::Ok);
    CHECK(frames[1].payload.empty());
    CHECK(frames[2].status == JadxFrame::Status::Err);
    CHECK_EQ(frames[2].payload, std::string{"boom"});
}

TEST_CASE("JadxFrameReader skips a stray line and keeps going") {
    JadxFrameReader reader;
    reader.feed("SLF4J: something\n" + frame(1, "ok", "x"));
    auto stray = reader.next();
    REQUIRE(stray.has_value());
    CHECK_EQ(stray->id, -1);
    auto real = reader.next();
    REQUIRE(real.has_value());
    CHECK_EQ(real->id, 1);
    CHECK_EQ(real->payload, std::string{"x"});
}

TEST_CASE("jadxRequestLine keeps arguments on one line") {
    CHECK_EQ(jadxRequestLine(5, "search", {"i", "", "a\tb\nc"}),
             std::string{"5\tsearch\ti\t\ta b c\n"});
}

TEST_CASE("parseJadxClasses reads kinds and names") {
    const auto classes =
        parseJadxClasses("o.a\tcom.app.Main\t\no.b\to.b\tin\no.c\t\t\nno-tab\n");
    REQUIRE(classes.size() == std::size_t{3});
    CHECK_EQ(classes[0].raw, std::string{"o.a"});
    CHECK_EQ(classes[0].name, std::string{"com.app.Main"});
    CHECK_EQ(std::string{classes[0].simpleName()}, std::string{"Main"});
    CHECK_EQ(std::string{classes[0].packageName()}, std::string{"com.app"});
    CHECK_EQ(classes[1].kind, 'i');
    CHECK(classes[1].noCode);
    CHECK_EQ(classes[2].name, std::string{"o.c"});  // no display name: the raw one
}

TEST_CASE("parseJadxCode splits code and sorted annotations") {
    const std::string code = "class A { B b; }";
    const std::string payload =
        std::to_string(code.size()) + "\n" + code + "12\tf\n6\tC\n10\tc\n999\tm\nx\ty\n";
    auto parsed = parseJadxCode(payload);
    REQUIRE(static_cast<bool>(parsed));
    CHECK_EQ(parsed->text, code);
    REQUIRE(parsed->refs.size() == std::size_t{3});  // 999 is past the end
    CHECK_EQ(parsed->refs[0].pos, 6U);
    CHECK(parsed->refs[0].declaration());
    CHECK_EQ(parsed->refs[1].kind, 'c');
    CHECK_EQ(parsed->refs[2].kind, 'f');
    CHECK(!static_cast<bool>(parseJadxCode("100\nshort")));
}

TEST_CASE("parseJadxHits reads hits, progress and usage summaries") {
    const auto chunk = parseJadxHits(
        "o.a\tcom.app.Main\t42\tString s = "
        "getString(R.string.x);\nP\t10\t291\nL\nN\tcom.app.Main.run\nT\t1\n");
    REQUIRE(chunk.hits.size() == std::size_t{1});
    CHECK_EQ(chunk.hits[0].raw, std::string{"o.a"});
    CHECK_EQ(chunk.hits[0].pos, 42U);
    CHECK_EQ(chunk.hits[0].line, std::string{"String s = getString(R.string.x);"});
    CHECK_EQ(chunk.done, 10);
    CHECK_EQ(chunk.total, 291);
    CHECK(chunk.limited);
    CHECK_EQ(chunk.node, std::string{"com.app.Main.run"});
    CHECK_EQ(chunk.usageCount, 1);
}

TEST_CASE("parseJadxResolved and parseJadxResource") {
    auto r = parseJadxResolved("o.b\t840\to.b.run\n");
    REQUIRE(static_cast<bool>(r));
    CHECK_EQ(r->raw, std::string{"o.b"});
    CHECK_EQ(r->pos, 840U);
    CHECK(!static_cast<bool>(parseJadxResolved("nothing")));

    auto text = parseJadxResource("t\n<manifest/>\n");
    CHECK(text.kind == JadxResourceContent::Kind::Text);
    CHECK_EQ(text.text, std::string{"<manifest/>\n"});
    auto list = parseJadxResource("l\nres/values/strings.xml\nres/values/colors.xml\n");
    CHECK(list.kind == JadxResourceContent::Kind::List);
    CHECK_EQ(list.children.size(), std::size_t{2});
    auto binary = parseJadxResource("b\t1234\n");
    CHECK(binary.kind == JadxResourceContent::Kind::Binary);
    CHECK_EQ(binary.size, std::uint64_t{1234});
}

TEST_CASE("jadxJarIn finds the jar in a release folder, a Homebrew keg and a wrapper script") {
    const fs::path root = fs::temp_directory_path() / "emeron_jadx_test";
    fs::remove_all(root);
    fs::create_directories(root / "release" / "lib");
    fs::create_directories(root / "release" / "bin");
    std::ofstream{root / "release" / "lib" / "jadx-1.5.3-all.jar"} << "jar";
    std::ofstream{root / "release" / "bin" / "jadx"} << "#!/bin/sh\n";
    fs::create_directories(root / "keg" / "libexec" / "lib");
    fs::create_directories(root / "keg" / "libexec" / "bin");
    std::ofstream{root / "keg" / "libexec" / "lib" / "jadx-1.5.3-all.jar"} << "jar";
    std::ofstream{root / "wrapper"}
        << "#!/bin/bash\nexec \"" + (root / "keg" / "libexec" / "bin" / "jadx").string() +
               "\" \"$@\"\n";

    CHECK(same(jadxJarIn(root / "release"), root / "release" / "lib" / "jadx-1.5.3-all.jar"));
    CHECK(same(jadxJarIn(root / "release" / "bin" / "jadx"),
               root / "release" / "lib" / "jadx-1.5.3-all.jar"));
    CHECK(
        same(jadxJarIn(root / "keg"), root / "keg" / "libexec" / "lib" / "jadx-1.5.3-all.jar"));
    CHECK(same(jadxJarIn(root / "wrapper"),
               root / "keg" / "libexec" / "lib" / "jadx-1.5.3-all.jar"));
    CHECK(!jadxJarIn(root / "nowhere").has_value());
    fs::remove_all(root);
}
