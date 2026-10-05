#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/IniFile.h"

using namespace em;

TEST_CASE("IniFile parses key = value with surrounding space") {
    const IniFile file = IniFile::parse("theme = light\n  spaced   =   value here  \n");

    CHECK_EQ(file.getOr("theme", "?"), std::string{"light"});
    CHECK_EQ(file.getOr("spaced", "?"), std::string{"value here"});
    CHECK_EQ(file.size(), std::size_t{2});
}

TEST_CASE("IniFile skips comments and blank lines") {
    const IniFile file = IniFile::parse(
        "# a comment\n"
        "; another style\n"
        "\n"
        "theme = dark\n"
        "   \n");

    CHECK_EQ(file.size(), std::size_t{1});
    CHECK_EQ(file.getOr("theme", "?"), std::string{"dark"});
}

TEST_CASE("IniFile skips unparseable lines rather than failing") {
    // A hand-edited or half-written file must still load what it can.
    const IniFile file = IniFile::parse("garbage with no equals\ntheme = light\n= novalue\n");

    CHECK_EQ(file.size(), std::size_t{1});
    CHECK_EQ(file.getOr("theme", "?"), std::string{"light"});
}

TEST_CASE("IniFile keeps the last value for a duplicated key") {
    const IniFile file = IniFile::parse("theme = dark\ntheme = light\n");
    CHECK_EQ(file.getOr("theme", "?"), std::string{"light"});
}

TEST_CASE("IniFile handles a value containing an equals sign") {
    const IniFile file = IniFile::parse("expr = a=b\n");
    CHECK_EQ(file.getOr("expr", "?"), std::string{"a=b"});
}

TEST_CASE("IniFile returns the fallback for a missing key") {
    const IniFile file = IniFile::parse("");
    CHECK(file.empty());
    CHECK(!file.get("nothing").has_value());
    CHECK_EQ(file.getOr("nothing", "fallback"), std::string{"fallback"});
    CHECK(file.getBool("nothing", true));
    CHECK_EQ(file.getInt("nothing", 42), 42LL);
}

TEST_CASE("IniFile reads the usual spellings of a boolean") {
    const IniFile file = IniFile::parse(
        "a = true\nb = 1\nc = YES\nd = on\ne = false\nf = 0\ng = No\nh = OFF\ni = maybe\n");

    for (const char* key : {"a", "b", "c", "d"}) CHECK(file.getBool(key, false));
    for (const char* key : {"e", "f", "g", "h"}) CHECK(!file.getBool(key, true));
    // An unrecognised value falls back rather than guessing.
    CHECK(file.getBool("i", true));
    CHECK(!file.getBool("i", false));
}

TEST_CASE("IniFile reads integers and falls back on junk") {
    const IniFile file = IniFile::parse("n = 1800\nneg = -5\njunk = abc\n");
    CHECK_EQ(file.getInt("n", 0), 1800LL);
    CHECK_EQ(file.getInt("neg", 0), -5LL);
    CHECK_EQ(file.getInt("junk", 7), 7LL);
}

TEST_CASE("IniFile setters overwrite and typed setters round-trip") {
    IniFile file;
    file.set("theme", "dark");
    file.set("theme", "light");
    file.setBool("follow", true);
    file.setInt("history", 1800);

    CHECK_EQ(file.getOr("theme", "?"), std::string{"light"});
    CHECK(file.getBool("follow", false));
    CHECK_EQ(file.getInt("history", 0), 1800LL);

    CHECK(file.contains("follow"));
    file.remove("follow");
    CHECK(!file.contains("follow"));
    file.remove("not-there");  // must not throw or corrupt
    CHECK_EQ(file.size(), std::size_t{2});
}

TEST_CASE("IniFile serialize round-trips and is key-ordered") {
    IniFile original;
    original.set("zebra", "last");
    original.set("alpha", "first");
    original.set("theme", "light");

    const std::string text = original.serialize();
    // Ordered output keeps a committed or diffed file stable.
    CHECK(text.find("alpha") < text.find("theme"));
    CHECK(text.find("theme") < text.find("zebra"));

    const IniFile reparsed = IniFile::parse(text);
    CHECK_EQ(reparsed.size(), original.size());
    CHECK_EQ(reparsed.getOr("theme", "?"), std::string{"light"});
    CHECK_EQ(reparsed.getOr("alpha", "?"), std::string{"first"});
}

TEST_CASE("IniFile load treats a missing file as empty, not an error") {
    // That is the correct state for a first run.
    const auto path = std::filesystem::temp_directory_path() /
                      "emeron-test-does-not-exist-9f3a.ini";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto loaded = IniFile::load(path);
    CHECK(loaded.hasValue());
    if (loaded) CHECK(loaded.value().empty());
}

TEST_CASE("IniFile save then load round-trips through the filesystem") {
    const auto path = std::filesystem::temp_directory_path() /
                      "emeron-test-prefs-7c21" / "emeron.ini";
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);

    IniFile written;
    written.set("theme", "light");
    written.setInt("history", 600);

    // save() creates the parent directory, so a fresh workspace works.
    auto saved = written.save(path);
    CHECK(saved.hasValue());
    if (!saved) return;
    CHECK(std::filesystem::exists(path, ec));

    auto loaded = IniFile::load(path);
    CHECK(loaded.hasValue());
    if (loaded) {
        CHECK_EQ(loaded.value().getOr("theme", "?"), std::string{"light"});
        CHECK_EQ(loaded.value().getInt("history", 0), 600LL);
    }

    std::filesystem::remove_all(path.parent_path(), ec);
}
