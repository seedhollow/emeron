#pragma once

// Decompiled source of an APK, through jadx (https://github.com/skylot/jadx).
//
// jadx is a Java library, so emeron does not link it: it runs one JVM per
// opened APK with assets/jadx/JadxBridge.java on top of the user's jadx jar,
// and talks to it over stdin/stdout (the protocol is described in that file).
// The APK is loaded once -- a few seconds even for big apps -- and after that
// a class decompiles on demand in milliseconds, which is how jadx-gui stays
// fast. Nothing is decompiled up front and nothing is written to disk.
//
// The parsing below is pure and unit tested; JadxSession owns the process.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/Result.h"
#include "core/Subprocess.h"

namespace em {

class Dispatcher;

// --- finding jadx -------------------------------------------------------------------------

struct JadxSetup {
    std::filesystem::path java;  // the java launcher
    std::filesystem::path jar;   // jadx-<version>-all.jar
    std::string error;           // what is missing, and how to get it

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// `jadxOverride` is a jar, a jadx install directory or its bin/jadx launcher;
// empty to look in $EMERON_JADX, next to `jadx` on PATH and in the usual
// install locations. Java comes from $JAVA_HOME, then PATH.
[[nodiscard]] JadxSetup findJadx(const std::filesystem::path& jadxOverride);

// The jar inside a jadx install: <dir>/lib/jadx-*-all.jar (release zip, most
// packages) or <dir>/libexec/lib/... (Homebrew). `path` may be the jar, the
// install directory, or bin/jadx.
[[nodiscard]] std::optional<std::filesystem::path> jadxJarIn(const std::filesystem::path& path);

// --- protocol -------------------------------------------------------------------------------

struct JadxFrame {
    enum class Status : std::uint8_t { Ok, Err, Part };
    int id = -1;
    Status status = Status::Ok;
    std::string payload;

    [[nodiscard]] bool final() const noexcept { return status != Status::Part; }
};

// Splits the bridge's stdout into frames; bytes may arrive in any chunking.
class JadxFrameReader {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    // The next complete frame, or nullopt until more bytes arrive. A malformed
    // header is reported as an Err frame with id -1.
    [[nodiscard]] std::optional<JadxFrame> next();

private:
    std::string buffer_;
    std::size_t consumed_ = 0;
};

// One request line. Tabs and line breaks inside arguments become spaces.
[[nodiscard]] std::string jadxRequestLine(int id, std::string_view command,
                                          const std::vector<std::string>& args);

struct JadxClass {
    std::string raw;   // as in the dex: what requests name
    std::string name;  // as shown (renamed when deobfuscating)
    char kind = 'c';   // c class, i interface, e enum, a annotation, b abstract
    bool noCode = false;

    [[nodiscard]] std::string_view packageName() const;
    [[nodiscard]] std::string_view simpleName() const;
};
[[nodiscard]] std::vector<JadxClass> parseJadxClasses(std::string_view payload);

// A spot in decompiled code that jadx knows the meaning of.
struct CodeRef {
    std::uint32_t pos = 0;  // byte offset of the identifier
    // c m f v: a use of a class, method, field, variable; C M F V: where one is
    // declared.
    char kind = 'c';

    [[nodiscard]] bool declaration() const noexcept { return kind >= 'A' && kind <= 'Z'; }
};

struct DecompiledCode {
    std::string text;
    std::vector<CodeRef> refs;  // sorted by pos
};
[[nodiscard]] Result<DecompiledCode> parseJadxCode(std::string_view payload);

struct JadxResolved {
    std::string raw;  // top-level class to open
    std::uint32_t pos = 0;
    std::string name;
};
[[nodiscard]] Result<JadxResolved> parseJadxResolved(std::string_view payload);

// A line of code matching a search, or using a node.
struct CodeHit {
    std::string raw;
    std::string name;
    std::uint32_t pos = 0;
    std::string line;
};

// One chunk of a streamed search or usages answer.
struct CodeHitChunk {
    std::vector<CodeHit> hits;
    int done = -1;  // search progress, classes
    int total = -1;
    bool limited = false;  // stopped at the hit limit
    std::string node;      // usages: the node's full name
    int usageCount = -1;   // usages: set on the last chunk
};
[[nodiscard]] CodeHitChunk parseJadxHits(std::string_view payload);

struct JadxResource {
    std::string name;  // path inside the APK, e.g. res/layout/main.xml
    std::string type;  // jadx ResourceType: XML, MANIFEST, ARSC, IMG, ...
};
[[nodiscard]] std::vector<JadxResource> parseJadxResources(std::string_view payload);

struct JadxResourceContent {
    enum class Kind : std::uint8_t { Text, List, Binary };
    Kind kind = Kind::Binary;
    std::string text;
    std::vector<std::string> children;  // resources.arsc: the decoded values files
    std::uint64_t size = 0;             // binary
};
[[nodiscard]] JadxResourceContent parseJadxResource(std::string_view payload);

// --- the process --------------------------------------------------------------------------

class JadxSession {
public:
    // Called on the UI thread for every frame of a request: Part frames, then
    // exactly one final Ok or Err. A session that dies ends its open requests
    // with an Err frame.
    using Handler = std::function<void(const JadxFrame&)>;

    explicit JadxSession(Dispatcher& dispatcher);
    ~JadxSession();

    JadxSession(const JadxSession&) = delete;
    JadxSession& operator=(const JadxSession&) = delete;
    JadxSession(JadxSession&&) = delete;
    JadxSession& operator=(JadxSession&&) = delete;

    // Writes the bridge source to `workDir` and starts the JVM loading `apks`.
    // `onLoaded` gets request 0: Ok with "key TAB value" lines, or Err.
    [[nodiscard]] Status start(const JadxSetup& setup,
                               const std::vector<std::filesystem::path>& apks, bool deobfuscate,
                               std::string_view bridgeSource,
                               const std::filesystem::path& workDir, Handler onLoaded);

    // Returns the request id, or -1 if the bridge is not running.
    int request(std::string_view command, std::vector<std::string> args, Handler handler);
    void cancel(int id);

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    // The last lines jadx wrote to stderr, for when something fails.
    [[nodiscard]] std::string stderrTail() const;

private:
    void readLoop();
    void failPending(const std::string& message);

    Dispatcher& dispatcher_;
    Subprocess process_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::map<int, Handler> handlers_;
    int nextId_ = 1;
    std::string stderr_;
};

}  // namespace em
