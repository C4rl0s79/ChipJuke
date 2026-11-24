#include "io_worker.h"

#include "vgmplay_ui/track_meta.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace vgmplay::app {
namespace fs = std::filesystem;

IoWorker::IoWorker(Paths paths, std::function<void()> wake)
    : paths_(std::move(paths)), wake_(std::move(wake)), thread_([this] { loop(); })
{
}

IoWorker::~IoWorker()
{
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        cancel_.requestCancel();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void IoWorker::loadPlaylists()
{
    {
        std::lock_guard lock(mutex_);
        loadPlaylists_ = true;
    }
    cv_.notify_one();
}

void IoWorker::saveSettings(settings::Settings s)
{
    {
        std::lock_guard lock(mutex_);
        pendingSettings_ = std::move(s);
    }
    cv_.notify_one();
}

void IoWorker::savePlaylists(playlist::PlaylistModel m)
{
    {
        std::lock_guard lock(mutex_);
        pendingPlaylists_ = std::move(m);
    }
    cv_.notify_one();
}

void IoWorker::queueMeta(std::vector<fs::path> paths)
{
    {
        std::lock_guard lock(mutex_);
        for (auto& p : paths) {
            if (!metaQueued_.insert(ui::pathKey(p)).second) continue;
            metaQueue_.push_back(std::move(p));
        }
    }
    cv_.notify_one();
}

void IoWorker::queueMetaDownload(fs::path path)
{
    {
        std::lock_guard lock(mutex_);
        downloadQueue_.push_back(std::move(path));
    }
    cv_.notify_one();
}

void IoWorker::importM3u(fs::path file)
{
    {
        std::lock_guard lock(mutex_);
        importQueue_.push_back(std::move(file));
    }
    cv_.notify_one();
}

void IoWorker::exportM3u(fs::path file, std::string text)
{
    {
        std::lock_guard lock(mutex_);
        exportQueue_.emplace_back(std::move(file), std::move(text));
    }
    cv_.notify_one();
}

IoResults IoWorker::take()
{
    std::lock_guard lock(mutex_);
    IoResults out = std::move(results_);
    results_ = IoResults{};
    wakePending_.store(false);
    return out;
}

void IoWorker::post(IoResults& add)
{
    {
        std::lock_guard lock(mutex_);
        if (add.playlists) results_.playlists = std::move(add.playlists);
        for (auto& m : add.meta) results_.meta.push_back(std::move(m));
        for (auto& i : add.imports) results_.imports.push_back(std::move(i));
        for (auto& e : add.exports) results_.exports.push_back(std::move(e));
        for (auto& f : add.saveFailures) results_.saveFailures.push_back(std::move(f));
    }
    if (!wakePending_.exchange(true) && wake_) wake_();
}

namespace {

// Czyta plik z limitem rozmiaru; tooBig = true, gdy wiekszy (wtedy nic nie jest wczytywane).
bool readLimited(const fs::path& file, std::uintmax_t limit, std::string& out, bool& tooBig, Error& err)
{
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) { err = makeError(ErrorCode::NotFound, "cannot stat file"); return false; }
    if (size > limit) { tooBig = true; return false; }
    std::ifstream f(file, std::ios::binary);
    if (!f) { err = makeError(ErrorCode::NotFound, "cannot open file"); return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() > limit) { out.clear(); tooBig = true; return false; }   // plik urosl w trakcie
    return true;
}

} // namespace

void IoWorker::loop()
{
    library::MetaCache cache;
    bool cacheLoaded = false;
    std::size_t cacheDirty = 0;

    auto flushCache = [&] {
        if (paths_.cache.empty() || cacheDirty == 0) return;
        (void)library::saveMetaCacheFile(paths_.cache, cache);
        cacheDirty = 0;
    };

    for (;;) {
        std::optional<settings::Settings> st;
        std::optional<playlist::PlaylistModel> pl;
        bool doLoad = false;
        std::optional<fs::path> imp;
        std::optional<std::pair<fs::path, std::string>> exp;
        std::optional<fs::path> meta;
        bool allowCloud = false;
        bool quitting = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return quit_ || pendingSettings_ || pendingPlaylists_ || loadPlaylists_ || !importQueue_.empty() ||
                       !exportQueue_.empty() || !metaQueue_.empty() || !downloadQueue_.empty();
            });
            quitting = quit_;
            st = std::move(pendingSettings_);
            pendingSettings_.reset();
            pl = std::move(pendingPlaylists_);
            pendingPlaylists_.reset();
            if (!quitting) {
                doLoad = loadPlaylists_;
                loadPlaylists_ = false;
                if (!importQueue_.empty()) {
                    imp = std::move(importQueue_.front());
                    importQueue_.pop_front();
                } else if (!exportQueue_.empty()) {
                    exp = std::move(exportQueue_.front());
                    exportQueue_.pop_front();
                } else if (!doLoad && !st && !pl && !downloadQueue_.empty()) {
                    meta = std::move(downloadQueue_.front());
                    downloadQueue_.pop_front();
                    allowCloud = true;
                } else if (!doLoad && !st && !pl && !metaQueue_.empty()) {
                    meta = std::move(metaQueue_.front());
                    metaQueue_.pop_front();
                    metaQueued_.erase(ui::pathKey(*meta));
                }
            }
        }

        IoResults out;
        // 1) zapisy zawsze pierwsze: dane uzytkownika sa wazniejsze niz metadane
        if (st && !paths_.settings.empty()) {
            auto r = settings::saveSettings(paths_.settings, *st);
            if (!r.ok()) out.saveFailures.push_back({"settings", r.error()});
        }
        if (pl && !paths_.playlists.empty()) {
            auto r = playlist::savePlaylistsFile(paths_.playlists, *pl);
            if (!r.ok()) out.saveFailures.push_back({"playlists", r.error()});
        }
        if (quitting) {
            flushCache();
            return;
        }
        // 2) wczytanie playlist
        if (doLoad) {
            if (paths_.playlists.empty()) out.playlists.emplace(playlist::PlaylistModel{});
            else out.playlists.emplace(playlist::loadPlaylistsFile(paths_.playlists, playlist::PlaylistLoadOptions{paths_.allowNetworkPaths}));
        }
        // 3) M3U
        if (imp) {
            ImportDone d;
            d.file = *imp;
            std::string text;
            Error err;
            if (!readLimited(*imp, kMaxM3uBytes, text, d.tooBig, err)) {
                if (!d.tooBig) d.result.emplace(err);
            } else {
                playlist::M3uImportOptions o;
                o.baseDir = imp->parent_path();
                o.allowedRoot = o.baseDir;
                d.result.emplace(playlist::importM3u(text, o));
            }
            out.imports.push_back(std::move(d));
        }
        if (exp) {
            ExportDone d;
            d.file = exp->first;
            fs::path tmp = exp->first;
            tmp += L".tmp";
            std::error_code ec;
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (f) f.write(exp->second.data(), static_cast<std::streamsize>(exp->second.size()));
                if (!f || !f.flush()) d.error = makeError(ErrorCode::Io, "cannot write file");
            }
            if (!d.error) {
                fs::rename(tmp, exp->first, ec);
                if (ec) d.error = makeError(ErrorCode::Io, "cannot replace file");
            }
            if (d.error) fs::remove(tmp, ec);
            out.exports.push_back(std::move(d));
        }
        // 4) metadane (jeden plik na obieg, zeby zapisy nie czekaly)
        if (meta) {
            if (!cacheLoaded) {
                cacheLoaded = true;
                if (!paths_.cache.empty()) {
                    auto c = library::loadMetaCacheFile(paths_.cache, paths_.allowNetworkPaths);
                    if (c.ok()) cache = std::move(c.value());   // uszkodzony = pusty (nic waznego tam nie ma)
                }
            }
            const std::size_t before = cache.size();
            MetaResult m;
            m.path = *meta;
            auto r = cache.getOrRead(*meta, cancel_, allowCloud);
            if (r.ok()) m.meta = std::move(r.value());
            else m.cloud = library::isCloudPlaceholderError(r.error());
            if (cache.size() != before) ++cacheDirty;
            out.meta.push_back(std::move(m));
            bool more;
            {
                std::lock_guard lock(mutex_);
                more = !metaQueue_.empty() || !downloadQueue_.empty();
            }
            if (!more || cacheDirty >= 200) flushCache();
        }
        if (!out.empty()) post(out);
    }
}

} // namespace vgmplay::app
