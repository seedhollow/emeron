#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <IconsFontAwesome6.h>

#include "collect/JadxSession.h"
#include "ui/CodeView.h"
#include "ui/Panel.h"

namespace em {

// An app's decompiled source, the jadx-gui way: a package tree, classes that
// decompile when opened, Java or smali, go to declaration, find usages,
// full-text search, and the decoded manifest and resources. The APKs come
// from the phone (the selected package, base and splits) or from a file.
class CodeBrowserPanel final : public Panel {
public:
    static constexpr const char* kId = "Code";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_CODE; }
    void draw(AppContext& context) override;
    void onShutdown(AppContext& context) override;

private:
    enum class Phase : std::uint8_t { Idle, Pulling, Loading, Ready, Failed };

    // A tree over dotted class names or slashed resource paths.
    struct TreeNode {
        std::string name;  // one segment
        std::string path;  // full path of a folder; the item's name for a leaf
        int parent = -1;
        std::vector<int> children;  // folders first, then leaves; each alphabetical
        int item = -1;              // index into classes_ / resources_ for a leaf
    };
    struct Tree {
        std::vector<TreeNode> nodes;  // [0] is the root
        std::unordered_set<std::string> expanded;
        struct Row {
            int node;
            int depth;
            std::string label;  // folders: single-child chains joined
        };
        std::vector<Row> rows;
        bool dirty = true;
        char separator = '.';

        void build(const std::vector<std::string>& names, char separator);
        void flatten();
    };

    struct Doc {
        std::uint64_t uid = 0;
        bool resource = false;
        std::string key;    // raw class name, or resource name
        std::string title;  // tab label
        std::string fullName;
        bool smali = false;
        CodeView java;
        CodeView smaliView;
        bool javaLoaded = false;
        bool smaliLoaded = false;
        bool loading = false;
        std::string error;
        std::optional<std::uint32_t> pendingPos;
        std::vector<std::string> children;  // resources.arsc
        std::optional<std::uint64_t> binarySize;
    };

    struct Place {
        bool resource = false;
        std::string key;
        std::uint32_t pos = 0;
    };

    struct Results {
        enum class Kind : std::uint8_t { None, Search, Usages };
        Kind kind = Kind::None;
        std::string title;
        std::vector<CodeHit> hits;
        int done = -1;
        int total = -1;
        bool running = false;
        bool limited = false;
        int requestId = -1;
        std::string error;
    };

    // Sources.
    void openPackage(AppContext& context, const std::string& packageName);
    void openFiles(AppContext& context, std::vector<std::filesystem::path> apks,
                   std::string label);
    void startSession(AppContext& context);
    void closeSession();
    [[nodiscard]] const JadxSetup& setup(AppContext& context);

    // Drawing.
    void drawToolbar(AppContext& context);
    void drawEmpty(AppContext& context);
    void drawSidebar(AppContext& context);
    void drawTree(Tree& tree, bool classes);
    void drawDocuments(AppContext& context);
    void drawDoc(AppContext& context, Doc& doc);
    void drawResults(AppContext& context);

    // Documents and navigation.
    Doc& openDoc(bool resource, const std::string& key, std::optional<std::uint32_t> pos,
                 bool record);
    void loadDoc(Doc& doc);
    [[nodiscard]] Doc* findDoc(std::uint64_t uid);
    [[nodiscard]] Doc* activeDoc();
    [[nodiscard]] std::optional<Place> currentPlace();
    void goBack();
    void goForward();
    void goToDeclaration(Doc& doc, std::uint32_t pos);
    void findUsages(Doc& doc, std::uint32_t pos);
    void runSearch(AppContext& context);
    void stopResults();

    // Source and session.
    Phase phase_ = Phase::Idle;
    std::string sourceLabel_;    // what is open, for the toolbar
    std::string sourcePackage_;  // set when it came from the phone
    std::vector<std::filesystem::path> apks_;
    std::string status_;   // what is happening, or what failed
    std::string failure_;  // stderr of a failed start
    double phaseStarted_ = 0.0;
    std::unique_ptr<JadxSession> session_;
    std::uint64_t generation_ = 0;  // bumped per session; stale answers are dropped
    bool deobfuscate_ = false;
    bool prefsLoaded_ = false;
    std::string jadxOverride_;
    std::optional<JadxSetup> setup_;

    // Contents.
    std::vector<JadxClass> classes_;
    std::vector<JadxResource> resources_;
    Tree classTree_;
    Tree resourceTree_;
    std::string classFilter_;
    std::vector<int> filtered_;  // class indices matching classFilter_
    std::string loadInfo_;       // "16 565 classes, loaded in 2.4 s"

    // Documents.
    std::vector<std::unique_ptr<Doc>> docs_;
    std::uint64_t nextUid_ = 1;
    std::uint64_t activeUid_ = 0;
    std::uint64_t selectUid_ = 0;  // tab to bring to the front next frame
    std::vector<Place> back_;
    std::vector<Place> forward_;

    // Search and usages.
    std::string query_;
    bool queryCase_ = false;
    bool querySkipLibraries_ = true;
    bool focusQuery_ = false;
    Results results_;
    float resultsHeight_ = 220.0F;
};

}  // namespace em
