#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "collect/FileSystemBrowser.h"

using namespace em;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

TEST_CASE("normalizeDevicePath strips trailing slashes but keeps root") {
    CHECK_EQ(normalizeDevicePath("/sdcard/"), std::string{"/sdcard"});
    CHECK_EQ(normalizeDevicePath("/sdcard///"), std::string{"/sdcard"});
    CHECK_EQ(normalizeDevicePath("/"), std::string{"/"});
    CHECK_EQ(normalizeDevicePath("///"), std::string{"/"});
    CHECK_EQ(normalizeDevicePath(""), std::string{"/"});
}

TEST_CASE("deviceBaseName returns the last component") {
    CHECK_EQ(deviceBaseName("/sdcard/Download/a.txt"), std::string{"a.txt"});
    CHECK_EQ(deviceBaseName("/sdcard/Download/"), std::string{"Download"});
    CHECK_EQ(deviceBaseName("/"), std::string{});
}

TEST_CASE("isSameOrDescendant matches whole components only") {
    CHECK(isSameOrDescendant("/sdcard/a", "/sdcard/a"));
    CHECK(isSameOrDescendant("/sdcard/a", "/sdcard/a/b/c"));
    CHECK(isSameOrDescendant("/sdcard/a/", "/sdcard/a/b"));
    // A sibling sharing a prefix is not a descendant.
    CHECK(!isSameOrDescendant("/sdcard/a", "/sdcard/ab"));
    CHECK(!isSameOrDescendant("/sdcard/a/b", "/sdcard/a"));
    CHECK(isSameOrDescendant("/", "/anything"));
}

TEST_CASE("validateFileName rejects what would escape or break a path") {
    CHECK(!validateFileName("report.pdf").has_value());
    CHECK(!validateFileName(".hidden").has_value());
    CHECK(!validateFileName("with space").has_value());

    CHECK(validateFileName("").has_value());
    CHECK(validateFileName("   ").has_value());
    CHECK(validateFileName(".").has_value());
    CHECK(validateFileName("..").has_value());
    CHECK(validateFileName("a/b").has_value());
    CHECK(validateFileName(std::string(256, 'x')).has_value());
    CHECK(!validateFileName(std::string(255, 'x')).has_value());
}

// ---------------------------------------------------------------------------
// Collision naming
// ---------------------------------------------------------------------------

TEST_CASE("uniqueCopyName leaves a free name alone") {
    CHECK_EQ(uniqueCopyName("a.txt", {"b.txt"}, false), std::string{"a.txt"});
}

TEST_CASE("uniqueCopyName keeps the extension on files") {
    const std::set<std::string> taken{"photo.jpg"};
    CHECK_EQ(uniqueCopyName("photo.jpg", taken, false), std::string{"photo copy.jpg"});
}

TEST_CASE("uniqueCopyName counts up past existing copies") {
    const std::set<std::string> taken{"photo.jpg", "photo copy.jpg", "photo copy 2.jpg"};
    CHECK_EQ(uniqueCopyName("photo.jpg", taken, false), std::string{"photo copy 3.jpg"});
}

TEST_CASE("uniqueCopyName folds an existing copy suffix instead of stacking it") {
    const std::set<std::string> taken{"a.txt", "a copy.txt"};
    // Copying "a copy.txt" must not produce "a copy copy.txt".
    CHECK_EQ(uniqueCopyName("a copy.txt", taken, false), std::string{"a copy 2.txt"});

    const std::set<std::string> more{"a copy 2.txt", "a copy 3.txt"};
    CHECK_EQ(uniqueCopyName("a copy 2.txt", more, false), std::string{"a copy.txt"});
}

TEST_CASE("uniqueCopyName treats directories and dotfiles as having no extension") {
    CHECK_EQ(uniqueCopyName("v1.2", {"v1.2"}, true), std::string{"v1.2 copy"});
    CHECK_EQ(uniqueCopyName(".bashrc", {".bashrc"}, false), std::string{".bashrc copy"});
    CHECK_EQ(uniqueCopyName("trailing.", {"trailing."}, false), std::string{"trailing. copy"});
}

TEST_CASE("uniqueCopyName only splits the last extension") {
    const std::set<std::string> taken{"archive.tar.gz"};
    CHECK_EQ(uniqueCopyName("archive.tar.gz", taken, false), std::string{"archive.tar copy.gz"});
}

// ---------------------------------------------------------------------------
// Transfer planning -- where data could be lost
// ---------------------------------------------------------------------------

TEST_CASE("planTransfer copies into another folder under the same name") {
    const auto plan = planTransfer({{"/sdcard/a.txt", false}}, "/sdcard/Download", {},
                                   TransferMode::Copy);
    CHECK_EQ(plan.ops.size(), std::size_t{1});
    CHECK(plan.skipped.empty());
    CHECK_EQ(plan.ops[0].source, std::string{"/sdcard/a.txt"});
    CHECK_EQ(plan.ops[0].destination, std::string{"/sdcard/Download/a.txt"});
    CHECK(plan.ops[0].mode == TransferMode::Copy);
}

TEST_CASE("planTransfer never overwrites an existing name") {
    const auto plan = planTransfer({{"/sdcard/a.txt", false}}, "/sdcard/Download",
                                   {"a.txt"}, TransferMode::Move);
    CHECK_EQ(plan.ops.size(), std::size_t{1});
    CHECK_EQ(plan.ops[0].destination, std::string{"/sdcard/Download/a copy.txt"});
}

TEST_CASE("planTransfer duplicates when pasting a copy into its own folder") {
    // Copy + paste in place is the duplicate gesture, so it must succeed.
    const auto plan = planTransfer({{"/sdcard/a.txt", false}}, "/sdcard", {"a.txt"},
                                   TransferMode::Copy);
    CHECK_EQ(plan.ops.size(), std::size_t{1});
    CHECK_EQ(plan.ops[0].destination, std::string{"/sdcard/a copy.txt"});
}

TEST_CASE("planTransfer skips a move into the folder it is already in") {
    const auto plan = planTransfer({{"/sdcard/a.txt", false}}, "/sdcard/", {"a.txt"},
                                   TransferMode::Move);
    CHECK(plan.ops.empty());
    CHECK_EQ(plan.skipped.size(), std::size_t{1});
}

TEST_CASE("planTransfer refuses to put a folder inside itself") {
    // Dropping a folder on itself, or on one of its own subfolders. cp -r would
    // recurse without end; mv would fail part way.
    for (const char* dest : {"/sdcard/Music", "/sdcard/Music/Albums"}) {
        for (const auto mode : {TransferMode::Copy, TransferMode::Move}) {
            const auto plan = planTransfer({{"/sdcard/Music", true}}, dest, {}, mode);
            CHECK(plan.ops.empty());
            CHECK_EQ(plan.skipped.size(), std::size_t{1});
        }
    }
}

TEST_CASE("planTransfer allows a sibling that merely shares a prefix") {
    const auto plan = planTransfer({{"/sdcard/Music", true}}, "/sdcard/MusicBackup", {},
                                   TransferMode::Move);
    CHECK_EQ(plan.ops.size(), std::size_t{1});
}

TEST_CASE("planTransfer refuses the root") {
    const auto plan = planTransfer({{"/", true}}, "/sdcard", {}, TransferMode::Copy);
    CHECK(plan.ops.empty());
    CHECK_EQ(plan.skipped.size(), std::size_t{1});
}

TEST_CASE("planTransfer keeps names unique within one batch") {
    // Two sources with the same basename from different folders must not both
    // land on one destination.
    const auto plan = planTransfer({{"/sdcard/x/notes.txt", false},
                                    {"/sdcard/y/notes.txt", false}},
                                   "/sdcard/z", {}, TransferMode::Copy);
    CHECK_EQ(plan.ops.size(), std::size_t{2});
    CHECK_EQ(plan.ops[0].destination, std::string{"/sdcard/z/notes.txt"});
    CHECK_EQ(plan.ops[1].destination, std::string{"/sdcard/z/notes copy.txt"});
}

TEST_CASE("planTransfer handles a mixed batch") {
    const auto plan = planTransfer({{"/sdcard/a", true},          // fine
                                    {"/sdcard/Dest", true},       // into itself
                                    {"/sdcard/Dest/c.txt", false}},  // already there
                                   "/sdcard/Dest", {"c.txt"}, TransferMode::Move);
    CHECK_EQ(plan.ops.size(), std::size_t{1});
    CHECK_EQ(plan.skipped.size(), std::size_t{2});
    CHECK_EQ(plan.ops[0].destination, std::string{"/sdcard/Dest/a"});
}

// ---------------------------------------------------------------------------
// Commands and their output
// ---------------------------------------------------------------------------

TEST_CASE("isDeletionAllowed refuses the root and relative paths") {
    CHECK(isDeletionAllowed("/sdcard/a.txt"));
    CHECK(!isDeletionAllowed("/"));
    CHECK(!isDeletionAllowed("//"));
    CHECK(!isDeletionAllowed(""));
    CHECK(!isDeletionAllowed("relative/path"));
}

TEST_CASE("buildOpCommand quotes, separates options and reports the exit code") {
    const std::string copy = buildOpCommand({TransferMode::Copy, "/sdcard/it's", "/sdcard/-x"});
    CHECK(copy.starts_with("cp -r -- "));
    CHECK(copy.find("'/sdcard/it'\\''s'") != std::string::npos);
    CHECK(copy.find("'/sdcard/-x'") != std::string::npos);
    CHECK(copy.find("2>&1") != std::string::npos);
    CHECK(copy.find("@@RC $?") != std::string::npos);

    const std::string move = buildOpCommand({TransferMode::Move, "/a", "/b"});
    CHECK(move.starts_with("mv -- "));
}

TEST_CASE("buildRemoveCommand only recurses for directories") {
    CHECK(buildRemoveCommand({"/sdcard/dir", true}).starts_with("rm -rf -- "));
    CHECK(buildRemoveCommand({"/sdcard/f", false}).starts_with("rm -f -- "));
}

TEST_CASE("parseOpOutput reads success and failure") {
    const OpOutcome ok = parseOpOutput("@@RC 0");
    CHECK(ok.ok());
    CHECK(ok.message.empty());

    const OpOutcome failed =
        parseOpOutput("cp: /data/x: Permission denied\ncp: another line\n@@RC 1");
    CHECK(!failed.ok());
    CHECK_EQ(failed.exitCode, 1);
    CHECK_EQ(failed.message, std::string{"cp: /data/x: Permission denied"});
}

TEST_CASE("parseOpOutput treats a missing marker as a failure") {
    // A truncated batch must never be mistaken for success.
    const OpOutcome none = parseOpOutput("");
    CHECK(!none.ok());
    CHECK(!none.message.empty());

    const OpOutcome partial = parseOpOutput("cp: something went wrong");
    CHECK(!partial.ok());
    CHECK_EQ(partial.message, std::string{"cp: something went wrong"});
}

TEST_CASE("parseOpOutput gives a reason for a silent non-zero exit") {
    const OpOutcome silent = parseOpOutput("@@RC 2");
    CHECK(!silent.ok());
    CHECK(!silent.message.empty());
}

TEST_CASE("parseLsNames reads ls -A1 output") {
    const auto names = parseLsNames("a.txt\r\nwith space\n.hidden\n\n");
    CHECK_EQ(names.size(), std::size_t{3});
    CHECK_EQ(names[1], std::string{"with space"});
    CHECK_EQ(names[2], std::string{".hidden"});
}

// ---------------------------------------------------------------------------
// Summaries
// ---------------------------------------------------------------------------

TEST_CASE("summarizeOutcomes reports full success") {
    const auto status = summarizeOutcomes("copied", {"a", "b"},
                                          {OpOutcome{0, {}}, OpOutcome{0, {}}}, {});
    CHECK(status.state == TransferStatus::State::Done);
    CHECK_EQ(status.message, std::string{"copied 2 items"});
    CHECK(status.details.empty());
}

TEST_CASE("summarizeOutcomes reports partial failure with the first reason") {
    const auto status = summarizeOutcomes(
        "moved", {"a", "b", "c"},
        {OpOutcome{0, {}}, OpOutcome{1, "Permission denied"}, OpOutcome{0, {}}}, {});
    CHECK(status.state == TransferStatus::State::Failed);
    CHECK(status.message.find("moved 2 of 3") != std::string::npos);
    CHECK(status.message.find("b: Permission denied") != std::string::npos);
    CHECK_EQ(status.details.size(), std::size_t{1});
}

TEST_CASE("summarizeOutcomes treats an all-skipped batch as nothing done") {
    const auto status = summarizeOutcomes("moved", {}, {}, {"x: already in /sdcard"});
    CHECK(status.state == TransferStatus::State::Failed);
    CHECK(status.message.find("nothing to do") != std::string::npos);
}

TEST_CASE("summarizeOutcomes uses the singular for one item") {
    const auto status = summarizeOutcomes("deleted", {"a"}, {OpOutcome{0, {}}}, {});
    CHECK_EQ(status.message, std::string{"deleted 1 item"});
}

TEST_CASE("summarizeOutcomes mentions skipped items alongside successes") {
    const auto status =
        summarizeOutcomes("moved", {"a"}, {OpOutcome{0, {}}}, {"b: already there"});
    CHECK(status.state == TransferStatus::State::Done);
    CHECK(status.message.find("1 skipped") != std::string::npos);
    CHECK_EQ(status.details.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Download destination
// ---------------------------------------------------------------------------

TEST_CASE("resolveDownloadStart prefers the first folder that exists") {
    const auto temp = std::filesystem::temp_directory_path();
    const auto missing = temp / "emeron-test-no-such-dir-41a7";
    std::error_code ec;
    std::filesystem::remove_all(missing, ec);

    // Last-used folder was deleted since: fall through to the next one.
    CHECK(resolveDownloadStart({missing, temp}) == temp);
    CHECK(resolveDownloadStart({temp, missing}) == temp);
}

TEST_CASE("resolveDownloadStart skips empty entries and files") {
    const auto temp = std::filesystem::temp_directory_path();
    const auto file = temp / "emeron-test-a-file-9c2e";
    { std::ofstream{file} << "x"; }

    // An unset HOME gives an empty candidate; a stale preference can point at
    // something that is now a file. Neither is somewhere to save into.
    CHECK(resolveDownloadStart({std::filesystem::path{}, file, temp}) == temp);

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE("resolveDownloadStart falls back to the last candidate") {
    const auto missing = std::filesystem::path{"/emeron/definitely/not/here"};
    CHECK(resolveDownloadStart({missing}) == missing);
    CHECK(resolveDownloadStart({}).empty());
}

TEST_CASE("buildListCommand follows a symlinked directory") {
    // On a real device /sdcard is a symlink; `ls -l /sdcard` lists the link,
    // `ls -l /sdcard/` lists the directory (1 line vs 57 on an Android 14 phone).
    CHECK_EQ(buildListCommand("/sdcard"), std::string{"ls -lA -- '/sdcard/'"});
    CHECK_EQ(buildListCommand("/sdcard/"), std::string{"ls -lA -- '/sdcard/'"});
    CHECK_EQ(buildListCommand("/"), std::string{"ls -lA -- '/'"});
    CHECK_EQ(buildListCommand("/data/it's"), std::string{"ls -lA -- '/data/it'\\''s/'"});
}
