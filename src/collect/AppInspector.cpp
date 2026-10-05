#include "collect/AppInspector.h"

#include <array>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>

#include "core/Hash.h"
#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "apps";

// Pulling a large game's split APKs over USB 2 can take minutes.
constexpr std::chrono::milliseconds kApkPullTimeout{600'000};

std::size_t indentOf(std::string_view line) noexcept {
    std::size_t n = 0;
    while (n < line.size() && line[n] == ' ') ++n;
    return n;
}

// "[ SYSTEM HAS_CODE ]" or "[base, chrome]" -> {"SYSTEM", "HAS_CODE"} / {"base", "chrome"}.
std::vector<std::string> parseBracketList(std::string_view value) {
    std::vector<std::string> out;
    value = trim(value);
    if (value.starts_with('[')) value.remove_prefix(1);
    if (value.ends_with(']')) value.remove_suffix(1);
    for (auto part : split(value, ',', false)) {
        for (auto token : tokenize(part)) {
            if (!token.empty()) out.emplace_back(token);
        }
    }
    return out;
}

std::int64_t toInt64(std::string_view text, std::int64_t fallback = -1) {
    return parseNumber<std::int64_t>(trim(text)).value_or(fallback);
}

bool toBool(std::string_view text) { return trim(text) == "true"; }

// One `key=value` line. A line is several pairs only when every
// space-separated token is itself a pair ("versionCode=1 minSdk=2 targetSdk=3");
// anything else -- "timeStamp=2024-01-01 07:00:00", "flags=[ A B ]",
// "signatures=PackageSignatures{...}" -- is one key with the rest as value.
void parseKeyValues(std::string_view text,
                    std::vector<std::pair<std::string, std::string>>& out) {
    const auto tokens = tokenize(text);
    const bool allPairs =
        tokens.size() > 1 && std::all_of(tokens.begin(), tokens.end(), [](std::string_view t) {
            const auto eq = t.find('=');
            return eq != std::string_view::npos && eq > 0;
        });
    if (allPairs) {
        for (const auto token : tokens) {
            const auto eq = token.find('=');
            out.emplace_back(std::string{token.substr(0, eq)}, std::string{token.substr(eq + 1)});
        }
        return;
    }
    const auto eq = text.find('=');
    if (eq == std::string_view::npos || eq == 0) return;
    out.emplace_back(std::string{trim(text.substr(0, eq))}, std::string{trim(text.substr(eq + 1))});
}

std::string firstValue(const std::vector<std::pair<std::string, std::string>>& fields,
                       std::string_view key) {
    for (const auto& [k, v] : fields) {
        if (k == key) return v;
    }
    return {};
}

// "name: granted=true, flags=[ USER_SET|USER_FIXED]" -> grant.
PermissionGrant parseGrant(std::string_view item) {
    PermissionGrant grant;
    const auto colon = item.find(':');
    grant.name = std::string{trim(item.substr(0, colon))};
    if (colon == std::string_view::npos) return grant;
    const std::string_view rest = item.substr(colon + 1);
    grant.granted = rest.find("granted=true") != std::string_view::npos;
    if (const auto flags = rest.find("flags=["); flags != std::string_view::npos) {
        std::string_view f = rest.substr(flags + 7);
        if (const auto end = f.find(']'); end != std::string_view::npos) f = f.substr(0, end);
        grant.flags = std::string{trim(f)};
    }
    return grant;
}

std::string baseName(std::string_view path) {
    const auto slash = path.rfind('/');
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

// "com.app/.Main" or "com.app/com.app.Main" -> "com.app.Main", if it belongs
// to `packageName`.
std::optional<std::string> componentClass(std::string_view token, std::string_view packageName) {
    const auto slash = token.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    if (token.substr(0, slash) != packageName) return std::nullopt;
    std::string_view cls = token.substr(slash + 1);
    while (!cls.empty() && (cls.back() == ':' || cls.back() == '}')) cls.remove_suffix(1);
    if (cls.empty()) return std::nullopt;
    if (cls.front() == '.') return std::string{packageName} + std::string{cls};
    return std::string{cls};
}

// The section a column-0 header line opens, for the parts of the dump that
// live outside the package block.
enum class Section {
    Other,
    Activities,
    Receivers,
    Services,
    Providers,
    RegisteredProviders,
    ProviderAuthorities,
    Packages,
    HiddenPackages,
    Dexopt,
};

Section sectionFor(std::string_view header) {
    if (header == "Activity Resolver Table:") return Section::Activities;
    if (header == "Receiver Resolver Table:") return Section::Receivers;
    if (header == "Service Resolver Table:") return Section::Services;
    if (header == "Provider Resolver Table:") return Section::Providers;
    if (header == "Registered ContentProviders:") return Section::RegisteredProviders;
    if (header == "ContentProvider Authorities:") return Section::ProviderAuthorities;
    if (header == "Packages:") return Section::Packages;
    if (header == "Hidden system packages:") return Section::HiddenPackages;
    if (header == "Dexopt state:") return Section::Dexopt;
    return Section::Other;
}

class ComponentCollector {
public:
    explicit ComponentCollector(std::string_view packageName) : package_(packageName) {}

    AppComponent* get(AppComponent::Kind kind, const std::string& cls) {
        const std::string key = std::to_string(static_cast<int>(kind)) + cls;
        if (const auto it = index_.find(key); it != index_.end()) return &components_[it->second];
        index_.emplace(key, components_.size());
        AppComponent component;
        component.kind = kind;
        component.className = cls;
        components_.push_back(std::move(component));
        return &components_.back();
    }

    std::vector<AppComponent> take() {
        std::stable_sort(components_.begin(), components_.end(),
                         [](const AppComponent& a, const AppComponent& b) {
                             if (a.kind != b.kind) return a.kind < b.kind;
                             return a.className < b.className;
                         });
        return std::move(components_);
    }

    [[nodiscard]] std::string_view package() const noexcept { return package_; }

private:
    std::string package_;
    std::vector<AppComponent> components_;
    std::unordered_map<std::string, std::size_t> index_;
};

void addUnique(std::vector<std::string>& list, std::string value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(std::move(value));
}

// Parses one "  Package [name] (hash):" block into fields and lists.
void parsePackageBlock(const std::vector<std::string_view>& lines, std::size_t begin,
                       std::size_t end, AppDetails& details) {
    struct OpenList {
        std::size_t indent;
        std::string name;
    };
    std::optional<OpenList> list;
    // Lines belonging to users other than 0 (work profile, guest) are
    // skipped: the panel shows the primary user's state.
    std::optional<std::size_t> skipDeeperThan;

    for (std::size_t i = begin; i < end; ++i) {
        const std::string_view raw = trimRight(lines[i]);
        const std::string_view text = trim(raw);
        if (text.empty()) continue;
        const std::size_t indent = indentOf(raw);

        if (skipDeeperThan) {
            if (indent > *skipDeeperThan) continue;
            skipDeeperThan.reset();
        }
        if (list && indent > list->indent) {
            details.lists[list->name].emplace_back(text);
            continue;
        }
        list.reset();

        if (text.starts_with("User ")) {
            const auto colon = text.find(':');
            if (colon == std::string_view::npos) continue;
            const auto user = parseNumber<int>(text.substr(5, colon - 5));
            if (!user || *user != 0) {
                skipDeeperThan = indent;
                continue;
            }
            parseKeyValues(trim(text.substr(colon + 1)), details.fields);
            continue;
        }
        if (text.ends_with(':') && text.find('=') == std::string_view::npos) {
            std::string name{text.substr(0, text.size() - 1)};
            details.lists.try_emplace(name);
            list = OpenList{indent, std::move(name)};
            continue;
        }
        if (text.find('=') != std::string_view::npos) parseKeyValues(text, details.fields);
    }
}

void fillTypedFields(AppDetails& d) {
    const auto& f = d.fields;
    d.versionName = firstValue(f, "versionName");
    d.versionCode = toInt64(firstValue(f, "versionCode"));
    d.minSdk = static_cast<int>(toInt64(firstValue(f, "minSdk")));
    d.targetSdk = static_cast<int>(toInt64(firstValue(f, "targetSdk")));
    d.appId = static_cast<int>(toInt64(firstValue(f, "appId")));
    if (d.appId < 0) d.appId = static_cast<int>(toInt64(firstValue(f, "userId")));
    d.codePath = firstValue(f, "codePath");
    d.dataDir = firstValue(f, "dataDir");
    d.primaryCpuAbi = firstValue(f, "primaryCpuAbi");
    if (d.primaryCpuAbi == "null") d.primaryCpuAbi.clear();
    d.splits = parseBracketList(firstValue(f, "splits"));
    d.flags = parseBracketList(firstValue(f, "flags"));
    d.privateFlags = parseBracketList(firstValue(f, "privateFlags"));
    d.signingVersion = static_cast<int>(toInt64(firstValue(f, "apkSigningVersion")));
    d.installer = firstValue(f, "installerPackageName");
    d.lastUpdateTime = firstValue(f, "lastUpdateTime");
    d.firstInstallTime = firstValue(f, "firstInstallTime");
    if (d.firstInstallTime.empty()) d.firstInstallTime = firstValue(f, "timeStamp");

    // "SharedUserSetting{c09dcd8 android.uid.system/1000}" -> "android.uid.system"
    if (const std::string shared = firstValue(f, "sharedUser"); !shared.empty()) {
        const auto tokens = tokenize(shared);
        if (tokens.size() >= 2) {
            std::string_view name = tokens[1];
            if (const auto slash = name.find('/'); slash != std::string_view::npos) {
                name = name.substr(0, slash);
            }
            d.sharedUser = std::string{name};
        }
    }

    // "PackageSignatures{88d0831 version:3, signatures:[61c2bf66], past signatures:[]}"
    if (const std::string sigs = firstValue(f, "signatures"); !sigs.empty()) {
        if (const auto at = sigs.find(" signatures:["); at != std::string::npos) {
            const auto open = at + std::string_view{" signatures:"}.size();
            const auto close = sigs.find(']', open);
            if (close != std::string::npos) {
                d.signatures = parseBracketList(std::string_view{sigs}.substr(open, close - open + 1));
            }
        }
    }

    d.installed = firstValue(f, "installed") != "false";
    d.hidden = toBool(firstValue(f, "hidden"));
    d.suspended = toBool(firstValue(f, "suspended"));
    d.stopped = toBool(firstValue(f, "stopped"));
    d.notLaunched = toBool(firstValue(f, "notLaunched"));
    d.enabledState = static_cast<int>(toInt64(firstValue(f, "enabled"), 0));

    const auto listOr = [&d](const char* name) -> const std::vector<std::string>* {
        const auto it = d.lists.find(name);
        return it == d.lists.end() ? nullptr : &it->second;
    };
    if (const auto* items = listOr("requested permissions")) {
        for (const auto& item : *items) {
            // Newer releases append ": restricted=true" and the like.
            d.requestedPermissions.emplace_back(trim(std::string_view{item}.substr(0, item.find(':'))));
        }
    }
    if (const auto* items = listOr("declared permissions")) d.declaredPermissions = *items;
    if (const auto* items = listOr("install permissions")) {
        for (const auto& item : *items) d.installPermissions.push_back(parseGrant(item));
    }
    if (const auto* items = listOr("runtime permissions")) {
        for (const auto& item : *items) d.runtimePermissions.push_back(parseGrant(item));
    }
    if (const auto* items = listOr("enabledComponents")) d.enabledComponents = *items;
    if (const auto* items = listOr("disabledComponents")) d.disabledComponents = *items;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

bool isSafeAndroidName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 512) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_';
    });
}

bool AppDetails::isSystem() const { return hasFlag("SYSTEM"); }

bool AppDetails::hasFlag(std::string_view flag) const {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

std::vector<InstalledApp> parsePackageList(std::string_view text) {
    std::vector<InstalledApp> apps;
    for (const auto rawLine : splitLines(text)) {
        const auto rest = stripPrefix(trim(rawLine), "package:");
        if (!rest) continue;

        const auto tokens = tokenize(*rest);
        if (tokens.empty()) continue;

        InstalledApp app;
        // With -f the first token is "<apk path>=<package>". Paths under
        // /data/app contain '=' themselves ("~~kOIG...==/"), package names
        // never do, so the package starts after the LAST '='.
        const std::string_view head = tokens[0];
        if (const auto eq = head.rfind('='); eq != std::string_view::npos) {
            app.apkPath = std::string{head.substr(0, eq)};
            app.packageName = std::string{head.substr(eq + 1)};
        } else {
            app.packageName = std::string{head};
        }
        if (!isSafeAndroidName(app.packageName)) continue;

        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string_view token = tokens[i];
            if (const auto v = stripPrefix(token, "versionCode:")) {
                app.versionCode = toInt64(*v);
            } else if (const auto v2 = stripPrefix(token, "installer=")) {
                app.installer = std::string{*v2};
            } else if (const auto v3 = stripPrefix(token, "uid:")) {
                app.uid = static_cast<int>(toInt64(v3->substr(0, v3->find(','))));
            }
        }
        apps.push_back(std::move(app));
    }
    std::sort(apps.begin(), apps.end(), [](const InstalledApp& a, const InstalledApp& b) {
        return a.packageName < b.packageName;
    });
    return apps;
}

std::vector<std::string> parsePackageNames(std::string_view text) {
    std::vector<std::string> names;
    for (const auto rawLine : splitLines(text)) {
        if (const auto rest = stripPrefix(trim(rawLine), "package:")) {
            const auto tokens = tokenize(*rest);
            if (!tokens.empty()) names.emplace_back(tokens[0]);
        }
    }
    return names;
}

std::map<std::string, DiskStats> parseDiskStats(std::string_view text) {
    std::vector<std::string> names;
    std::vector<std::int64_t> app;
    std::vector<std::int64_t> data;
    std::vector<std::int64_t> cache;

    const auto numbers = [](std::string_view body) {
        std::vector<std::int64_t> out;
        for (const auto& part : parseBracketList(body)) out.push_back(toInt64(part));
        return out;
    };

    for (const auto rawLine : splitLines(text)) {
        const std::string_view line = trim(rawLine);
        if (const auto v = stripPrefix(line, "Package Names:")) {
            for (auto name : parseBracketList(*v)) {
                std::string_view n = name;
                if (n.size() >= 2 && n.front() == '"' && n.back() == '"') n = n.substr(1, n.size() - 2);
                names.emplace_back(n);
            }
        } else if (const auto v2 = stripPrefix(line, "App Sizes:")) {
            app = numbers(*v2);
        } else if (const auto v3 = stripPrefix(line, "App Data Sizes:")) {
            data = numbers(*v3);
        } else if (const auto v4 = stripPrefix(line, "Cache Sizes:")) {
            cache = numbers(*v4);
        }
    }

    std::map<std::string, DiskStats> out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        DiskStats stats;
        if (i < app.size()) stats.appBytes = app[i];
        if (i < data.size()) stats.dataBytes = data[i];
        if (i < cache.size()) stats.cacheBytes = cache[i];
        out[names[i]] = stats;
    }
    return out;
}

AppDetails parsePackageDump(std::string_view dump, std::string_view packageName) {
    AppDetails details;
    details.packageName = std::string{packageName};
    details.rawDump = std::string{dump};

    const auto lines = splitLines(dump);
    const std::string blockHeader = "Package [" + std::string{packageName} + "]";

    ComponentCollector components{packageName};
    Section section = Section::Other;
    AppComponent* current = nullptr;
    std::string pendingAuthority;
    std::string dexoptApk;
    bool inOwnDexopt = false;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view raw = trimRight(lines[i]);
        if (raw.empty()) continue;
        const std::size_t indent = indentOf(raw);
        const std::string_view text = trim(raw);

        if (indent == 0) {
            section = sectionFor(text);
            current = nullptr;
            continue;
        }

        switch (section) {
            case Section::Activities:
            case Section::Receivers:
            case Section::Services:
            case Section::Providers: {
                // "36766fd com.app/.Main filter e1e439f [permission X]"
                const auto tokens = tokenize(text);
                if (tokens.size() >= 3 && tokens[2] == "filter") {
                    current = nullptr;
                    if (const auto cls = componentClass(tokens[1], packageName)) {
                        const auto kind = section == Section::Activities ? AppComponent::Kind::Activity
                                          : section == Section::Receivers ? AppComponent::Kind::Receiver
                                          : section == Section::Services  ? AppComponent::Kind::Service
                                                                          : AppComponent::Kind::Provider;
                        current = components.get(kind, *cls);
                        for (std::size_t t = 1; t < tokens.size(); ++t) {
                            if (tokens[t - 1] == "permission") current->permission = std::string{tokens[t]};
                        }
                    }
                } else if (current != nullptr) {
                    if (const auto action = stripPrefix(text, "Action: ")) {
                        std::string_view a = trim(*action);
                        if (a.size() >= 2 && a.front() == '"' && a.back() == '"') a = a.substr(1, a.size() - 2);
                        addUnique(current->actions, std::string{a});
                    }
                }
                break;
            }
            case Section::RegisteredProviders: {
                // "  com.app/com.app.Provider:"
                if (indent == 2) {
                    if (const auto cls = componentClass(text, packageName)) {
                        components.get(AppComponent::Kind::Provider, *cls);
                    }
                }
                break;
            }
            case Section::ProviderAuthorities: {
                // "  [authority]:" then "    Provider{hash com.app/cls}"
                if (indent == 2 && text.starts_with('[')) {
                    const auto close = text.find(']');
                    pendingAuthority = close == std::string_view::npos
                                           ? std::string{}
                                           : std::string{text.substr(1, close - 1)};
                } else if (!pendingAuthority.empty() && text.starts_with("Provider{")) {
                    const auto tokens = tokenize(text);
                    if (tokens.size() >= 2) {
                        if (const auto cls = componentClass(tokens[1], packageName)) {
                            addUnique(components.get(AppComponent::Kind::Provider, *cls)->authorities,
                                      pendingAuthority);
                        }
                    }
                    pendingAuthority.clear();
                }
                break;
            }
            case Section::Packages:
            case Section::HiddenPackages: {
                if (indent != 2 || !text.starts_with(blockHeader)) break;
                std::size_t end = i + 1;
                while (end < lines.size()) {
                    const std::string_view next = trimRight(lines[end]);
                    if (!next.empty() && indentOf(next) <= 2) break;
                    ++end;
                }
                if (section == Section::Packages && !details.found) {
                    details.found = true;
                    parsePackageBlock(lines, i + 1, end, details);
                } else if (section == Section::HiddenPackages) {
                    details.updatedSystemApp = true;
                    AppDetails factory;
                    parsePackageBlock(lines, i + 1, end, factory);
                    details.factoryVersionName = firstValue(factory.fields, "versionName");
                }
                i = end - 1;
                break;
            }
            case Section::Dexopt: {
                if (indent == 2) {
                    // "  [com.app]" -- only the requested package's entries.
                    inOwnDexopt = text == "[" + std::string{packageName} + "]";
                    break;
                }
                if (!inOwnDexopt) break;
                if (const auto path = stripPrefix(text, "path: ")) {
                    dexoptApk = baseName(trim(*path));
                } else if (!dexoptApk.empty() && !text.starts_with('[')) {
                    // "arm64: [status=speed-profile] [reason=cmdline] [primary-abi]"
                    // or the older "arm64: speed-profile".
                    const auto colon = text.find(':');
                    if (colon == std::string_view::npos) break;
                    DexoptEntry entry;
                    entry.apk = dexoptApk;
                    entry.isa = std::string{text.substr(0, colon)};
                    const std::string_view rest = trim(text.substr(colon + 1));
                    const auto bracketValue = [rest](std::string_view key) -> std::string {
                        const auto at = rest.find(key);
                        if (at == std::string_view::npos) return {};
                        const auto start = at + key.size();
                        const auto close = rest.find(']', start);
                        return std::string{rest.substr(start, close == std::string_view::npos
                                                                   ? std::string_view::npos
                                                                   : close - start)};
                    };
                    entry.status = bracketValue("[status=");
                    entry.reason = bracketValue("[reason=");
                    if (entry.status.empty()) entry.status = std::string{rest};
                    details.dexopt.push_back(std::move(entry));
                }
                break;
            }
            case Section::Other:
                break;
        }
    }

    details.components = components.take();
    if (details.found) {
        fillTypedFields(details);
    } else {
        details.error = "the device reports no package named " + std::string{packageName};
    }
    return details;
}

std::vector<ApkFile> parseApkSizes(std::string_view statOutput) {
    std::vector<ApkFile> apks;
    for (const auto rawLine : splitLines(statOutput)) {
        const std::string_view line = trim(rawLine);
        const auto space = line.find(' ');
        if (space == std::string_view::npos) continue;
        const auto size = parseNumber<std::int64_t>(line.substr(0, space));
        if (!size) continue;
        apks.push_back(ApkFile{std::string{trim(line.substr(space + 1))}, *size});
    }
    // Base first, then splits by name -- the order `pm path` uses varies.
    std::stable_sort(apks.begin(), apks.end(), [](const ApkFile& a, const ApkFile& b) {
        const bool aBase = baseName(a.path) == "base.apk";
        const bool bBase = baseName(b.path) == "base.apk";
        if (aBase != bBase) return aBase;
        return a.path < b.path;
    });
    return apks;
}

std::string parseLauncherActivity(std::string_view resolveOutput) {
    std::string found;
    for (const auto rawLine : splitLines(resolveOutput)) {
        const std::string_view line = trim(rawLine);
        if (line.empty() || line.find('=') != std::string_view::npos) continue;
        if (line.find(' ') != std::string_view::npos) continue;  // "No activity found"
        if (line.find('/') != std::string_view::npos) found = std::string{line};
    }
    return found;
}

std::vector<std::pair<std::string, std::string>> parseAppOps(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> ops;
    for (const auto rawLine : splitLines(text)) {
        std::string_view line = trim(rawLine);
        std::string suffix;
        if (const auto uid = stripPrefix(line, "Uid mode: ")) {
            line = *uid;
            suffix = " (whole app)";
        }
        const auto sep = line.find(": ");
        if (sep == std::string_view::npos || sep == 0) continue;
        const std::string_view name = line.substr(0, sep);
        // Op names are SCREAMING_SNAKE; skip anything else the tool prints.
        if (!std::all_of(name.begin(), name.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            })) {
            continue;
        }
        ops.emplace_back(std::string{name} + suffix, std::string{trim(line.substr(sep + 2))});
    }
    return ops;
}

std::vector<MemoryRow> parseMemorySummary(std::string_view text) {
    std::vector<MemoryRow> rows;
    for (const auto rawLine : splitLines(text)) {
        const std::string_view line = trim(rawLine);
        if (line.starts_with("TOTAL PSS:")) {
            // "TOTAL PSS:   477443            TOTAL RSS:   423416       TOTAL SWAP PSS:   149829"
            MemoryRow total;
            total.label = "Total";
            const auto valueAfter = [line](std::string_view key) -> std::int64_t {
                const auto at = line.find(key);
                if (at == std::string_view::npos) return -1;
                const auto tokens = tokenize(line.substr(at + key.size()));
                return tokens.empty() ? -1 : toInt64(tokens[0]);
            };
            total.pssKb = valueAfter("TOTAL PSS:");
            total.rssKb = valueAfter("TOTAL RSS:");
            rows.push_back(total);
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        const std::string_view label = line.substr(0, colon);
        if (label.find("App Summary") != std::string_view::npos) continue;

        // The PSS and RSS columns are aligned under headers, and a row can
        // lack either ("Unknown:" has RSS only). Tell them apart by column.
        const std::string_view rest = line.substr(colon + 1);
        const auto tokens = tokenize(rest);
        if (tokens.empty()) continue;
        MemoryRow row;
        row.label = std::string{label};
        const std::size_t rawColon = rawLine.find(':');
        const std::size_t firstValueEnd =
            static_cast<std::size_t>(tokens[0].data() - rawLine.data()) + tokens[0].size();
        // Values more than ~20 columns past the colon sit in the RSS column.
        const bool firstIsRss = firstValueEnd - rawColon > 20;
        if (firstIsRss) {
            row.rssKb = toInt64(tokens[0]);
        } else {
            row.pssKb = toInt64(tokens[0]);
            if (tokens.size() >= 2) row.rssKb = toInt64(tokens[1]);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// AppInspector
// ---------------------------------------------------------------------------

AppInspector::AppInspector(DeviceManager& devices) : devices_(devices) {}

void AppInspector::refreshList(ThreadPool& pool, Dispatcher& dispatcher,
                               std::function<void(AppListResult)> onDone) {
    const std::string serial = devices_.selectedSerial();
    pool.submit([this, &dispatcher, serial, onDone = std::move(onDone)]() mutable {
        AppListResult result;
        if (serial.empty()) {
            result.error = "no device selected";
        } else {
            // The flags arrived over several releases (-U in 8, --show-versioncode
            // in 9); an older pm rejects the whole command, so fall back to -f.
            const std::array<std::string, 5> commands{
                "out=$(pm list packages -f -U -i --show-versioncode 2>&1); "
                "case \"$out\" in *package:*) echo \"$out\";; *) pm list packages -f;; esac",
                "pm list packages -3",
                "pm list packages -d",
                "ps -A -o NAME= 2>/dev/null || ps -o NAME",
                "dumpsys diskstats | grep -E '^(Package Names|App Sizes|App Data Sizes|Cache Sizes):'",
            };
            auto outputs = devices_.adb().shellBatch(serial, commands, kAdbLongTimeout);
            if (!outputs) {
                result.error = outputs.error().message;
            } else {
                const auto& out = outputs.value();
                result.apps = parsePackageList(out[0]);
                if (result.apps.empty()) {
                    result.error = "pm returned no packages";
                } else {
                    const auto third = parsePackageNames(out[1]);
                    const std::set<std::string> userApps{third.begin(), third.end()};
                    const auto disabled = parsePackageNames(out[2]);
                    const std::set<std::string> disabledApps{disabled.begin(), disabled.end()};
                    std::set<std::string> processes;
                    for (const auto line : splitLines(out[3])) {
                        std::string_view name = trim(line);
                        // "com.app:remote" is a secondary process of com.app.
                        if (const auto colon = name.find(':'); colon != std::string_view::npos) {
                            name = name.substr(0, colon);
                        }
                        if (!name.empty()) processes.emplace(name);
                    }
                    const auto sizes = parseDiskStats(out[4]);

                    for (auto& app : result.apps) {
                        app.system = !userApps.contains(app.packageName);
                        app.disabled = disabledApps.contains(app.packageName);
                        app.running = processes.contains(app.packageName);
                        if (const auto it = sizes.find(app.packageName); it != sizes.end()) {
                            app.appBytes = it->second.appBytes;
                            app.dataBytes = it->second.dataBytes;
                            app.cacheBytes = it->second.cacheBytes;
                        }
                    }
                }
            }
        }
        dispatcher.post([cb = std::move(onDone), r = std::move(result)] { cb(r); });
    });
}

void AppInspector::loadDetails(ThreadPool& pool, Dispatcher& dispatcher, std::string packageName,
                               std::function<void(AppDetails)> onDone) {
    const std::string serial = devices_.selectedSerial();
    pool.submit([this, &dispatcher, serial, packageName = std::move(packageName),
                 onDone = std::move(onDone)]() mutable {
        AppDetails details;
        details.packageName = packageName;

        if (serial.empty()) {
            details.error = "no device selected";
        } else if (!isSafeAndroidName(packageName)) {
            details.error = "not a valid package name: " + packageName;
        } else {
            const std::string& p = packageName;
            const std::array<std::string, 7> commands{
                "dumpsys package " + p,
                "for f in $(pm path " + p + " | sed 's/^package://'); do stat -c '%s %n' \"$f\"; done",
                "pidof " + p,
                "cmd package resolve-activity --brief -a android.intent.action.MAIN "
                "-c android.intent.category.LAUNCHER " + p,
                "cmd appops get " + p,
                // Only when it runs: meminfo on a stopped app prints nothing useful.
                "if pidof " + p + " >/dev/null; then dumpsys meminfo " + p +
                    " | sed -n '/App Summary/,/TOTAL SWAP/p'; fi",
                "dumpsys diskstats | grep -E '^(Package Names|App Sizes|App Data Sizes|Cache Sizes):'",
            };
            auto outputs = devices_.adb().shellBatch(serial, commands, kAdbLongTimeout);
            if (!outputs) {
                details.error = outputs.error().message;
            } else {
                const auto& out = outputs.value();
                details = parsePackageDump(out[0], p);
                details.apks = parseApkSizes(out[1]);
                for (const auto token : tokenize(out[2])) {
                    if (const auto pid = parseNumber<int>(token)) details.pids.push_back(*pid);
                }
                details.launcherActivity = parseLauncherActivity(out[3]);
                details.appOps = parseAppOps(out[4]);
                details.memory = parseMemorySummary(out[5]);
                const auto sizes = parseDiskStats(out[6]);
                if (const auto it = sizes.find(p); it != sizes.end()) {
                    details.appBytes = it->second.appBytes;
                    details.dataBytes = it->second.dataBytes;
                    details.cacheBytes = it->second.cacheBytes;
                }
            }
        }
        dispatcher.post([cb = std::move(onDone), d = std::move(details)] { cb(d); });
    });
}

void AppInspector::runAction(ThreadPool& pool, Dispatcher& dispatcher, AppAction action,
                             std::string packageName, std::string argument,
                             std::function<void(AppActionResult)> onDone) {
    const std::string serial = devices_.selectedSerial();
    pool.submit([this, &dispatcher, serial, action, packageName = std::move(packageName),
                 argument = std::move(argument), onDone = std::move(onDone)]() mutable {
        AppActionResult result;
        const std::string& p = packageName;

        std::string command;
        std::string doneMessage;
        if (serial.empty()) {
            result.message = "no device selected";
        } else if (!isSafeAndroidName(p)) {
            result.message = "not a valid package name: " + p;
        } else {
            switch (action) {
                case AppAction::Launch: {
                    // Component names may also hold '/' and '$' (inner classes).
                    const bool safeComponent =
                        !argument.empty() &&
                        std::all_of(argument.begin(), argument.end(), [](char c) {
                            return isSafeAndroidName(std::string_view{&c, 1}) || c == '/' || c == '$';
                        });
                    command = safeComponent
                                  ? "am start -n " + shellQuote(argument)
                                  : "monkey -p " + p + " -c android.intent.category.LAUNCHER 1";
                    doneMessage = "launched " + p;
                    break;
                }
                case AppAction::ForceStop:
                    command = "am force-stop " + p;
                    doneMessage = "stopped " + p;
                    break;
                case AppAction::OpenSettings:
                    command = "am start -a android.settings.APPLICATION_DETAILS_SETTINGS -d package:" + p;
                    doneMessage = "opened the app's settings page on the device";
                    break;
                case AppAction::ClearData:
                    command = "pm clear " + p;
                    doneMessage = "cleared all data of " + p;
                    break;
                case AppAction::Uninstall:
                    command = "pm uninstall " + p;
                    doneMessage = "uninstalled " + p;
                    break;
                case AppAction::GrantPermission:
                case AppAction::RevokePermission: {
                    if (!isSafeAndroidName(argument)) {
                        result.message = "not a valid permission name: " + argument;
                        break;
                    }
                    const bool grant = action == AppAction::GrantPermission;
                    command = std::string{grant ? "pm grant " : "pm revoke "} + p + " " + argument;
                    doneMessage = std::string{grant ? "granted " : "revoked "} + argument;
                    break;
                }
            }
        }

        if (!command.empty()) {
            EM_LOG_INFO(kCategory, command);
            auto output = devices_.adb().shellRaw(serial, command, kAdbNormalTimeout);
            if (!output) {
                result.message = output.error().message;
            } else {
                const CommandOutput& o = output.value();
                const std::string combined = o.out + o.err;
                // pm and am report most failures on stdout with exit code 0.
                const bool failed = !o.ok() || containsIgnoreCase(combined, "Failure") ||
                                    containsIgnoreCase(combined, "Exception") ||
                                    containsIgnoreCase(combined, "Error:") ||
                                    containsIgnoreCase(combined, "No activities found");
                result.ok = !failed;
                if (failed) {
                    // Prefer the reason ("java.lang.SecurityException: Permission X
                    // is not a changeable permission type") over the generic
                    // "Exception occurred while executing 'grant':" above it.
                    std::string reason;
                    std::string first;
                    for (const auto line : splitLines(combined)) {
                        const auto t = trim(line);
                        if (t.empty()) continue;
                        if (first.empty()) first = std::string{t};
                        if (const auto at = t.find("Exception: "); at != std::string_view::npos) {
                            reason = std::string{t.substr(at + 11)};
                            break;
                        }
                        if (reason.empty() && (t.starts_with("Failure") || t.starts_with("Error"))) {
                            reason = std::string{t};
                        }
                    }
                    result.message = !reason.empty() ? reason
                                     : !first.empty() ? first
                                                      : std::string{"the device refused"};
                } else {
                    result.message = doneMessage;
                }
            }
        }
        if (!result.ok) EM_LOG_WARN(kCategory, result.message);
        dispatcher.post([cb = std::move(onDone), r = std::move(result)] { cb(r); });
    });
}

namespace {

// Byte ranges come back base64-encoded, one line per range, through a plain
// `adb shell`: text survives every adb version and transport, where raw
// binary over `exec-out` does not.
std::string rangeCommand(const std::string& path, std::uint64_t offset, std::uint64_t length) {
    return "dd if=" + shellQuote(path) + " iflag=skip_bytes,count_bytes skip=" +
           std::to_string(offset) + " count=" + std::to_string(length) + " bs=65536 2>/dev/null";
}

std::string entryHeadCommand(const std::string& path, const std::string& entry, std::size_t bytes) {
    return "unzip -p " + shellQuote(path) + " " + shellQuote(entry) + " 2>/dev/null | head -c " +
           std::to_string(bytes);
}

// Runs every command, returning each one's stdout bytes in order.
Result<std::vector<std::string>> fetchChunks(AdbClient& adb, const std::string& serial,
                                             const std::vector<std::string>& commands) {
    std::vector<std::string> out;
    if (commands.empty()) return out;
    std::string script;
    for (const auto& command : commands) {
        script += "{ " + command + " ; } | base64 -w0; echo; ";
    }
    EM_TRY(text, adb.shell(serial, script, kAdbLongTimeout));
    const auto lines = splitLines(text);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const std::string_view line = i < lines.size() ? lines[i] : std::string_view{};
        auto bytes = base64Decode(line);
        if (!bytes) return makeError(ErrorKind::ParseFailure, "the device returned unreadable data");
        out.push_back(std::move(*bytes));
    }
    return out;
}

std::string fileName(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Anything bigger is not an APK index we should be pulling over adb.
constexpr std::uint64_t kMaxDirectoryBytes = 32ULL << 20;
constexpr std::uint64_t kMaxSigningBlockBytes = 16ULL << 20;
constexpr std::uint64_t kSigningBlockGuess = 256ULL << 10;
constexpr std::size_t kElfHeadBytes = 4096;
constexpr std::size_t kMaxLibraries = 400;

}  // namespace

void AppInspector::analyzeApks(ThreadPool& pool, Dispatcher& dispatcher, std::string packageName,
                               std::vector<ApkFile> apks, std::function<void(ApkContents)> onDone) {
    const std::string serial = devices_.selectedSerial();
    pool.submit([this, &dispatcher, serial, packageName = std::move(packageName),
                 apks = std::move(apks), onDone = std::move(onDone)]() mutable {
        ApkContents result;
        result.packageName = packageName;
        const auto finish = [&] {
            dispatcher.post([cb = std::move(onDone), r = std::move(result)] { cb(r); });
        };
        if (serial.empty()) {
            result.error = "no device selected";
            return finish();
        }
        std::erase_if(apks, [](const ApkFile& a) { return a.sizeBytes <= 0; });
        if (apks.empty()) {
            result.error = "the device listed no APK files for this app";
            return finish();
        }
        AdbClient& adb = devices_.adb();

        // 1. The tail of every APK, for the End Of Central Directory record.
        std::vector<std::string> commands;
        std::vector<std::uint64_t> tailOffsets;
        for (const auto& apk : apks) {
            const auto size = static_cast<std::uint64_t>(apk.sizeBytes);
            const std::uint64_t length = std::min<std::uint64_t>(size, 65557 + 20 + 56);
            tailOffsets.push_back(size - length);
            commands.push_back(rangeCommand(apk.path, size - length, length));
        }
        auto tails = fetchChunks(adb, serial, commands);
        if (!tails) {
            result.error = tails.error().message;
            return finish();
        }

        std::vector<std::optional<ZipDirectory>> dirs;
        for (std::size_t i = 0; i < apks.size(); ++i) {
            dirs.push_back(findZipDirectory(tails.value()[i], tailOffsets[i]));
        }
        if (!dirs[0]) {
            result.error = "could not read the ZIP index of " + fileName(apks[0].path) +
                           " (is `dd` with iflag=skip_bytes available?)";
            return finish();
        }

        // 2. Each central directory; for base.apk also the bytes just before
        //    it, where the APK Signing Block lives.
        commands.clear();
        std::vector<std::uint64_t> preBytes(apks.size(), 0);
        for (std::size_t i = 0; i < apks.size(); ++i) {
            if (!dirs[i] || dirs[i]->centralDirectorySize > kMaxDirectoryBytes) {
                commands.emplace_back("true");
                continue;
            }
            const ZipDirectory& d = *dirs[i];
            preBytes[i] = i == 0 ? std::min(d.centralDirectoryOffset, kSigningBlockGuess) : 0;
            commands.push_back(rangeCommand(apks[i].path, d.centralDirectoryOffset - preBytes[i],
                                            preBytes[i] + d.centralDirectorySize));
        }
        auto directories = fetchChunks(adb, serial, commands);
        if (!directories) {
            result.error = directories.error().message;
            return finish();
        }

        std::vector<std::vector<ZipEntry>> entries(apks.size());
        for (std::size_t i = 0; i < apks.size(); ++i) {
            const std::string& bytes = directories.value()[i];
            if (bytes.size() < preBytes[i]) continue;
            entries[i] = parseCentralDirectory(std::string_view{bytes}.substr(preBytes[i]));
        }

        // The signing block, re-fetched whole if the first guess was short.
        {
            const std::string& bytes = directories.value()[0];
            result.signing = parseSigningBlock(std::string_view{bytes}.substr(0, std::min<std::size_t>(bytes.size(), preBytes[0])));
            const std::uint64_t needed = result.signing.bytesNeeded;
            if (needed > 0 && needed <= kMaxSigningBlockBytes &&
                needed <= dirs[0]->centralDirectoryOffset) {
                auto block = fetchChunks(adb, serial,
                                         {rangeCommand(apks[0].path,
                                                       dirs[0]->centralDirectoryOffset - needed, needed)});
                if (block) result.signing = parseSigningBlock(block.value()[0]);
            }
            if (result.signing.bytesNeeded > 0) result.signing.error = "the APK Signing Block is too large to read";
        }

        // 3. The head of every native library, the v1 certificate if that is
        //    the only signature, in one round trip.
        commands.clear();
        struct Pending {
            std::size_t library;
            bool localHeader;
        };
        std::vector<Pending> pending;
        std::string v1CertEntry;
        for (std::size_t i = 0; i < apks.size(); ++i) {
            for (const auto& e : entries[i]) {
                if (i == 0 && e.name.starts_with("META-INF/")) {
                    const std::string upper = toLower(e.name);
                    if (upper.ends_with(".sf")) result.hasV1Signature = true;
                    if (v1CertEntry.empty() && (upper.ends_with(".rsa") || upper.ends_with(".dsa") ||
                                                upper.ends_with(".ec"))) {
                        v1CertEntry = e.name;
                    }
                }
                if (!e.name.starts_with("lib/") || !e.name.ends_with(".so")) continue;
                const auto slash = e.name.find('/', 4);
                if (slash == std::string::npos) continue;
                if (result.libraries.size() >= kMaxLibraries) break;

                NativeLibrary lib;
                lib.apk = fileName(apks[i].path);
                lib.abi = e.name.substr(4, slash - 4);
                lib.name = e.name.substr(slash + 1);
                lib.sizeBytes = e.uncompressedSize;
                lib.compressed = e.method != 0;
                result.libraries.push_back(std::move(lib));
                const std::size_t index = result.libraries.size() - 1;

                commands.push_back(entryHeadCommand(apks[i].path, e.name, kElfHeadBytes));
                pending.push_back({index, false});
                if (e.method == 0) {
                    // Only 30 fixed bytes are needed: name and extra lengths
                    // give where the data starts.
                    commands.push_back(rangeCommand(apks[i].path, e.localHeaderOffset, 30));
                    pending.push_back({index, true});
                }
            }
        }
        const bool needV1 = result.signing.signers.empty() && !v1CertEntry.empty();
        if (needV1) commands.push_back(entryHeadCommand(apks[0].path, v1CertEntry, 1U << 20));

        auto heads = fetchChunks(adb, serial, commands);
        if (!heads) {
            result.error = heads.error().message;
            return finish();
        }
        std::size_t c = 0;
        for (const auto& p : pending) {
            NativeLibrary& lib = result.libraries[p.library];
            const std::string& bytes = heads.value()[c++];
            if (p.localHeader) {
                // The local header's own offset is the one the central
                // directory gave; recover it from the entry list.
                for (std::size_t i = 0; i < apks.size(); ++i) {
                    if (fileName(apks[i].path) != lib.apk) continue;
                    for (const auto& e : entries[i]) {
                        if (e.name == "lib/" + lib.abi + "/" + lib.name) {
                            if (const auto offset = zipDataOffset(bytes, e.localHeaderOffset)) {
                                lib.dataOffset = static_cast<std::int64_t>(*offset);
                            }
                        }
                    }
                }
            } else {
                lib.elf = parseElfHeader(bytes);
            }
        }
        if (needV1) {
            for (const auto& der : certificatesFromPkcs7(heads.value()[c])) {
                result.v1Certificates.push_back(parseCertificate(der));
            }
        }

        std::stable_sort(result.libraries.begin(), result.libraries.end(),
                         [](const NativeLibrary& a, const NativeLibrary& b) {
                             if (a.abi != b.abi) return a.abi < b.abi;
                             return a.name < b.name;
                         });
        finish();
    });
}

void AppInspector::pullApks(ThreadPool& pool, Dispatcher& dispatcher,
                            std::vector<std::string> remotePaths,
                            std::filesystem::path destination,
                            std::function<void(AppActionResult)> onDone) {
    const std::string serial = devices_.selectedSerial();
    pool.submit([this, &dispatcher, serial, remotePaths = std::move(remotePaths),
                 destination = std::move(destination), onDone = std::move(onDone)]() mutable {
        AppActionResult result;
        std::error_code ec;
        std::filesystem::create_directories(destination, ec);
        if (serial.empty()) {
            result.message = "no device selected";
        } else if (ec) {
            result.message = "cannot create " + destination.string() + ": " + ec.message();
        } else {
            std::size_t pulled = 0;
            for (const auto& remote : remotePaths) {
                auto status = devices_.adb().pull(serial, remote, destination / baseName(remote),
                                                  kApkPullTimeout);
                if (!status) {
                    result.message = "pulling " + baseName(remote) + " failed: " +
                                     status.error().message;
                    break;
                }
                ++pulled;
            }
            if (pulled == remotePaths.size()) {
                result.ok = true;
                result.message = "saved " + std::to_string(pulled) +
                                 (pulled == 1 ? " APK to " : " APKs to ") + destination.string();
            }
        }
        dispatcher.post([cb = std::move(onDone), r = std::move(result)] { cb(r); });
    });
}

}  // namespace em
