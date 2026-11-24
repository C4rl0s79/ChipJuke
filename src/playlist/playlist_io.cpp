// vgmplay - playlist: JSON (saveJson/loadJson) i pliki (loadPlaylistsFile/savePlaylistsFile).
#include <algorithm>
#include <cctype>
#include <limits>
#include <new>

#include <nlohmann/json.hpp>

#include "fs_util.hpp"
#include "vgmplay/playlist.hpp"

namespace vgmplay::playlist {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxPlaylists = 1000;
constexpr std::size_t kMaxEntries = 100000;
constexpr std::uint64_t kMaxFileBytes = 256ull << 20;   // 100000 wpisow * 1000 list to teoretyczny sufit

// Wpis JSON jest odrzucany, gdy wyglada na URL (schemat://) albo sciezke urzadzenia (\\?\, \\.\).
// UNC (\\serwer\udzial) w JSON jest dozwolone - to dane zapisane przez sama aplikacje.
bool isUnsafeStoredPath(const std::string& s)
{
    if (s.empty() || s.find('\0') != std::string::npos) return true;
    auto isSep = [](char c) { return c == '/' || c == '\\'; };
    if (s.size() >= 4 && isSep(s[0]) && isSep(s[1]) && (s[2] == '?' || s[2] == '.') && isSep(s[3])) return true;
    // schemat: litera, potem litery/cyfry/+-. i "://"
    if (std::isalpha(static_cast<unsigned char>(s[0]))) {
        std::size_t i = 1;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '+' || s[i] == '-' || s[i] == '.'))
            ++i;
        if (i + 2 < s.size() && s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') return true;
    }
    return false;
}

bool readUint(const json& j, const char* key, std::uint64_t& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) return false;
    out = it->get<std::uint64_t>();
    return true;
}

} // namespace

std::string saveJson(const PlaylistModel& model)
{
    json root = json::object();
    root["schemaVersion"] = kPlaylistSchemaVersion;
    root["active"] = model.activeIndex();
    json lists = json::array();
    for (std::size_t i = 0; i < model.playlistCount(); ++i) {
        const Playlist& p = model.playlist(i);
        json jl = json::object();
        jl["name"] = p.name;
        jl["cursor"] = p.cursor;
        json entries = json::array();
        for (const Entry& e : p.entries) {
            json je = json::object();
            je["path"] = fsutil::pathToUtf8(e.path);
            if (!e.title.empty()) je["title"] = e.title;
            if (e.durationMs != 0) je["durationMs"] = e.durationMs;
            entries.push_back(std::move(je));
        }
        jl["entries"] = std::move(entries);
        lists.push_back(std::move(jl));
    }
    root["playlists"] = std::move(lists);
    // Zly UTF-8 w tytule/sciezce nie moze rzucic wyjatkiem: zamieniamy na U+FFFD.
    return root.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
}

Result<PlaylistModel> loadJson(std::string_view text)
{
    try {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            text.remove_prefix(3);
        json root = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
        if (root.is_discarded() || !root.is_object()) return makeError(ErrorCode::ParseError, "invalid playlist JSON");

        std::uint64_t ver = 0;
        if (!readUint(root, "schemaVersion", ver)) return makeError(ErrorCode::ParseError, "missing schemaVersion");
        if (ver > kPlaylistSchemaVersion) return makeError(ErrorCode::UnsupportedSchema, "playlist schema is newer than supported");
        if (ver < 1) return makeError(ErrorCode::ParseError, "invalid schemaVersion");

        auto lists = root.find("playlists");
        if (lists == root.end() || !lists->is_array()) return makeError(ErrorCode::ParseError, "missing playlists array");
        if (lists->size() > kMaxPlaylists) return makeError(ErrorCode::LimitExceeded, "too many playlists");

        PlaylistModel model;
        for (const json& jl : *lists) {
            if (!jl.is_object()) return makeError(ErrorCode::ParseError, "playlist is not an object");
            auto nm = jl.find("name");
            if (nm == jl.end() || !nm->is_string()) return makeError(ErrorCode::ParseError, "playlist without name");
            auto idx = model.addPlaylist(nm->get<std::string>());
            if (!idx.ok()) return makeError(ErrorCode::ParseError, "invalid or duplicate playlist name");
            auto en = jl.find("entries");
            if (en == jl.end() || !en->is_array()) return makeError(ErrorCode::ParseError, "playlist without entries array");
            if (en->size() > kMaxEntries) return makeError(ErrorCode::LimitExceeded, "too many playlist entries");

            std::vector<Entry> entries;
            entries.reserve(en->size());
            std::int64_t rawCursor = -1;
            if (auto c = jl.find("cursor"); c != jl.end() && c->is_number_integer()) rawCursor = c->get<std::int64_t>();
            std::int64_t newCursor = -1;
            for (std::size_t k = 0; k < en->size(); ++k) {
                const json& je = (*en)[k];
                if (!je.is_object()) return makeError(ErrorCode::ParseError, "entry is not an object");
                auto pa = je.find("path");
                if (pa == je.end() || !pa->is_string()) return makeError(ErrorCode::ParseError, "entry without path");
                const std::string path = pa->get<std::string>();
                if (isUnsafeStoredPath(path)) continue;   // URL / urzadzenie: wpis pomijany
                Entry e;
                e.path = fsutil::pathFromUtf8(path);
                if (e.path.empty()) continue;
                if (auto t = je.find("title"); t != je.end() && t->is_string()) e.title = t->get<std::string>();
                if (auto d = je.find("durationMs"); d != je.end() && d->is_number_unsigned()) e.durationMs = d->get<std::uint64_t>();
                if (rawCursor == static_cast<std::int64_t>(k)) newCursor = static_cast<std::int64_t>(entries.size());
                entries.push_back(std::move(e));
            }
            if (auto r = model.insert(idx.value(), 0, entries); !r.ok()) return r.error();
            (void)model.setCursor(idx.value(), newCursor);
        }
        std::uint64_t active = 0;
        if (readUint(root, "active", active) && active < model.playlistCount()) (void)model.setActive(static_cast<std::size_t>(active));
        return model;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory parsing playlists");
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::ParseError, std::string("playlist JSON: ") + ex.what());
    }
}

Result<PlaylistModel> loadPlaylistsFile(const std::filesystem::path& file)
{
    auto text = fsutil::readFileLimited(file, kMaxFileBytes);
    if (!text.ok()) {
        // Brak pliku to normalny stan przy pierwszym uruchomieniu: pusty model.
        if (text.error().code == ErrorCode::NotFound) return PlaylistModel{};
        return text.error();
    }
    auto model = loadJson(text.value());
    if (!model.ok()) {
        // Nie gubimy danych: oryginal zostaje nietkniety, obok powstaje kopia .bak; wywolujacy dostaje
        // blad (takze przy UnsupportedSchema - plik nowszej wersji tez wolno nadpisac dopiero po kopii).
        if (!fsutil::preserveCorruptCopy(file, L".bak"))
            return makeError(model.error().code, model.error().message + " (backup copy failed)");
        return model.error();
    }
    return model;
}

Result<void> savePlaylistsFile(const std::filesystem::path& file, const PlaylistModel& model)
{
    try {
        return fsutil::atomicWriteFile(file, saveJson(model));
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory saving playlists");
    }
}

} // namespace vgmplay::playlist
