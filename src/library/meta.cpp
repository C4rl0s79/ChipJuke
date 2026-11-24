// vgmplay - library: metadane utworow (readTrackMeta) i cache JSON.
#include <windows.h>

#include <algorithm>
#include <new>

#include <nlohmann/json.hpp>

#include "vgmplay/library.hpp"
#include "vgmplay/loader.hpp"

#include "fs_util.hpp"
#include "json_limits.hpp"

namespace vgmplay::library {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint32_t kCacheSchemaVersion = 1;
constexpr std::size_t kMaxCacheItems = 200000;
constexpr std::uint64_t kMaxCacheFileBytes = 64ull << 20;
constexpr int kMaxJsonDepth = 8;                 // root, items, wpis = 3 poziomy
constexpr std::size_t kMaxJsonValues = 4000000;
constexpr std::uint64_t kTicksPerSecond = 44100;   // format::kVgmSampleRate

std::wstring keyOf(const fs::path& p)
{
    std::wstring w = p.generic_wstring();
    if (!w.empty()) ::CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    return w;
}

std::uint64_t ticksToMs(std::uint64_t ticks) { return ticks / kTicksPerSecond * 1000 + ticks % kTicksPerSecond * 1000 / kTicksPerSecond; }

void putStr(json& o, const char* key, const std::string& v)
{
    if (!v.empty()) o[key] = v;
}

std::string getStr(const json& o, const char* key)
{
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? fsutil::sanitizeTitle(it->get_ref<const std::string&>()) : std::string();
}

} // namespace

Result<FileStamp> statFile(const fs::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return fsutil::winError("cannot stat file", ::GetLastError());
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return makeError(ErrorCode::InvalidArgument, "path is a directory");
    FileStamp s;
    s.sizeBytes = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    s.mtime = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return s;
}

bool isCloudPlaceholderError(const Error& e) noexcept
{
    return e.code == ErrorCode::AccessDenied && e.message.rfind("cloud placeholder", 0) == 0;
}

Result<TrackMeta> readTrackMeta(const fs::path& path, const CancelToken& cancel, bool allowCloudDownload)
{
    try {
        if (!allowCloudDownload) {
            const DWORD a = ::GetFileAttributesW(path.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && isCloudPlaceholder(a))
                return makeError(ErrorCode::AccessDenied, "cloud placeholder (content not local); download not requested");
        }
        auto stamp = statFile(path);   // przed odczytem: zmiana w trakcie => cache niezgodny przy nastepnym uzyciu
        if (!stamp.ok()) return stamp.error();
        auto lt = loader::loadTrackFromFile(path, {}, cancel);
        if (!lt.ok()) return lt.error();
        const loader::LoadedTrack& t = lt.value();
        TrackMeta m;
        m.path = path;
        m.stamp = stamp.value();
        m.durationMs = fsutil::clampDurationMs(ticksToMs(t.stream.totalSamples));
        if (t.header.hasLoop() && !t.stream.loopDisabled) m.loopMs = fsutil::clampDurationMs(ticksToMs(t.header.loopSamples));
        m.hasGd3 = t.gd3.present;
        m.trackEn = t.gd3.trackEn;   m.trackJp = t.gd3.trackJp;
        m.gameEn = t.gd3.gameEn;     m.gameJp = t.gd3.gameJp;
        m.systemEn = t.gd3.systemEn; m.systemJp = t.gd3.systemJp;
        m.authorEn = t.gd3.authorEn; m.authorJp = t.gd3.authorJp;
        return m;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory reading track metadata");
    }
}

std::optional<TrackMeta> MetaCache::find(const fs::path& path, const FileStamp& stamp) const
{
    auto it = items_.find(keyOf(path));
    if (it == items_.end() || !(it->second.stamp == stamp)) return std::nullopt;
    return it->second;
}

void MetaCache::put(const TrackMeta& meta)
{
    try {
        std::wstring key = keyOf(meta.path);
        auto it = items_.find(key);
        if (it != items_.end()) { it->second = meta; return; }
        if (items_.size() >= kMaxCacheItems) return;
        items_.emplace(std::move(key), meta);
    } catch (const std::bad_alloc&) {
        // cache jest opcjonalny: przy braku pamieci po prostu nie dodajemy wpisu
    }
}

Result<TrackMeta> MetaCache::getOrRead(const fs::path& path, const CancelToken& cancel, bool allowCloudDownload)
{
    auto stamp = statFile(path);
    if (!stamp.ok()) return stamp.error();
    if (auto hit = find(path, stamp.value())) return std::move(*hit);
    auto m = readTrackMeta(path, cancel, allowCloudDownload);
    if (m.ok()) put(m.value());
    return m;
}

std::string MetaCache::toJson() const
{
    // Kolejnosc deterministyczna: sortowanie po kluczu (unordered_map nie ma kolejnosci).
    std::vector<const std::pair<const std::wstring, TrackMeta>*> sorted;
    sorted.reserve(items_.size());
    for (const auto& kv : items_) sorted.push_back(&kv);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->first < b->first; });

    json root = json::object();
    root["schemaVersion"] = kCacheSchemaVersion;
    json arr = json::array();
    for (const auto* kv : sorted) {
        const TrackMeta& m = kv->second;
        json o = json::object();
        o["path"] = fsutil::pathToUtf8(m.path);
        o["size"] = m.stamp.sizeBytes;
        o["mtime"] = m.stamp.mtime;
        o["durationMs"] = m.durationMs;
        if (m.loopMs) o["loopMs"] = m.loopMs;
        if (m.hasGd3) o["gd3"] = true;
        putStr(o, "trackEn", m.trackEn);   putStr(o, "trackJp", m.trackJp);
        putStr(o, "gameEn", m.gameEn);     putStr(o, "gameJp", m.gameJp);
        putStr(o, "systemEn", m.systemEn); putStr(o, "systemJp", m.systemJp);
        putStr(o, "authorEn", m.authorEn); putStr(o, "authorJp", m.authorJp);
        arr.push_back(std::move(o));
    }
    root["items"] = std::move(arr);
    return root.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
}

Result<MetaCache> MetaCache::fromJson(std::string_view text, bool allowNetworkPaths)
{
    try {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            text.remove_prefix(3);
        auto parsed = fsutil::parseJsonLimited(text, kMaxJsonDepth, kMaxJsonValues);
        if (!parsed.ok()) return parsed.error();
        const json& root = parsed.value();
        if (!root.is_object()) return makeError(ErrorCode::ParseError, "invalid cache JSON");
        auto ver = root.find("schemaVersion");
        if (ver == root.end() || !ver->is_number_unsigned()) return makeError(ErrorCode::ParseError, "missing schemaVersion");
        if (ver->get<std::uint64_t>() > kCacheSchemaVersion) return makeError(ErrorCode::UnsupportedSchema, "cache schema is newer than supported");
        auto items = root.find("items");
        if (items == root.end() || !items->is_array()) return makeError(ErrorCode::ParseError, "missing items array");
        MetaCache c;
        c.items_.reserve((std::min<std::size_t>)(items->size(), kMaxCacheItems));
        for (const json& o : *items) {
            if (c.items_.size() >= kMaxCacheItems) break;
            if (!o.is_object()) continue;
            auto p = o.find("path");
            auto sz = o.find("size");
            auto mt = o.find("mtime");
            if (p == o.end() || !p->is_string() || sz == o.end() || !sz->is_number_unsigned() || mt == o.end() || !mt->is_number_unsigned())
                continue;   // niepelny wpis cache: pomijamy (zostanie odczytany ponownie)
            const std::string& pathStr = p->get_ref<const std::string&>();
            if (fsutil::isUnsafeStoredPath(pathStr, allowNetworkPaths)) continue;
            TrackMeta m;
            m.path = fsutil::pathFromUtf8(pathStr);
            if (m.path.empty()) continue;
            m.stamp.sizeBytes = sz->get<std::uint64_t>();
            m.stamp.mtime = mt->get<std::uint64_t>();
            if (auto d = o.find("durationMs"); d != o.end() && d->is_number_unsigned()) m.durationMs = fsutil::clampDurationMs(d->get<std::uint64_t>());
            if (auto d = o.find("loopMs"); d != o.end() && d->is_number_unsigned()) m.loopMs = fsutil::clampDurationMs(d->get<std::uint64_t>());
            if (auto d = o.find("gd3"); d != o.end() && d->is_boolean()) m.hasGd3 = d->get<bool>();
            m.trackEn = getStr(o, "trackEn");   m.trackJp = getStr(o, "trackJp");
            m.gameEn = getStr(o, "gameEn");     m.gameJp = getStr(o, "gameJp");
            m.systemEn = getStr(o, "systemEn"); m.systemJp = getStr(o, "systemJp");
            m.authorEn = getStr(o, "authorEn"); m.authorJp = getStr(o, "authorJp");
            std::wstring key = keyOf(m.path);
            c.items_[std::move(key)] = std::move(m);   // przy duplikatach wygrywa ostatni wpis
        }
        return c;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory parsing cache");
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::ParseError, std::string("cache JSON: ") + ex.what());
    }
}

Result<MetaCache> loadMetaCacheFile(const fs::path& file, bool allowNetworkPaths)
{
    auto text = fsutil::readFileLimited(file, kMaxCacheFileBytes);
    if (!text.ok()) {
        if (text.error().code == ErrorCode::NotFound) return MetaCache{};
        return text.error();
    }
    return MetaCache::fromJson(text.value(), allowNetworkPaths);
}

Result<void> saveMetaCacheFile(const fs::path& file, const MetaCache& cache)
{
    try {
        return fsutil::atomicWriteFile(file, cache.toJson());
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory saving cache");
    }
}

} // namespace vgmplay::library
