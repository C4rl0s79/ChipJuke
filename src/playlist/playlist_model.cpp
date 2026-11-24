// vgmplay - playlist: PlaylistModel (czysta logika, bez I/O).
#include "vgmplay/playlist.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>
#include <random>

#include "fs_util.hpp"

namespace vgmplay::playlist {

namespace {

constexpr std::size_t kMaxPlaylists = 1000;
constexpr std::size_t kMaxEntriesPerList = 100000;
constexpr std::size_t kMaxNameBytes = 256;

Error badArg(const char* msg) { return makeError(ErrorCode::InvalidArgument, msg); }

bool blankName(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\t'; });
}

// Nazwy porownywane bez rozrozniania wielkosci liter (Unicode).
bool sameName(const std::string& a, const std::string& b)
{
    const auto wa = fsutil::utf8ToWide(a, false);
    const auto wb = fsutil::utf8ToWide(b, false);
    return wa && wb && fsutil::equalsIgnoreCase(*wa, *wb);
}

Result<void> checkName(const std::vector<Playlist>& lists, const std::string& name, std::size_t ignore)
{
    if (name.empty() || blankName(name)) return badArg("playlist name is empty");
    if (name.size() > kMaxNameBytes) return badArg("playlist name too long");
    if (!fsutil::utf8ToWide(name, true)) return badArg("playlist name is not valid UTF-8");
    for (std::size_t i = 0; i < lists.size(); ++i)
        if (i != ignore && sameName(lists[i].name, name)) return badArg("duplicate playlist name");
    return Result<void>{};
}

// Nowy indeks kursora po permutacji: perm[nowy] = stary.
std::int64_t followPermutation(const std::vector<std::size_t>& perm, std::int64_t cursor)
{
    if (cursor < 0) return cursor;
    for (std::size_t i = 0; i < perm.size(); ++i)
        if (perm[i] == static_cast<std::size_t>(cursor)) return static_cast<std::int64_t>(i);
    return -1;
}

} // namespace

std::size_t PlaylistModel::playlistCount() const noexcept { return lists_.size(); }

const Playlist& PlaylistModel::playlist(std::size_t index) const { return lists_[index]; }

Result<std::size_t> PlaylistModel::addPlaylist(std::string name)
{
    try {
        if (lists_.size() >= kMaxPlaylists) return makeError(ErrorCode::LimitExceeded, "too many playlists");
        if (auto r = checkName(lists_, name, static_cast<std::size_t>(-1)); !r.ok()) return r.error();
        Playlist p;
        p.name = std::move(name);
        lists_.push_back(std::move(p));
        return lists_.size() - 1;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory");
    }
}

Result<void> PlaylistModel::removePlaylist(std::size_t index)
{
    if (index >= lists_.size()) return badArg("playlist index out of range");
    lists_.erase(lists_.begin() + static_cast<std::ptrdiff_t>(index));
    if (lists_.empty()) active_ = 0;
    else if (index < active_) --active_;
    else if (active_ >= lists_.size()) active_ = lists_.size() - 1;
    return Result<void>{};
}

Result<void> PlaylistModel::renamePlaylist(std::size_t index, std::string name)
{
    if (index >= lists_.size()) return badArg("playlist index out of range");
    if (auto r = checkName(lists_, name, index); !r.ok()) return r;
    lists_[index].name = std::move(name);
    return Result<void>{};
}

Result<void> PlaylistModel::movePlaylist(std::size_t from, std::size_t to)
{
    if (from >= lists_.size() || to >= lists_.size()) return badArg("playlist index out of range");
    if (from == to) return Result<void>{};
    // Aktywna playlista podaza za soba.
    std::size_t newActive = active_;
    if (active_ == from) newActive = to;
    else {
        std::size_t a = active_ > from ? active_ - 1 : active_;   // indeks po usunieciu `from`
        if (a >= to) ++a;                                         // po wstawieniu w `to`
        newActive = a;
    }
    Playlist moved = std::move(lists_[from]);
    lists_.erase(lists_.begin() + static_cast<std::ptrdiff_t>(from));
    lists_.insert(lists_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    active_ = newActive;
    return Result<void>{};
}

std::size_t PlaylistModel::activeIndex() const noexcept { return active_; }

Result<void> PlaylistModel::setActive(std::size_t index)
{
    if (index >= lists_.size()) return badArg("playlist index out of range");
    active_ = index;
    return Result<void>{};
}

Result<void> PlaylistModel::insert(std::size_t list, std::size_t pos, std::span<const Entry> entries)
{
    if (list >= lists_.size()) return badArg("playlist index out of range");
    Playlist& p = lists_[list];
    if (pos > p.entries.size()) return badArg("insert position out of range");
    if (entries.size() > kMaxEntriesPerList || p.entries.size() + entries.size() > kMaxEntriesPerList)
        return makeError(ErrorCode::LimitExceeded, "too many playlist entries");
    if (entries.empty()) return Result<void>{};
    try {
        p.entries.insert(p.entries.begin() + static_cast<std::ptrdiff_t>(pos), entries.begin(), entries.end());
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory");
    }
    if (p.cursor >= static_cast<std::int64_t>(pos)) p.cursor += static_cast<std::int64_t>(entries.size());
    return Result<void>{};
}

Result<void> PlaylistModel::remove(std::size_t list, std::size_t first, std::size_t count)
{
    if (list >= lists_.size()) return badArg("playlist index out of range");
    Playlist& p = lists_[list];
    if (first > p.entries.size() || count > p.entries.size() - first) return badArg("remove range out of range");
    if (count == 0) return Result<void>{};
    p.entries.erase(p.entries.begin() + static_cast<std::ptrdiff_t>(first),
                    p.entries.begin() + static_cast<std::ptrdiff_t>(first + count));
    const auto f = static_cast<std::int64_t>(first);
    const auto c = static_cast<std::int64_t>(count);
    if (p.cursor >= f + c) p.cursor -= c;
    else if (p.cursor >= f) p.cursor = -1;   // usunieto biezacy wpis: brak biezacego
    return Result<void>{};
}

Result<void> PlaylistModel::move(std::size_t list, std::size_t first, std::size_t count, std::size_t to)
{
    if (list >= lists_.size()) return badArg("playlist index out of range");
    Playlist& p = lists_[list];
    const std::size_t n = p.entries.size();
    if (first > n || count > n - first) return badArg("move range out of range");
    if (to > n - count) return badArg("move target out of range");
    if (count == 0 || first == to) return Result<void>{};
    // perm[nowy] = stary indeks.
    std::vector<std::size_t> perm;
    try {
        perm.reserve(n);
        std::vector<std::size_t> rest;
        rest.reserve(n - count);
        for (std::size_t i = 0; i < n; ++i)
            if (i < first || i >= first + count) rest.push_back(i);
        perm.insert(perm.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(to));
        for (std::size_t i = 0; i < count; ++i) perm.push_back(first + i);
        perm.insert(perm.end(), rest.begin() + static_cast<std::ptrdiff_t>(to), rest.end());
        std::vector<Entry> out;
        out.reserve(n);
        for (std::size_t idx : perm) out.push_back(std::move(p.entries[idx]));
        p.entries = std::move(out);
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory");
    }
    p.cursor = followPermutation(perm, p.cursor);
    return Result<void>{};
}

Result<void> PlaylistModel::shuffle(std::size_t list, std::uint64_t seed)
{
    if (list >= lists_.size()) return badArg("playlist index out of range");
    Playlist& p = lists_[list];
    const std::size_t n = p.entries.size();
    if (n < 2) return Result<void>{};
    try {
        std::vector<std::size_t> perm(n);
        std::iota(perm.begin(), perm.end(), std::size_t{0});
        // Wlasny Fisher-Yates: std::shuffle nie gwarantuje tego samego wyniku miedzy implementacjami.
        std::mt19937_64 rng(seed);
        for (std::size_t i = n - 1; i > 0; --i) {
            const std::uint64_t range = static_cast<std::uint64_t>(i) + 1;
            const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max() / range) * range;
            std::uint64_t r = rng();
            while (r >= limit) r = rng();   // odrzucanie => brak obciazenia modulo
            std::swap(perm[i], perm[static_cast<std::size_t>(r % range)]);
        }
        std::vector<Entry> out;
        out.reserve(n);
        for (std::size_t idx : perm) out.push_back(std::move(p.entries[idx]));
        p.entries = std::move(out);
        p.cursor = followPermutation(perm, p.cursor);
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory");
    }
    return Result<void>{};
}

Result<void> PlaylistModel::setCursor(std::size_t list, std::int64_t index)
{
    if (list >= lists_.size()) return badArg("playlist index out of range");
    Playlist& p = lists_[list];
    if (index < -1 || index >= static_cast<std::int64_t>(p.entries.size())) return badArg("cursor out of range");
    p.cursor = index;
    return Result<void>{};
}

// Semantyka (patrz tez ADR 0003):
//  next:     kursor -1 -> 0; Off: za ostatnim NotFound (kursor bez zmian); One: ten sam wpis
//            (auto-advance po utworze); All: za ostatnim zawija na 0.
//  previous: kursor -1 -> NotFound (Off/One) lub ostatni (All); Off: przed pierwszym NotFound;
//            One: ten sam wpis; All: przed pierwszym zawija na ostatni.
Result<std::size_t> PlaylistModel::next(RepeatMode repeat)
{
    if (lists_.empty()) return makeError(ErrorCode::NotFound, "no playlists");
    Playlist& p = lists_[active_];
    const auto n = static_cast<std::int64_t>(p.entries.size());
    if (n == 0) return makeError(ErrorCode::NotFound, "playlist is empty");
    std::int64_t c = p.cursor;
    if (c < 0) c = 0;
    else if (repeat == RepeatMode::One) { /* bez zmian */ }
    else if (c + 1 < n) ++c;
    else if (repeat == RepeatMode::All) c = 0;
    else return makeError(ErrorCode::NotFound, "end of playlist");
    p.cursor = c;
    return static_cast<std::size_t>(c);
}

Result<std::size_t> PlaylistModel::previous(RepeatMode repeat)
{
    if (lists_.empty()) return makeError(ErrorCode::NotFound, "no playlists");
    Playlist& p = lists_[active_];
    const auto n = static_cast<std::int64_t>(p.entries.size());
    if (n == 0) return makeError(ErrorCode::NotFound, "playlist is empty");
    std::int64_t c = p.cursor;
    if (c < 0) {
        if (repeat != RepeatMode::All) return makeError(ErrorCode::NotFound, "no current entry");
        c = n - 1;
    } else if (repeat == RepeatMode::One) { /* bez zmian */ }
    else if (c > 0) --c;
    else if (repeat == RepeatMode::All) c = n - 1;
    else return makeError(ErrorCode::NotFound, "start of playlist");
    p.cursor = c;
    return static_cast<std::size_t>(c);
}

} // namespace vgmplay::playlist
