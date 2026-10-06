#include "ui/panels/CodeBrowserPanel.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <system_error>
#include <unordered_map>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "adb/DeviceManager.h"
#include "app/NativeDialogs.h"
#include "app/Theme.h"
#include "core/IniFile.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Notifications.h"
#include "ui/Widgets.h"

namespace em {

namespace embedded {
extern const unsigned char kJadxBridge[];
extern const std::size_t kJadxBridgeSize;
}  // namespace embedded

namespace fs = std::filesystem;

namespace {

constexpr const char* kDeobfKey = "code.deobfuscate";
constexpr const char* kJadxKey = "code.jadx";
constexpr auto kPullTimeout = std::chrono::minutes{10};
constexpr std::size_t kMaxFiltered = 5000;
constexpr std::size_t kMaxHistory = 100;

ImU32 color(const ImVec4& c, float alpha = 1.0F) {
    return ImGui::GetColorU32({c.x, c.y, c.z, c.w * alpha});
}

std::string baseName(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

bool decompilable(const fs::path& p) {
    std::string ext = toLower(p.extension().string());
    return ext == ".apk" || ext == ".apks" || ext == ".xapk" || ext == ".apkm" ||
           ext == ".dex" || ext == ".jar" || ext == ".aar" || ext == ".aab" || ext == ".zip";
}

CodeView::Language languageFor(std::string_view name) {
    const std::string lower = toLower(name);
    if (lower.ends_with(".xml")) return CodeView::Language::Xml;
    if (lower.ends_with(".smali")) return CodeView::Language::Smali;
    if (lower.ends_with(".java") || lower.ends_with(".kt")) return CodeView::Language::Java;
    return CodeView::Language::Plain;
}

// Kind letter and colour for a class, IntelliJ style.
std::pair<const char*, ImVec4> classBadge(char kind) {
    const auto& p = theme::palette();
    switch (kind) {
        case 'i':
            return {"I", p.good};
        case 'e':
            return {"E", p.warning};
        case 'a':
            return {"@", p.muted};
        case 'b':
            return {"A", p.accent};
        default:
            return {"C", p.accent};
    }
}

struct PullResult {
    std::vector<fs::path> apks;
    std::string error;
};

// Every APK of `packageName` (base and splits) into `destination`, skipping
// the ones already there with the same size: reopening an app is instant.
PullResult pullPackage(AdbClient& adb, const std::string& serial,
                       const std::string& packageName, const fs::path& destination) {
    PullResult result;
    auto listed = adb.shell(serial, "pm path " + shellQuote(packageName));
    if (!listed) {
        result.error = "pm path failed: " + listed.error().message;
        return result;
    }
    std::vector<std::string> remote;
    for (const auto line : splitLines(*listed)) {
        if (auto path = stripPrefix(trim(line), "package:")) remote.emplace_back(*path);
    }
    if (remote.empty()) {
        result.error =
            packageName + " has no APK on the device -- is it installed for this user?";
        return result;
    }
    std::string statCmd = "stat -c '%s %n'";
    for (const auto& r : remote) statCmd += " " + shellQuote(r);
    std::map<std::string, std::uint64_t> sizes;
    if (auto stat = adb.shell(serial, statCmd)) {
        for (const auto line : splitLines(*stat)) {
            const auto space = line.find(' ');
            if (space == std::string_view::npos) continue;
            if (auto size = parseNumber<std::uint64_t>(line.substr(0, space)))
                sizes[std::string{line.substr(space + 1)}] = *size;
        }
    }

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        result.error = "cannot create " + destination.string() + ": " + ec.message();
        return result;
    }
    // Splits from an older install would be decompiled too: drop them.
    std::vector<std::string> names;
    for (const auto& r : remote) names.push_back(baseName(r));
    for (const auto& entry : fs::directory_iterator(destination, ec)) {
        const std::string name = entry.path().filename().string();
        if (std::find(names.begin(), names.end(), name) == names.end())
            fs::remove(entry.path(), ec);
    }
    for (const auto& r : remote) {
        const fs::path local = destination / baseName(r);
        const auto known = sizes.find(r);
        std::error_code sizeEc;
        if (known != sizes.end() && fs::exists(local, sizeEc) &&
            fs::file_size(local, sizeEc) == known->second) {
            result.apks.push_back(local);
            continue;
        }
        const fs::path partial = local.string() + ".part";
        if (auto status = adb.pull(serial, r, partial, kPullTimeout); !status) {
            fs::remove(partial, ec);
            result.error = "pulling " + baseName(r) + " failed: " + status.error().message;
            return result;
        }
        fs::rename(partial, local, ec);
        if (ec) {
            result.error = "cannot write " + local.string() + ": " + ec.message();
            return result;
        }
        result.apks.push_back(local);
    }
    return result;
}

}  // namespace

// --- tree -----------------------------------------------------------------------------------

void CodeBrowserPanel::Tree::build(const std::vector<std::string>& names, char sep) {
    nodes.clear();
    nodes.push_back({});
    separator = sep;
    std::unordered_map<std::string, int> folders;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        int parent = 0;
        std::size_t start = 0;
        for (std::size_t at = name.find(sep); at != std::string::npos;
             at = name.find(sep, start)) {
            const std::string path = name.substr(0, at);
            auto [it, fresh] = folders.try_emplace(path, static_cast<int>(nodes.size()));
            if (fresh) {
                nodes.push_back({name.substr(start, at - start), path, parent, {}, -1});
                nodes[static_cast<std::size_t>(parent)].children.push_back(it->second);
            }
            parent = it->second;
            start = at + 1;
        }
        const int leaf = static_cast<int>(nodes.size());
        nodes.push_back({name.substr(start), name, parent, {}, static_cast<int>(i)});
        nodes[static_cast<std::size_t>(parent)].children.push_back(leaf);
    }
    for (auto& node : nodes) {
        std::sort(node.children.begin(), node.children.end(), [this](int a, int b) {
            const TreeNode& x = nodes[static_cast<std::size_t>(a)];
            const TreeNode& y = nodes[static_cast<std::size_t>(b)];
            const bool xf = x.item < 0;
            const bool yf = y.item < 0;
            if (xf != yf) return xf;
            return x.name < y.name;
        });
    }
    dirty = true;
}

void CodeBrowserPanel::Tree::flatten() {
    rows.clear();
    dirty = false;
    if (nodes.empty()) return;
    const std::function<void(int, int)> visit = [&](int index, int depth) {
        const TreeNode* node = &nodes[static_cast<std::size_t>(index)];
        std::string label = node->name;
        // a.b.c with nothing else in a and b: one row, like jadx-gui.
        while (node->item < 0 && node->children.size() == 1 &&
               nodes[static_cast<std::size_t>(node->children[0])].item < 0) {
            index = node->children[0];
            node = &nodes[static_cast<std::size_t>(index)];
            label += separator;
            label += node->name;
        }
        rows.push_back({index, depth, std::move(label)});
        if (node->item < 0 && expanded.contains(node->path)) {
            for (const int c : node->children) visit(c, depth + 1);
        }
    };
    for (const int c : nodes[0].children) visit(c, 0);
}

// --- sources and session --------------------------------------------------------------------

const JadxSetup& CodeBrowserPanel::setup(AppContext& context) {
    (void)context;
    if (!setup_) setup_ = findJadx(jadxOverride_);
    return *setup_;
}

void CodeBrowserPanel::openPackage(AppContext& context, const std::string& packageName) {
    if (!context.hasDevice() || packageName.empty()) return;
    closeSession();
    docs_.clear();
    back_.clear();
    forward_.clear();
    classes_.clear();
    resources_.clear();
    sourcePackage_ = packageName;
    sourceLabel_ = packageName;
    phase_ = Phase::Pulling;
    phaseStarted_ = ImGui::GetTime();
    status_ = "Copying the APKs of " + packageName + " from the phone...";
    failure_.clear();
    const std::uint64_t gen = generation_;
    const std::string serial = context.selectedSerial;
    const fs::path destination = context.workspaceDir / "decompile" / packageName;
    AdbClient& adb = context.devices.adb();
    context.pool.submit([this, &context, &adb, gen, serial, packageName, destination] {
        PullResult result = pullPackage(adb, serial, packageName, destination);
        context.dispatcher.post([this, &context, gen, result = std::move(result)]() mutable {
            if (gen != generation_) return;
            if (!result.error.empty()) {
                phase_ = Phase::Failed;
                status_ = result.error;
                return;
            }
            apks_ = std::move(result.apks);
            startSession(context);
        });
    });
}

void CodeBrowserPanel::openFiles(AppContext& context, std::vector<fs::path> apks,
                                 std::string label) {
    closeSession();
    docs_.clear();
    back_.clear();
    forward_.clear();
    classes_.clear();
    resources_.clear();
    sourcePackage_.clear();
    sourceLabel_ = std::move(label);
    apks_ = std::move(apks);
    startSession(context);
}

void CodeBrowserPanel::closeSession() {
    stopResults();
    session_.reset();
    ++generation_;
}

void CodeBrowserPanel::startSession(AppContext& context) {
    session_.reset();
    const std::uint64_t gen = ++generation_;
    failure_.clear();
    loadInfo_.clear();
    const JadxSetup& jadx = setup(context);
    if (!jadx.ok()) {
        phase_ = Phase::Failed;
        status_ = jadx.error;
        return;
    }
    phase_ = Phase::Loading;
    phaseStarted_ = ImGui::GetTime();
    status_ = apks_.size() == 1 ? "jadx is loading the APK..."
                                : format("jadx is loading %zu APKs...", apks_.size());
    session_ = std::make_unique<JadxSession>(context.dispatcher);
    const std::string_view bridge{reinterpret_cast<const char*>(embedded::kJadxBridge),
                                  embedded::kJadxBridgeSize};
    auto started = session_->start(
        jadx, apks_, deobfuscate_, bridge, context.workspaceDir / "jadx",
        [this, gen](const JadxFrame& frame) {
            if (gen != generation_) return;
            if (frame.status == JadxFrame::Status::Err) {
                phase_ = Phase::Failed;
                status_ = "jadx could not load it: " + frame.payload;
                failure_ = session_ ? session_->stderrTail() : std::string{};
                return;
            }
            std::map<std::string, std::string, std::less<>> info;
            for (const auto line : splitLines(frame.payload)) {
                const auto f = split(line, '\t');
                if (f.size() == 2) info[std::string{f[0]}] = std::string{f[1]};
            }
            const double seconds = parseNumber<double>(info["ms"]).value_or(0.0) / 1000.0;
            loadInfo_ = format("jadx %s  %s classes  loaded in %.1f s", info["jadx"].c_str(),
                               info["classes"].c_str(), seconds);
            status_ = "Reading the class list...";
            session_->request("classes", {}, [this, gen](const JadxFrame& f) {
                if (gen != generation_) return;
                if (f.status != JadxFrame::Status::Ok) {
                    phase_ = Phase::Failed;
                    status_ = f.payload;
                    return;
                }
                classes_ = parseJadxClasses(f.payload);
                std::vector<std::string> names;
                names.reserve(classes_.size());
                for (const auto& c : classes_) names.push_back(c.name);
                classTree_.build(names, '.');
                // Open the app's own package, the way into most investigations.
                if (!sourcePackage_.empty()) {
                    std::string prefix;
                    for (const auto part : split(sourcePackage_, '.')) {
                        prefix += prefix.empty() ? std::string{part} : "." + std::string{part};
                        classTree_.expanded.insert(prefix);
                    }
                }
                filtered_.clear();
                phase_ = Phase::Ready;
                // Documents kept across a restart (deobfuscation toggled) reload.
                for (auto& doc : docs_) {
                    doc->javaLoaded = doc->smaliLoaded = false;
                    doc->children.clear();
                    doc->binarySize.reset();
                    loadDoc(*doc);
                }
            });
            session_->request("resources", {}, [this, gen](const JadxFrame& f) {
                if (gen != generation_ || f.status != JadxFrame::Status::Ok) return;
                resources_ = parseJadxResources(f.payload);
                std::vector<std::string> names;
                names.reserve(resources_.size());
                for (const auto& r : resources_) names.push_back(r.name);
                resourceTree_.build(names, '/');
            });
        });
    if (!started) {
        phase_ = Phase::Failed;
        status_ = "Could not start Java: " + started.error().message;
        session_.reset();
    }
}

// --- documents ------------------------------------------------------------------------------

CodeBrowserPanel::Doc* CodeBrowserPanel::findDoc(std::uint64_t uid) {
    for (auto& d : docs_) {
        if (d->uid == uid) return d.get();
    }
    return nullptr;
}

CodeBrowserPanel::Doc* CodeBrowserPanel::activeDoc() {
    return findDoc(activeUid_);
}

std::optional<CodeBrowserPanel::Place> CodeBrowserPanel::currentPlace() {
    Doc* doc = activeDoc();
    if (doc == nullptr) return std::nullopt;
    return Place{doc->resource, doc->key, doc->smali ? 0U : doc->java.caret()};
}

CodeBrowserPanel::Doc& CodeBrowserPanel::openDoc(bool resource, const std::string& key,
                                                 std::optional<std::uint32_t> pos,
                                                 bool record) {
    if (record) {
        if (auto here = currentPlace()) {
            back_.push_back(std::move(*here));
            if (back_.size() > kMaxHistory) back_.erase(back_.begin());
        }
        forward_.clear();
    }
    for (auto& d : docs_) {
        if (d->resource != resource || d->key != key) continue;
        if (pos) {
            d->smali = false;  // positions are in the Java text
            if (d->javaLoaded)
                d->java.scrollTo(*pos);
            else
                d->pendingPos = pos;
        }
        selectUid_ = activeUid_ = d->uid;
        return *d;
    }
    auto doc = std::make_unique<Doc>();
    doc->uid = nextUid_++;
    doc->resource = resource;
    doc->key = key;
    doc->pendingPos = pos;
    if (resource) {
        doc->fullName = key;
        doc->title = baseName(key);
    } else {
        doc->fullName = key;
        for (const auto& c : classes_) {
            if (c.raw == key) {
                doc->fullName = c.name;
                break;
            }
        }
        const auto dot = doc->fullName.rfind('.');
        doc->title = dot == std::string::npos ? doc->fullName : doc->fullName.substr(dot + 1);
    }
    Doc& ref = *doc;
    docs_.push_back(std::move(doc));
    selectUid_ = activeUid_ = ref.uid;
    loadDoc(ref);
    return ref;
}

void CodeBrowserPanel::loadDoc(Doc& doc) {
    if (!session_ || !session_->running() || phase_ != Phase::Ready) return;
    doc.loading = true;
    doc.error.clear();
    const std::uint64_t gen = generation_;
    const std::uint64_t uid = doc.uid;
    const auto failed = [](Doc& d, const JadxFrame& f) {
        d.loading = false;
        d.error = f.payload;
    };
    if (doc.resource) {
        session_->request("res", {doc.key}, [this, gen, uid, failed](const JadxFrame& f) {
            Doc* d = gen == generation_ ? findDoc(uid) : nullptr;
            if (d == nullptr) return;
            if (f.status != JadxFrame::Status::Ok) return failed(*d, f);
            d->loading = false;
            JadxResourceContent content = parseJadxResource(f.payload);
            switch (content.kind) {
                case JadxResourceContent::Kind::Text:
                    d->java.setText(std::move(content.text), languageFor(d->key));
                    d->javaLoaded = true;
                    break;
                case JadxResourceContent::Kind::List:
                    d->children = std::move(content.children);
                    break;
                case JadxResourceContent::Kind::Binary:
                    d->binarySize = content.size;
                    break;
            }
        });
    } else if (doc.smali) {
        session_->request("smali", {doc.key}, [this, gen, uid, failed](const JadxFrame& f) {
            Doc* d = gen == generation_ ? findDoc(uid) : nullptr;
            if (d == nullptr) return;
            if (f.status != JadxFrame::Status::Ok) return failed(*d, f);
            d->loading = false;
            d->smaliView.setText(f.payload, CodeView::Language::Smali);
            d->smaliLoaded = true;
        });
    } else {
        session_->request("code", {doc.key}, [this, gen, uid, failed](const JadxFrame& f) {
            Doc* d = gen == generation_ ? findDoc(uid) : nullptr;
            if (d == nullptr) return;
            if (f.status != JadxFrame::Status::Ok) return failed(*d, f);
            auto code = parseJadxCode(f.payload);
            d->loading = false;
            if (!code) {
                d->error = code.error().message;
                return;
            }
            d->java.setText(std::move(code->text), CodeView::Language::Java,
                            std::move(code->refs));
            d->javaLoaded = true;
            if (d->pendingPos) {
                d->java.scrollTo(*d->pendingPos);
                d->pendingPos.reset();
            }
        });
    }
}

void CodeBrowserPanel::goBack() {
    if (back_.empty()) return;
    if (auto here = currentPlace()) forward_.push_back(std::move(*here));
    const Place place = back_.back();
    back_.pop_back();
    openDoc(place.resource, place.key, place.pos, false);
}

void CodeBrowserPanel::goForward() {
    if (forward_.empty()) return;
    if (auto here = currentPlace()) back_.push_back(std::move(*here));
    const Place place = forward_.back();
    forward_.pop_back();
    openDoc(place.resource, place.key, place.pos, false);
}

void CodeBrowserPanel::goToDeclaration(Doc& doc, std::uint32_t pos) {
    if (!session_) return;
    const std::uint64_t gen = generation_;
    session_->request("resolve", {doc.key, std::to_string(pos)},
                      [this, gen](const JadxFrame& f) {
                          if (gen != generation_) return;
                          if (f.status != JadxFrame::Status::Ok) {
                              notify::info("Go to declaration", f.payload);
                              return;
                          }
                          auto target = parseJadxResolved(f.payload);
                          if (target) openDoc(false, target->raw, target->pos, true);
                      });
}

void CodeBrowserPanel::stopResults() {
    if (results_.running && session_ && results_.requestId > 0)
        session_->cancel(results_.requestId);
    results_.running = false;
}

void CodeBrowserPanel::findUsages(Doc& doc, std::uint32_t pos) {
    if (!session_) return;
    stopResults();
    const auto [s, e] = doc.java.wordAt(pos);
    results_ = {};
    results_.kind = Results::Kind::Usages;
    results_.title = "Usages of " + doc.java.text().substr(s, e - s);
    results_.running = true;
    const std::uint64_t gen = generation_;
    results_.requestId = session_->request(
        "usages", {doc.key, std::to_string(pos)}, [this, gen](const JadxFrame& f) {
            if (gen != generation_ || results_.kind != Results::Kind::Usages) return;
            if (f.status == JadxFrame::Status::Err) {
                results_.running = false;
                results_.error = f.payload;
                return;
            }
            CodeHitChunk chunk = parseJadxHits(f.payload);
            if (!chunk.node.empty()) results_.title = "Usages of " + chunk.node;
            for (auto& hit : chunk.hits) results_.hits.push_back(std::move(hit));
            if (f.final()) results_.running = false;
        });
}

void CodeBrowserPanel::runSearch(AppContext& context) {
    (void)context;
    const std::string query{trim(query_)};
    if (query.empty() || !session_ || phase_ != Phase::Ready) return;
    stopResults();
    results_ = {};
    results_.kind = Results::Kind::Search;
    results_.title = "\"" + query + "\"" + (querySkipLibraries_ ? " outside libraries" : "");
    results_.running = true;
    const std::uint64_t gen = generation_;
    std::string flags = queryCase_ ? "" : "i";
    if (querySkipLibraries_) flags += 'l';
    results_.requestId = session_->request(
        "search", {flags, "", query, sourcePackage_}, [this, gen](const JadxFrame& f) {
            if (gen != generation_ || results_.kind != Results::Kind::Search) return;
            if (f.status == JadxFrame::Status::Err) {
                results_.running = false;
                results_.error = f.payload;
                return;
            }
            CodeHitChunk chunk = parseJadxHits(f.payload);
            for (auto& hit : chunk.hits) results_.hits.push_back(std::move(hit));
            if (chunk.total >= 0) {
                results_.done = chunk.done;
                results_.total = chunk.total;
            }
            results_.limited = results_.limited || chunk.limited;
            if (f.final()) results_.running = false;
        });
}

// --- drawing --------------------------------------------------------------------------------

void CodeBrowserPanel::draw(AppContext& context) {
    if (!prefsLoaded_) {
        prefsLoaded_ = true;
        deobfuscate_ = context.prefs.getBool(kDeobfKey, false);
        jadxOverride_ = context.prefs.getOr(kJadxKey, "");
    }
    if (!context.browseCodeRequest.empty()) {
        const std::string pkg = std::move(context.browseCodeRequest);
        context.browseCodeRequest.clear();
        if (pkg != sourcePackage_ || phase_ == Phase::Failed || phase_ == Phase::Idle)
            openPackage(context, pkg);
    }
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    // APKs dropped onto this panel open here.
    if (!context.droppedPaths.empty() &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        std::vector<fs::path> files;
        for (const auto& p : context.droppedPaths) {
            if (decompilable(p)) files.push_back(p);
        }
        if (!files.empty()) {
            context.droppedPathsConsumed = true;
            const std::string label =
                files.front().filename().string() +
                (files.size() > 1 ? format(" +%zu", files.size() - 1) : "");
            openFiles(context, std::move(files), label);
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) goBack();
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) goForward();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F,
                            ImGuiInputFlags_RouteGlobal))
            focusQuery_ = true;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_RouteGlobal)) {
            std::erase_if(docs_, [this](const auto& d) { return d->uid == activeUid_; });
        }
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        if (ImGui::IsMouseClicked(3)) goBack();
        if (ImGui::IsMouseClicked(4)) goForward();
    }

    drawToolbar(context);
    ImGui::Separator();
    if (phase_ != Phase::Ready) {
        drawEmpty(context);
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##code", 2, flags, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthStretch, 0.27F);
        ImGui::TableSetupColumn("docs", ImGuiTableColumnFlags_WidthStretch, 0.73F);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        drawSidebar(context);
        ImGui::TableNextColumn();
        drawDocuments(context);
        ImGui::EndTable();
    }
    ImGui::End();
}

void CodeBrowserPanel::onShutdown(AppContext& context) {
    (void)context;
    closeSession();
}

void CodeBrowserPanel::drawToolbar(AppContext& context) {
    const auto& palette = theme::palette();
    const bool busy = phase_ == Phase::Pulling || phase_ == Phase::Loading;

    ImGui::BeginDisabled(back_.empty());
    if (ImGui::Button(ICON_FA_ARROW_LEFT)) goBack();
    ImGui::EndDisabled();
    widgets::termTooltip("Back  (Alt+Left)", "Where you were before the last jump.");
    ImGui::SameLine();
    ImGui::BeginDisabled(forward_.empty());
    if (ImGui::Button(ICON_FA_ARROW_RIGHT)) goForward();
    ImGui::EndDisabled();
    widgets::termTooltip("Forward  (Alt+Right)", "Undo Back.");

    // Which app.
    ImGui::SameLine();
    const bool canPackage = context.hasDevice() && context.hasPackage();
    const std::string openLabel =
        canPackage ? std::string{ICON_FA_MOBILE_SCREEN "  "} + context.selectedPackage
                   : std::string{ICON_FA_MOBILE_SCREEN "  Pick an app in the toolbar"};
    ImGui::BeginDisabled(!canPackage || busy);
    if (ImGui::Button((openLabel + "###openpkg").c_str()))
        openPackage(context, context.selectedPackage);
    ImGui::EndDisabled();
    widgets::termTooltip(
        "Decompile the selected app",
        "Copy its APKs (base and splits) from the phone and open them in jadx. "
        "Copies are kept, so reopening is instant.");
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open file")) {
        auto choice =
            dialogs::openFile("Open an APK, DEX or JAR", context.workspaceDir, "Android code",
                              "apk,apks,xapk,apkm,dex,jar,aar,aab,zip");
        if (choice.chosen())
            openFiles(context, {choice.path}, choice.path.filename().string());
        else if (choice.outcome == dialogs::Outcome::Unavailable)
            notify::info("Open file",
                         "No native file dialog in this build: drop the APK onto the panel.");
    }
    ImGui::EndDisabled();
    widgets::termTooltip("Open a file",
                         "An APK, split APKs (.apks/.xapk), DEX, JAR or AAR. Or drop it here.");

    if (phase_ == Phase::Ready || phase_ == Phase::Failed) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE)) {
            if (!sourcePackage_.empty() && phase_ == Phase::Failed)
                openPackage(context, sourcePackage_);
            else if (!apks_.empty())
                startSession(context);
        }
        widgets::termTooltip("Reload", "Restart jadx on the same APKs.");
    }

    // Options.
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GEAR)) ImGui::OpenPopup("##codeoptions");
    widgets::termTooltip("Options", "Deobfuscation, and where jadx is.");
    if (ImGui::BeginPopup("##codeoptions")) {
        if (ImGui::Checkbox("Deobfuscate names", &deobfuscate_)) {
            context.prefs.setBool(kDeobfKey, deobfuscate_);
            if (!apks_.empty() && phase_ != Phase::Pulling) startSession(context);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(jadx --deobf)");
        ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
        ImGui::TextWrapped(
            "Renames short or clashing obfuscated names (a, b, C0123a) to unique ones\n"
            "so they can be told apart. It cannot recover the original names.");
        ImGui::PopStyleColor();
        ImGui::Separator();
        const JadxSetup& jadx = setup(context);
        if (jadx.ok()) {
            ImGui::TextDisabled("jadx: %s", jadx.jar.string().c_str());
            ImGui::TextDisabled("java: %s", jadx.java.string().c_str());
        } else {
            ImGui::TextColored(palette.bad, "%s", jadx.error.c_str());
        }
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Locate jadx...")) {
            auto choice = dialogs::openFile(
                "Locate jadx: its bin/jadx launcher or jadx-*-all.jar", {}, "", "");
            if (choice.chosen()) {
                jadxOverride_ = choice.path.string();
                context.prefs.set(kJadxKey, jadxOverride_);
                setup_.reset();
            }
        }
        if (!jadxOverride_.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Auto-detect")) {
                jadxOverride_.clear();
                context.prefs.set(kJadxKey, "");
                setup_.reset();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE "  Look again")) setup_.reset();
        ImGui::EndPopup();
    }

    // Search.
    if (phase_ == Phase::Ready) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (focusQuery_) {
            ImGui::SetKeyboardFocusHere();
            focusQuery_ = false;
        }
        ImGui::SetNextItemWidth(
            std::clamp(ImGui::GetContentRegionAvail().x * 0.35F, 140.0F, 360.0F));
        if (ImGui::InputTextWithHint("##query",
                                     ICON_FA_MAGNIFYING_GLASS "  Search code  (Ctrl+Shift+F)",
                                     &query_, ImGuiInputTextFlags_EnterReturnsTrue))
            runSearch(context);
        ImGui::SameLine();
        ImGui::Checkbox("Aa", &queryCase_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match case");
        ImGui::SameLine();
        ImGui::Checkbox("Skip libraries", &querySkipLibraries_);
        widgets::termTooltip(
            "Skip libraries",
            "Leave androidx, kotlin, Play services, OkHttp, Glide and other "
            "common libraries out. Searching everything decompiles every class "
            "once: slow on a big app the first time.");
        if (!loadInfo_.empty()) {
            widgets::sameLineOrWrap(ImGui::CalcTextSize(loadInfo_.c_str()).x);
            ImGui::TextDisabled("%s", loadInfo_.c_str());
        }
    }
}

void CodeBrowserPanel::drawEmpty(AppContext& context) {
    const auto& palette = theme::palette();
    ImGui::Spacing();
    if (phase_ == Phase::Pulling || phase_ == Phase::Loading) {
        ImGui::Text(ICON_FA_HOURGLASS_HALF "  %s", status_.c_str());
        ImGui::TextDisabled("%.0f s", ImGui::GetTime() - phaseStarted_);
        if (phase_ == Phase::Loading) {
            ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
            ImGui::TextWrapped(
                "jadx reads every class once (a few seconds, longer for very big apps). "
                "After that, classes decompile as you open them.");
            ImGui::PopStyleColor();
        }
        return;
    }
    if (phase_ == Phase::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.bad);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", status_.c_str());
        ImGui::PopStyleColor();
        if (!failure_.empty() && ImGui::TreeNode("jadx output")) {
            ImGui::InputTextMultiline("##stderr", &failure_,
                                      {-FLT_MIN, ImGui::GetTextLineHeight() * 12.0F},
                                      ImGuiInputTextFlags_ReadOnly);
            ImGui::TreePop();
        }
        ImGui::Spacing();
    }

    ImGui::TextUnformatted(ICON_FA_CODE "  Browse an app's decompiled source");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
    ImGui::TextWrapped(
        "Pick an app in the toolbar and press its button above, use Browse code in the Apps "
        "panel, "
        "or open / drop an APK file. Classes decompile when you open them: click a name with "
        "Ctrl "
        "(Cmd) held, or press D, to go to its declaration; X finds usages; Ctrl+Shift+F "
        "searches "
        "all code; Alt+Left goes back. Decompiled code is for understanding and debugging -- "
        "respect the app's license.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    const JadxSetup& jadx = setup(context);
    if (jadx.ok()) {
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK "  jadx found: %s",
                            jadx.jar.filename().string().c_str());
    } else if (phase_ != Phase::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", jadx.error.c_str());
        ImGui::PopStyleColor();
    }
}

void CodeBrowserPanel::drawSidebar(AppContext& context) {
    (void)context;
    if (!ImGui::BeginTabBar("##side")) return;
    const std::string classesLabel =
        format(ICON_FA_CUBES "  Classes (%zu)###classes", classes_.size());
    if (ImGui::BeginTabItem(classesLabel.c_str())) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, ImGuiInputFlags_RouteGlobal))
            ImGui::SetKeyboardFocusHere();
        if (ImGui::InputTextWithHint("##classfilter",
                                     ICON_FA_MAGNIFYING_GLASS "  Class name  (Ctrl+N)",
                                     &classFilter_)) {
            filtered_.clear();
            const std::string_view needle = trim(classFilter_);
            for (std::size_t i = 0; i < classes_.size() && filtered_.size() < kMaxFiltered;
                 ++i) {
                if (containsIgnoreCase(classes_[i].name, needle))
                    filtered_.push_back(static_cast<int>(i));
            }
            // Simple-name hits first: typing "MainActivity" should find it at the top.
            std::stable_sort(filtered_.begin(), filtered_.end(), [&](int a, int b) {
                const bool x = containsIgnoreCase(
                    classes_[static_cast<std::size_t>(a)].simpleName(), needle);
                const bool y = containsIgnoreCase(
                    classes_[static_cast<std::size_t>(b)].simpleName(), needle);
                return x && !y;
            });
        }
        if (trim(classFilter_).empty()) {
            drawTree(classTree_, true);
        } else {
            ImGui::BeginChild("##filtered", {0.0F, 0.0F}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar);
            const Doc* active = activeDoc();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(filtered_.size()));
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    const JadxClass& c = classes_[static_cast<std::size_t>(
                        filtered_[static_cast<std::size_t>(r)])];
                    ImGui::PushID(r);
                    const auto [badge, badgeColor] = classBadge(c.kind);
                    const bool selected =
                        active != nullptr && !active->resource && active->key == c.raw;
                    if (ImGui::Selectable("##c", selected, ImGuiSelectableFlags_None))
                        openDoc(false, c.raw, std::nullopt, true);
                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetItemRectSize().x);
                    ImGui::TextColored(badgeColor, "%s", badge);
                    ImGui::SameLine();
                    const std::string simple{c.simpleName()};
                    ImGui::TextUnformatted(simple.c_str());
                    ImGui::SameLine();
                    const std::string pkg{c.packageName()};
                    ImGui::TextDisabled("%s", pkg.c_str());
                    ImGui::PopID();
                }
            }
            if (filtered_.empty())
                ImGui::TextDisabled("No class matches.");
            else if (filtered_.size() >= kMaxFiltered)
                ImGui::TextDisabled("First %zu shown.", kMaxFiltered);
            ImGui::EndChild();
        }
        ImGui::EndTabItem();
    }
    const std::string resLabel =
        format(ICON_FA_FILE_ZIPPER "  Resources (%zu)###res", resources_.size());
    if (ImGui::BeginTabItem(resLabel.c_str())) {
        drawTree(resourceTree_, false);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void CodeBrowserPanel::drawTree(Tree& tree, bool classes) {
    if (tree.dirty) tree.flatten();
    const auto& palette = theme::palette();
    ImGui::BeginChild("##tree", {0.0F, 0.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float indent = ImGui::GetFontSize() * 0.9F;
    const float iconW = ImGui::GetFontSize() * 1.4F;
    const Doc* active = activeDoc();
    bool changed = false;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(tree.rows.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto& row = tree.rows[static_cast<std::size_t>(r)];
            const TreeNode& node = tree.nodes[static_cast<std::size_t>(row.node)];
            const bool folder = node.item < 0;
            const float textW = ImGui::CalcTextSize(row.label.c_str()).x;
            const float width =
                static_cast<float>(row.depth) * indent + iconW * 2.0F + textW + 8.0F;
            ImGui::PushID(row.node);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            bool selected = false;
            if (!folder && active != nullptr && active->resource == !classes) {
                const std::string& key =
                    classes ? classes_[static_cast<std::size_t>(node.item)].raw
                            : resources_[static_cast<std::size_t>(node.item)].name;
                selected = active->key == key;
            }
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None,
                                  {std::max(width, ImGui::GetContentRegionAvail().x), 0.0F})) {
                if (folder) {
                    if (!tree.expanded.erase(node.path)) tree.expanded.insert(node.path);
                    changed = true;
                } else if (classes) {
                    openDoc(false, classes_[static_cast<std::size_t>(node.item)].raw,
                            std::nullopt, true);
                } else {
                    openDoc(true, resources_[static_cast<std::size_t>(node.item)].name,
                            std::nullopt, true);
                }
            }
            if (!folder && classes && ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "%s", classes_[static_cast<std::size_t>(node.item)].name.c_str());

            float x = start.x + static_cast<float>(row.depth) * indent;
            const float y = start.y;
            if (folder) {
                const bool open = tree.expanded.contains(node.path);
                draw->AddText({x, y}, color(palette.muted),
                              open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
                x += iconW * 0.7F;
                draw->AddText({x, y}, color(palette.warning, 0.85F),
                              open ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER);
                x += iconW;
                draw->AddText({x, y}, ImGui::GetColorU32(ImGuiCol_Text), row.label.c_str());
            } else {
                x += iconW * 0.7F;
                if (classes) {
                    const JadxClass& c = classes_[static_cast<std::size_t>(node.item)];
                    const auto [badge, badgeColor] = classBadge(c.kind);
                    draw->AddText({x + 2.0F, y}, color(badgeColor), badge);
                    x += iconW;
                    draw->AddText({x, y},
                                  color(c.noCode ? palette.muted
                                                 : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                                  row.label.c_str());
                } else {
                    draw->AddText({x, y}, color(palette.muted), ICON_FA_FILE);
                    x += iconW;
                    draw->AddText({x, y}, ImGui::GetColorU32(ImGuiCol_Text), row.label.c_str());
                }
            }
            ImGui::PopID();
        }
    }
    if (tree.rows.empty()) ImGui::TextDisabled("Nothing here.");
    ImGui::EndChild();
    if (changed) tree.dirty = true;
}

void CodeBrowserPanel::drawDocuments(AppContext& context) {
    const bool showResults = results_.kind != Results::Kind::None;
    const float avail = ImGui::GetContentRegionAvail().y;
    const float splitterH = 6.0F;
    if (showResults)
        resultsHeight_ = std::clamp(resultsHeight_, 80.0F, std::max(80.0F, avail - 120.0F));
    const float docsH = showResults ? avail - resultsHeight_ - splitterH : 0.0F;

    ImGui::BeginChild("##docs", {0.0F, docsH});
    if (docs_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Open a class from the tree, find one with Ctrl+N, or search the code with "
            "Ctrl+Shift+F.");
    } else if (ImGui::BeginTabBar("##doctabs", ImGuiTabBarFlags_Reorderable |
                                                   ImGuiTabBarFlags_FittingPolicyScroll |
                                                   ImGuiTabBarFlags_TabListPopupButton)) {
        std::vector<std::uint64_t> closed;
        for (auto& doc : docs_) {
            bool open = true;
            const ImGuiTabItemFlags flags =
                selectUid_ == doc->uid ? ImGuiTabItemFlags_SetSelected : 0;
            const std::string label = std::string{doc->resource ? ICON_FA_FILE "  " : ""} +
                                      doc->title + "###doc" + std::to_string(doc->uid);
            const bool visible = ImGui::BeginTabItem(label.c_str(), &open, flags);
            if (ImGui::IsItemHovered()) {
                ImGui::SetItemTooltip("%s", doc->fullName.c_str());
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) open = false;
            }
            if (visible) {
                activeUid_ = doc->uid;
                drawDoc(context, *doc);
                ImGui::EndTabItem();
            }
            if (!open) closed.push_back(doc->uid);
        }
        ImGui::EndTabBar();
        selectUid_ = 0;
        for (const auto uid : closed) {
            std::erase_if(docs_, [uid](const auto& d) { return d->uid == uid; });
        }
    }
    ImGui::EndChild();

    if (!showResults) return;
    ImGui::InvisibleButton("##split", {-FLT_MIN, splitterH});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) resultsHeight_ -= ImGui::GetIO().MouseDelta.y;
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine({a.x, (a.y + b.y) * 0.5F}, {b.x, (a.y + b.y) * 0.5F},
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::BeginChild("##results");
    drawResults(context);
    ImGui::EndChild();
}

void CodeBrowserPanel::drawDoc(AppContext& context, Doc& doc) {
    (void)context;
    const auto& palette = theme::palette();
    if (!doc.resource) {
        if (ImGui::RadioButton("Java", !doc.smali)) doc.smali = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Smali", doc.smali)) doc.smali = true;
        // Members: the declarations jadx marked, for jumping around big classes.
        ImGui::SameLine();
        ImGui::BeginDisabled(doc.smali || !doc.javaLoaded);
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::BeginCombo("##members", ICON_FA_LIST "  Members",
                              ImGuiComboFlags_HeightLarge)) {
            const std::string& text = doc.java.text();
            for (const CodeRef& ref : doc.java.refs()) {
                if (ref.kind != 'M' && ref.kind != 'F' && ref.kind != 'C') continue;
                const auto [s, e] = doc.java.wordAt(ref.pos);
                if (e <= s) continue;
                const char* icon = ref.kind == 'M' ? "m " : ref.kind == 'F' ? "f " : "C ";
                const std::string label = std::string{icon} + text.substr(s, e - s) + "  :" +
                                          std::to_string(doc.java.lineOf(ref.pos) + 1) + "###" +
                                          std::to_string(ref.pos);
                if (ImGui::Selectable(label.c_str())) {
                    if (auto here = currentPlace()) back_.push_back(std::move(*here));
                    forward_.clear();
                    doc.java.scrollTo(ref.pos);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_COPY)) ImGui::SetClipboardText(doc.fullName.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the class name");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", doc.fullName.c_str());
    } else {
        ImGui::TextDisabled("%s", doc.fullName.c_str());
    }

    if (!doc.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.bad);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", doc.error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    // One request per document at a time; the view on show loads once the
    // other is done (switching to Smali while the Java is still coming).
    if (!doc.loading && doc.error.empty() && !doc.resource &&
        !(doc.smali ? doc.smaliLoaded : doc.javaLoaded)) {
        loadDoc(doc);
    }
    if (doc.loading) {
        ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Decompiling...");
        return;
    }
    if (!doc.children.empty()) {
        ImGui::TextDisabled("%zu decoded value files:", doc.children.size());
        ImGui::BeginChild("##children");
        for (const auto& child : doc.children) {
            if (ImGui::Selectable(child.c_str())) {
                openDoc(true, child, std::nullopt, true);
                break;  // docs_ changed
            }
        }
        ImGui::EndChild();
        return;
    }
    if (doc.binarySize) {
        ImGui::TextDisabled(ICON_FA_FILE
                            "  Binary file, %s. Only text and decoded XML are shown.",
                            humanBytes(static_cast<double>(*doc.binarySize)).c_str());
        return;
    }

    CodeView& view = doc.smali ? doc.smaliView : doc.java;
    const CodeView::Action action = view.draw("view");
    if (doc.smali || doc.resource) return;
    switch (action.kind) {
        case CodeView::Action::Kind::GoTo:
            goToDeclaration(doc, action.pos);
            break;
        case CodeView::Action::Kind::FindUsages:
            findUsages(doc, action.pos);
            break;
        case CodeView::Action::Kind::None:
            break;
    }
}

void CodeBrowserPanel::drawResults(AppContext& context) {
    (void)context;
    const auto& palette = theme::palette();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(results_.kind == Results::Kind::Search ? ICON_FA_MAGNIFYING_GLASS
                                                                  : ICON_FA_SITEMAP);
    ImGui::SameLine();
    ImGui::TextUnformatted(results_.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu%s", results_.hits.size(),
                        results_.limited ? "+ (limit reached)" : "");
    if (results_.running) {
        ImGui::SameLine();
        if (results_.total > 0) {
            const float fraction =
                static_cast<float>(results_.done) / static_cast<float>(results_.total);
            ImGui::ProgressBar(fraction, {160.0F, 0.0F},
                               format("%d / %d", results_.done, results_.total).c_str());
        } else {
            ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_STOP "  Stop")) stopResults();
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetFrameHeight() -
                    ImGui::GetStyle().WindowPadding.x);
    if (ImGui::SmallButton(ICON_FA_XMARK)) {
        stopResults();
        results_ = {};
        return;
    }
    if (!results_.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.bad);
        ImGui::TextWrapped("%s", results_.error.c_str());
        ImGui::PopStyleColor();
    }
    if (!results_.running && results_.hits.empty() && results_.error.empty()) {
        ImGui::TextDisabled(results_.kind == Results::Kind::Usages ? "No usages in this APK."
                                                                   : "Nothing found.");
    }

    ImGui::BeginChild("##hits", {0.0F, 0.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(results_.hits.size()));
    std::optional<CodeHit> open;
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const CodeHit& hit = results_.hits[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const auto dot = hit.name.rfind('.');
            const std::string simple =
                dot == std::string::npos ? hit.name : hit.name.substr(dot + 1);
            const float w = ImGui::CalcTextSize(simple.c_str()).x +
                            ImGui::CalcTextSize(hit.line.c_str()).x + 40.0F;
            if (ImGui::Selectable("##hit", false, ImGuiSelectableFlags_None,
                                  {std::max(w, ImGui::GetContentRegionAvail().x), 0.0F}))
                open = hit;
            if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", hit.name.c_str());
            ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextColored(palette.accent, "%s", simple.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(hit.line.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (open) openDoc(false, open->raw, open->pos, true);
}

}  // namespace em
