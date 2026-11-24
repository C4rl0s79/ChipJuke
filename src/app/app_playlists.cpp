// AppController: playlisty, ustawienia, sesja, import/eksport M3U, skan folderow, metadane.
// Wszystko na watku UI; operacje na dysku zleca IoWorker (osobny watek), skan folderu - ScanJob.
#include "app_controller.h"

#include "vgmplay_ui/pattern_view.h"
#include "vgmplay_ui/strings.h"

#include <algorithm>
#include <cwctype>
#include <random>

namespace vgmplay::app {
namespace fs = std::filesystem;
namespace {

constexpr auto kNoticeLife = std::chrono::seconds(10);
constexpr auto kSessionTick = std::chrono::seconds(30);
constexpr std::size_t kMaxDetails = 12;

std::string u8(const fs::path& p)
{
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

bool isVgmFile(const fs::path& p)
{
    std::wstring e = p.extension().wstring();
    for (wchar_t& c : e) c = static_cast<wchar_t>(std::towlower(c));
    if (e == L".vgm" || e == L".vgz") return true;
    // [E4] ".vgm.gz" (przegladarka plikow je pokazuje; loader rozpoznaje gzip po zawartosci)
    std::wstring n = p.filename().wstring();
    for (wchar_t& c : n) c = static_cast<wchar_t>(std::towlower(c));
    return n.size() > 7 && n.compare(n.size() - 7, 7, L".vgm.gz") == 0;
}

template <class... A>
std::string fmt(const char* f, A... a)
{
    char b[1024];
    std::snprintf(b, sizeof b, f, a...);
    return b;
}

std::string truncUtf8(std::string s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    s.resize(maxBytes);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0u) == 0x80u) s.pop_back();
    if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0u) s.pop_back();
    return s;
}

std::string trim(const std::string& s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

// ------------------------------------------------------------------ ustawienia

void AppController::loadSettingsAtStart()
{
    persist_ = !options_.settingsFile.empty();
    if (persist_) {
        std::vector<Warning> warnings;
        auto r = settings::loadSettings(options_.settingsFile, &warnings, options_.allowNetworkPaths);
        if (r.ok()) {
            settings_ = std::move(r.value());
        } else {
            settingsBlocked_ = true;   // oryginal zostaje (kopia .bad); nie nadpisujemy bez wiedzy uzytkownika
            postNotice(ui::tr(ui::Str::NtSettingsBad),
                       {std::string("settings.json: ") + ui::errorCodeName(r.error().code) + " " + r.error().message},
                       true, true);
        }
        rowTicks_ = std::clamp<std::uint32_t>(settings_.ui.rowTicks, 64u, ui::kVgmTickRate);
    } else {
        settings_.ui.rowTicks = rowTicks_;
        settings_.playback.volume = volume_;
        if (options_.noAppDataNotice) postNotice(ui::tr(ui::Str::NtNoAppData), {}, true);
    }
    settings_.ui.rowTicks = rowTicks_;
    repeat_ = static_cast<playlist::RepeatMode>(settings_.playback.repeat);   // [E3] zapamietany tryb powtarzania
    volume_ = std::clamp(settings_.playback.volume, 0.0f, 1.0f);
    view_.patternRowTicks = rowTicks_;
}

void AppController::applyRuntimeSettings(const settings::Settings& old, bool initial)
{
    const settings::Settings& n = settings_;
    if (initial || n.playback.loopCount != old.playback.loopCount) send(engine::cmd::SetLoopCount{n.playback.loopCount});
    if (initial || n.playback.fadeMs != old.playback.fadeMs) send(engine::cmd::SetFade{n.playback.fadeMs});
    if (initial || n.playback.nonLoopEnd != old.playback.nonLoopEnd) send(engine::cmd::SetEndBehavior{n.playback.nonLoopEnd});
    if (initial || n.playback.volume != old.playback.volume) {
        volume_ = std::clamp(n.playback.volume, 0.0f, 1.0f);
        view_.volume = volume_;
        send(engine::cmd::SetVolume{volume_});
    }
    if (!initial && n.ui.rowTicks != old.ui.rowTicks) setPatternRowTicks(n.ui.rowTicks);
    if (!initial && n.audio.device != old.audio.device) switchAudioDevice();   // [E3] ponowne otwarcie wyjscia
}

void AppController::applySettings(const settings::Settings& s)
{
    const settings::Settings old = settings_;
    settings::Settings n = s;
    n.session = old.session;   // sesje zapisuje kontroler
    n.playback.volume = std::clamp(n.playback.volume, 0.0f, 1.0f);
    n.playback.loopCount = std::min<std::uint32_t>(n.playback.loopCount, 99u);
    n.playback.fadeMs = std::min<std::uint32_t>(n.playback.fadeMs, 30000u);
    n.ui.rowTicks = std::clamp<std::uint32_t>(n.ui.rowTicks, 64u, ui::kVgmTickRate);
    settings_ = std::move(n);
    if (io_ && old.library.allowNetworkPaths != settings_.library.allowNetworkPaths)   // [E3-sec] od razu, bez restartu
        io_->setAllowNetworkPaths(options_.allowNetworkPaths || settings_.library.allowNetworkPaths);
    applyRuntimeSettings(old, false);
    markSettingsDirty();
}

settings::Settings AppController::settingsForSave() const
{
    settings::Settings s = settings_;
    if (!restoreActive_) {
        if (track_) {
            s.session.trackPath = u8(track_->path);
            const auto st = view_.transport.state;
            s.session.positionMs = st == engine::TransportState::Ended || view_.sampleRate == 0
                                       ? 0
                                       : view_.transport.audibleFrame * 1000 / view_.sampleRate;
        } else if (inflight_.empty()) {
            s.session = {};
        }
    }
    return s;
}

void AppController::markSettingsDirty()
{
    if (!persist_) return;
    settingsDirty_ = true;
    settingsDirtyAt_ = std::chrono::steady_clock::now();
}

void AppController::markPlaylistsDirty()
{
    if (!persist_) return;
    playlistsDirty_ = true;
    playlistsDirtyAt_ = std::chrono::steady_clock::now();
}

void AppController::flushPersistence(bool force)
{
    if (!io_ || !persist_) return;
    const auto now = std::chrono::steady_clock::now();
    const auto delay = std::chrono::milliseconds(options_.saveDelayMs);
    if (!force && view_.transport.state == engine::TransportState::Playing && lastSessionTick_.time_since_epoch().count() != 0 &&
        now - lastSessionTick_ >= kSessionTick) {
        lastSessionTick_ = now;
        markSettingsDirty();   // pozycja w utworze (sesja) - rzadko, nie co klatke
    }
    if (force || (settingsDirty_ && now - settingsDirtyAt_ >= delay)) {
        if (!settingsBlocked_) io_->saveSettings(settingsForSave());
        settingsDirty_ = false;
    }
    if (force || (playlistsDirty_ && now - playlistsDirtyAt_ >= delay)) {
        if (playlistsReady_ && !playlistsBlocked_) io_->savePlaylists(model_);
        playlistsDirty_ = false;
    }
}

// ------------------------------------------------------------------ komunikaty

void AppController::postNotice(std::string text, std::vector<std::string> details, bool error, bool needsAck)
{
    if (notice_ && notice_->needsAck) {   // nie gubimy pytania o potwierdzenie; kolejne trafia do szczegolow
        if (needsAck) {
            notice_->details.push_back(std::move(text));
            for (auto& d : details) notice_->details.push_back(std::move(d));
        }
        return;
    }
    if (details.size() > kMaxDetails) details.resize(kMaxDetails);
    ui::Notice n;
    n.text = std::move(text);
    n.details = std::move(details);
    n.error = error;
    n.needsAck = needsAck;
    n.serial = ++noticeSerial_;
    notice_ = std::move(n);
    noticeUntil_ = std::chrono::steady_clock::now() + kNoticeLife;
}

void AppController::dismissNotice()
{
    if (!notice_) return;
    if (notice_->needsAck) {   // uzytkownik wie o kopii .bak/.bad: zapis nowych danych dozwolony
        settingsBlocked_ = false;
        playlistsBlocked_ = false;
        markSettingsDirty();
        markPlaylistsDirty();
    }
    notice_.reset();
}

// ------------------------------------------------------------------ skrzynka (dowolny watek)

void AppController::postInbox(std::function<void()> fn)
{
    {
        std::lock_guard lock(inboxMutex_);
        inbox_.push_back(std::move(fn));
    }
    if (wake_) wake_();
}

void AppController::submitPaths(std::vector<fs::path> paths)
{
    postInbox([this, p = std::move(paths)] { addPathsNow(p); });
}

void AppController::submitFolder(const fs::path& folder)
{
    postInbox([this, folder] { addFolderNow(folder); });
}

void AppController::submitImportM3u(const fs::path& file)
{
    postInbox([this, file] { if (io_) io_->importM3u(file); });
}

void AppController::submitExportM3u(const fs::path& file, bool overwrite)
{
    postInbox([this, file, overwrite] {
        if (!io_ || model_.playlistCount() == 0) return;
        playlist::M3uExportOptions o;
        o.baseDir = file.parent_path();
        io_->exportM3u(file, playlist::exportM3u(model_.playlist(model_.activeIndex()), o), overwrite);
    });
}

void AppController::processInbox()
{
    if (!playlistsReady_) return;   // zlecenia czekaja na wczytanie playlist
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(inboxMutex_);
        work.swap(inbox_);
    }
    for (auto& fn : work) fn();
}

void AppController::openStartupFile(const fs::path& file)
{
    std::error_code ec;
    fs::path abs = fs::absolute(file, ec);
    startupFile_ = ec ? file : abs;
    if (playlistsReady_) restoreSession();
}

// ------------------------------------------------------------------ playlisty

void AppController::installDefaultPlaylists()
{
    model_ = playlist::PlaylistModel{};
    (void)model_.addPlaylist(settings_.ui.language == i18n::Language::Pl ? "Playlista" : "Playlist");
    playlistsReady_ = true;
    restoreSession();
}

std::optional<std::size_t> AppController::findList(const std::string& name) const
{
    for (std::size_t i = 0; i < model_.playlistCount(); ++i)
        if (model_.playlist(i).name == name) return i;
    return std::nullopt;
}

bool AppController::nothingLoaded() const
{
    if (track_ || !inflight_.empty() || restoreActive_) return false;
    std::lock_guard lock(const_cast<std::mutex&>(mutex_));
    return latestId_ == handledId_;
}

void AppController::handlePlaylistsLoaded(Result<playlist::PlaylistModel>& r)
{
    if (r.ok()) {
        model_ = std::move(r.value());
    } else {
        model_ = playlist::PlaylistModel{};
        playlistsBlocked_ = true;   // plik zostaje (kopia .bak); zapis dopiero po potwierdzeniu
        postNotice(ui::tr(ui::Str::NtPlaylistsBad),
                   {std::string("playlists.json: ") + ui::errorCodeName(r.error().code) + " " + r.error().message},
                   true, true);
    }
    if (model_.playlistCount() == 0)
        (void)model_.addPlaylist(settings_.ui.language == i18n::Language::Pl ? "Playlista" : "Playlist");
    playlistsReady_ = true;
    // Metadane w tle: najpierw aktywna lista, potem reszta.
    if (model_.playlistCount() > 0) queueMetaFor(model_.playlist(model_.activeIndex()).entries);
    for (std::size_t i = 0; i < model_.playlistCount(); ++i)
        if (i != model_.activeIndex()) queueMetaFor(model_.playlist(i).entries);
    restoreSession();
}

void AppController::addAndPlay(const fs::path& path)
{
    const std::size_t list = model_.activeIndex();
    const auto& entries = model_.playlist(list).entries;
    std::size_t idx = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (ui::pathKey(entries[i].path) == ui::pathKey(path)) { idx = i; break; }
    if (idx == entries.size()) {
        playlist::Entry e;
        e.path = path;
        std::size_t first = 0;
        if (addEntries(list, {e}, false, &first) == 0) { loadFileEx(path, true); return; }
        idx = first;
    }
    (void)model_.setCursor(list, static_cast<std::int64_t>(idx));
    markPlaylistsDirty();
    loadFileEx(path, true);
}

void AppController::restoreSession()
{
    if (!playlistsReady_ || model_.playlistCount() == 0) return;
    if (startupFile_) {   // plik z wiersza polecen ma pierwszenstwo; sesja nie jest wznawiana
        const fs::path f = *startupFile_;
        startupFile_.reset();
        addAndPlay(f);
        return;
    }
    const playlist::Playlist& l = model_.playlist(model_.activeIndex());
    if (l.cursor < 0 || static_cast<std::size_t>(l.cursor) >= l.entries.size()) return;
    const fs::path path = l.entries[static_cast<std::size_t>(l.cursor)].path;
    {   // [E3-sec] sciezka UNC (takze z trackPath sesji) nie jest wznawiana automatycznie bez --allow-network-paths:
        // otwarcie moze wyslac dane logowania Windows do serwera wskazanego w pliku.
        const auto isUnc = [](const std::wstring& w) {
            const auto sep = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
            return w.size() >= 2 && sep(w[0]) && sep(w[1]);
        };
        const std::wstring sess = fs::path(std::u8string(settings_.session.trackPath.begin(), settings_.session.trackPath.end())).native();
        if (!options_.allowNetworkPaths && (isUnc(path.native()) || isUnc(sess))) return;
    }
    Restore r;
    r.path = path;
    const bool same = !settings_.session.trackPath.empty() &&
                      ui::pathKey(fs::path(std::u8string(settings_.session.trackPath.begin(),
                                                          settings_.session.trackPath.end()))) == ui::pathKey(path);
    r.positionMs = same ? settings_.session.positionMs : 0;
    restore_ = r;
    restoreActive_ = true;
    loadFileEx(path, false);   // bez automatycznego odtwarzania: pauza na zapisanej pozycji
}

void AppController::loadCursor(bool autoplay)
{
    if (model_.playlistCount() == 0) return;
    const playlist::Playlist& l = model_.playlist(model_.activeIndex());
    if (l.cursor < 0 || static_cast<std::size_t>(l.cursor) >= l.entries.size()) return;
    restore_.reset();
    restoreActive_ = false;
    loadFileEx(l.entries[static_cast<std::size_t>(l.cursor)].path, autoplay);
}

void AppController::onTrackEnded()
{
    if (!playlistsReady_ || model_.playlistCount() == 0) return;
    const playlist::Playlist& l = model_.playlist(model_.activeIndex());
    // Auto-advance tylko gdy gra biezacy wpis kursora (nie plik spoza playlisty / usuniety wpis).
    if (l.cursor < 0 || static_cast<std::size_t>(l.cursor) >= l.entries.size() || !track_ ||
        ui::pathKey(l.entries[static_cast<std::size_t>(l.cursor)].path) != ui::pathKey(track_->path))
        return;
    auto r = model_.next(repeat_);   // One: ten sam indeks; Off za koncem: NotFound (zostaje Ended)
    if (!r.ok()) return;
    markPlaylistsDirty();
    loadCursor(true);
}

std::size_t AppController::addEntries(std::size_t list, std::vector<playlist::Entry> entries, bool playIfIdle,
                                      std::size_t* firstIndex)
{
    if (entries.empty() || list >= model_.playlistCount()) return 0;
    const std::size_t first = model_.playlist(list).entries.size();
    auto r = model_.insert(list, first, entries);
    if (!r.ok()) {
        postNotice(ui::trError(r.error().code), {r.error().message}, true);
        return 0;
    }
    if (firstIndex) *firstIndex = first;
    markPlaylistsDirty();
    queueMetaFor(entries);
    if (playIfIdle && list == model_.activeIndex() && nothingLoaded()) {
        (void)model_.setCursor(list, static_cast<std::int64_t>(first));
        loadCursor(options_.autoplay);
    }
    return entries.size();
}

void AppController::queueMetaFor(const std::vector<playlist::Entry>& entries)
{
    if (!io_) return;
    std::vector<fs::path> paths;
    for (const auto& e : entries)
        if (!metaStore_.find(e.path)) paths.push_back(e.path);
    if (!paths.empty()) io_->queueMeta(std::move(paths));
}

void AppController::addPathsNow(const std::vector<fs::path>& paths)
{
    if (model_.playlistCount() == 0) return;
    // Rozpoznanie plik/katalog robi IoWorker (dysk poza watkiem UI); wynik wraca przez handleIo() -> addClassifiedNow().
    if (io_) {
        io_->classifyPaths(paths, model_.playlist(model_.activeIndex()).name);   // cel zapamietany teraz, nie po rozpoznaniu
        return;
    }
    std::vector<ClassifiedPath> flat;   // bez workera: tylko po rozszerzeniu (bez dotykania dysku)
    flat.reserve(paths.size());
    for (const fs::path& p : paths) flat.push_back(ClassifiedPath{p, false});
    addClassifiedNow(flat);
}

void AppController::addClassifiedNow(const std::vector<ClassifiedPath>& paths, const std::string& listName)
{
    if (model_.playlistCount() == 0) return;
    // Playlista z chwili zlecenia (po nazwie); gdy w miedzyczasie zniknela - aktywna, z komunikatem.
    std::size_t list = model_.activeIndex();
    std::string target = model_.playlist(list).name;
    bool gone = false;
    if (!listName.empty()) {
        if (const auto found = findList(listName)) {
            list = *found;
            target = listName;
        } else {
            gone = true;
        }
    }
    std::vector<playlist::Entry> entries;
    unsigned skipped = 0;
    bool folders = false;
    for (const ClassifiedPath& cp : paths) {
        const fs::path& p = cp.path;
        if (cp.isDirectory) {
            addFolderNow(p, target);
            folders = true;
        } else if (isVgmFile(p)) {
            playlist::Entry e;
            e.path = p;
            entries.push_back(std::move(e));
        } else {
            ++skipped;
        }
    }
    const std::size_t n = addEntries(list, std::move(entries), true);
    if (n > 0) {
        std::string t = fmt(ui::tr(ui::Str::NtAdded), static_cast<unsigned>(n),
                            model_.playlist(list).name.c_str());
        if (skipped) t += "; " + fmt(ui::tr(ui::Str::NtSkipped), skipped);
        postNotice(t, gone ? std::vector<std::string>{ui::tr(ui::Str::NtListGone)} : std::vector<std::string>{});
    } else if (skipped && !folders) {
        postNotice(ui::tr(ui::Str::NtNonVgm), {}, true);
    } else if (gone && folders) {
        postNotice(ui::tr(ui::Str::NtListGone));
    }
}

void AppController::addFolderNow(const fs::path& folder, const std::string& listName)
{
    auto r = library::ScanJob::start(folder);
    if (!r.ok()) {
        postNotice(ui::tr(ui::Str::NtScanFailed), {r.error().message}, true);
        return;
    }
    ScanEntry se;
    se.job = std::move(r.value());
    se.root = folder;
    if (!listName.empty()) se.listName = listName;
    else if (model_.playlistCount() > 0) se.listName = model_.playlist(model_.activeIndex()).name;
    scans_.push_back(std::move(se));
}

void AppController::processScans()
{
    for (std::size_t i = 0; i < scans_.size();) {
        if (!scans_[i].job->done()) { ++i; continue; }
        auto res = scans_[i].job->takeResult();
        const std::string listName = scans_[i].listName;
        scans_.erase(scans_.begin() + static_cast<std::ptrdiff_t>(i));
        if (!res) continue;
        if (res->cancelled) { postNotice(ui::tr(ui::Str::NtScanCancelled)); continue; }
        std::vector<playlist::Entry> entries;
        entries.reserve(res->files.size());
        for (const auto& f : res->files) {
            playlist::Entry e;
            e.path = f.path;
            entries.push_back(std::move(e));
        }
        const std::size_t list = findList(listName).value_or(model_.activeIndex());
        const std::size_t n = addEntries(list, std::move(entries), true);
        std::vector<std::string> details;
        if (res->truncatedByMaxFiles || res->truncatedByMaxDepth) details.push_back(ui::tr(ui::Str::NtScanTruncated));
        for (const Warning& w : res->warnings) {
            if (details.size() >= kMaxDetails) break;
            details.push_back(std::string(ui::trWarning(w.code)) + ": " + w.message);
        }
        postNotice(fmt(ui::tr(ui::Str::NtScanDone), static_cast<unsigned>(n)), std::move(details));
    }
}

void AppController::importM3uDone(ImportDone& d)
{
    if (d.tooBig) { postNotice(ui::tr(ui::Str::NtTooBig), {}, true); return; }
    if (!d.result || !d.result->ok()) {
        postNotice(ui::tr(ui::Str::NtImportFail), {d.result ? d.result->error().message : std::string()}, true);
        return;
    }
    playlist::M3uImportResult& imp = d.result->value();
    std::string base = d.file.stem().empty() ? std::string("M3U") : truncUtf8(u8(d.file.stem()), 100);
    std::optional<std::size_t> idx;
    for (int k = 1; k < 100 && !idx; ++k) {
        auto r = model_.addPlaylist(k == 1 ? base : base + " (" + std::to_string(k) + ")");
        if (r.ok()) idx = r.value();
    }
    if (!idx) { postNotice(ui::tr(ui::Str::NtImportFail), {}, true); return; }
    (void)model_.setActive(*idx);
    const std::size_t added = addEntries(*idx, std::move(imp.entries), false);
    std::vector<std::string> details;
    for (const Warning& w : imp.warnings) {
        if (details.size() >= kMaxDetails) break;
        details.push_back(std::string(ui::trWarning(w.code)) + ": " + w.message);
    }
    markPlaylistsDirty();
    postNotice(fmt(ui::tr(ui::Str::NtImport), base.c_str(), static_cast<unsigned>(added), imp.rejected),
               std::move(details), imp.rejected > 0);
}

void AppController::handleIo()
{
    if (!io_) return;
    IoResults r = io_->take();
    if (r.playlists) handlePlaylistsLoaded(*r.playlists);
    for (MetaResult& m : r.meta) {
        ui::TrackDisplay d;
        if (const ui::TrackDisplay* old = metaStore_.find(m.path)) d = *old;
        if (m.cloud) {   // placeholder chmury: bez pobierania; uzytkownik moze wybrac "pobierz mimo to"
            d.cloud = true;
            d.failed = false;
        } else if (m.meta) {
            d.cloud = false;
            if (!m.meta->trackEn.empty() || !m.meta->trackJp.empty()) {
                d.trackEn = m.meta->trackEn;
                d.trackJp = m.meta->trackJp;
            }
            d.durationMs = m.meta->durationMs;
            d.failed = false;
        } else {
            d.failed = true;   // nie ponawiamy; lista pokaze nazwe pliku
        }
        metaStore_.put(m.path, std::move(d));
    }
    if (!r.meta.empty()) {   // [E3-sec] jedno przejscie po playlistach dla calego batcha (nie O(wyniki x wpisy))
        std::vector<playlist::EntryMetaUpdate> ups;
        ups.reserve(r.meta.size());
        for (const MetaResult& m : r.meta)
            if (auto u = entryMetaUpdate(m.path)) ups.push_back(std::move(*u));
        if (model_.updateEntryMeta(ups) > 0) markPlaylistsDirty();   // [E3] tytul i dlugosc ze skanu do wpisow
    }
    for (ImportDone& d : r.imports) importM3uDone(d);
    for (std::size_t i = 0; i < r.classified.size(); ++i)
        addClassifiedNow(r.classified[i], i < r.classifiedTags.size() ? r.classifiedTags[i] : std::string());
    for (ExportDone& d : r.exports) {
        if (d.error) postNotice(ui::tr(ui::Str::NtExportFail), {d.error->message}, true);
        else postNotice(fmt(ui::tr(ui::Str::NtExportOk), u8(d.written.filename()).c_str()));   // faktyczna nazwa
    }
    for (SaveFailure& f : r.saveFailures)
        postNotice(std::string(ui::tr(ui::Str::NtSaveFail)) + " (" + f.what + ")", {f.error.message}, true);
}

// ------------------------------------------------------------------ PlayerActions: playlisty

void AppController::playEntry(std::size_t index)
{
    if (!playlistsReady_ || model_.playlistCount() == 0) return;
    const std::size_t list = model_.activeIndex();
    if (index >= model_.playlist(list).entries.size()) return;
    (void)model_.setCursor(list, static_cast<std::int64_t>(index));
    markPlaylistsDirty();
    loadCursor(true);
}

void AppController::playNext()
{
    if (!playlistsReady_) return;
    // Reczne "nastepny": One traktujemy jak Off (inaczej przycisk stalby w miejscu).
    auto r = model_.next(repeat_ == playlist::RepeatMode::All ? playlist::RepeatMode::All : playlist::RepeatMode::Off);
    if (!r.ok()) return;
    markPlaylistsDirty();
    loadCursor(true);
}

void AppController::playPrevious()
{
    if (!playlistsReady_) return;
    const bool deepIn = view_.hasTrack() && view_.sampleRate > 0 &&
                        static_cast<double>(view_.transport.audibleFrame) > kPrevRestartSeconds * view_.sampleRate;
    if (deepIn) {   // dalej niz ~3 s: poczatek biezacego utworu
        seekToFrame(0);
        if (view_.transport.state == engine::TransportState::Ended) play();
        return;
    }
    auto r = model_.previous(repeat_ == playlist::RepeatMode::All ? playlist::RepeatMode::All : playlist::RepeatMode::Off);
    if (r.ok()) {
        markPlaylistsDirty();
        loadCursor(true);
    } else if (view_.hasTrack()) {   // brak poprzedniego: poczatek utworu
        seekToFrame(0);
        if (view_.transport.state == engine::TransportState::Ended) play();
    }
}

void AppController::downloadEntry(std::size_t index)
{
    if (!playlistsReady_ || model_.playlistCount() == 0 || !io_) return;
    const auto& l = model_.playlist(model_.activeIndex());
    if (index >= l.entries.size()) return;
    const ui::TrackDisplay* d = metaStore_.find(l.entries[index].path);
    if (d && d->cloud) io_->queueMetaDownload(l.entries[index].path);
}

void AppController::removeEntry(std::size_t index)
{
    if (!playlistsReady_ || model_.playlistCount() == 0) return;
    if (model_.remove(model_.activeIndex(), index, 1).ok()) markPlaylistsDirty();
}

void AppController::moveEntry(std::size_t from, std::size_t to)
{
    if (!playlistsReady_ || model_.playlistCount() == 0) return;
    if (model_.move(model_.activeIndex(), from, 1, to).ok()) markPlaylistsDirty();
}

void AppController::shufflePlaylist()
{
    if (!playlistsReady_ || model_.playlistCount() == 0) return;
    std::random_device rd;
    const std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    if (model_.shuffle(model_.activeIndex(), seed).ok()) markPlaylistsDirty();
}

void AppController::setRepeat(playlist::RepeatMode mode)
{
    repeat_ = mode;
    const auto r = static_cast<settings::Repeat>(mode);   // wartosci jak playlist::RepeatMode
    if (settings_.playback.repeat != r) {
        settings_.playback.repeat = r;   // [E3] zapamietany tryb powtarzania
        markSettingsDirty();
    }
}

std::optional<playlist::EntryMetaUpdate> AppController::entryMetaUpdate(const std::filesystem::path& path) const
{
    const ui::TrackDisplay* d = metaStore_.find(path);
    if (!d || d->failed) return std::nullopt;
    const std::string& title = !d->trackEn.empty() ? d->trackEn : d->trackJp;
    if (title.empty() && d->durationMs == 0) return std::nullopt;
    return playlist::EntryMetaUpdate{path, title, d->durationMs};
}

void AppController::syncEntryMeta(const std::filesystem::path& path)
{
    if (auto u = entryMetaUpdate(path))
        if (model_.updateEntryMeta(std::span<const playlist::EntryMetaUpdate>(&*u, 1)) > 0) markPlaylistsDirty();
}

void AppController::refreshAudioDevices()
{
    audioDevices_.clear();
    const auto r = audio_out::listDevices(audioCfg_.backend);
    if (r.ok())
        for (const audio_out::DeviceEntry& e : r.value()) audioDevices_.push_back({e.id, e.name, e.isDefault});
    audioDevicesLoaded_ = true;
    if (audioNoOutput_) switchAudioDevice();   // [E3-sec] reczne ponowienie (Confirm w wierszu urzadzenia / otwarcie ustawien)
}

void AppController::switchAudioDevice()
{
    audio_.stop();
    pcm_.discardPending();   // [E3-sec] konsument zatrzymany: stare PCM nie trafi do nowego urzadzenia
    audioCfg_.deviceId = settings_.audio.device;
    audioCfg_.sampleRate = view_.sampleRate;   // silnik renderuje w stalej czestotliwosci; konwersje robi WASAPI
    auto r = audio_.open(audioCfg_);
    bool fellBack = r.ok() && r.value().fellBackToDefault;
    std::string firstError = r.ok() ? std::string() : r.error().message;
    if (!r.ok() && !audioCfg_.deviceId.empty()) {   // wybrane urzadzenie niedostepne: wracamy do domyslnego
        audio_out::Config def = audioCfg_;
        def.deviceId.clear();
        r = audio_.open(def);
        fellBack = r.ok();   // [E3-sec] tylko po UDANYM drugim open
    }
    if (!r.ok() && audioCfg_.backend != audio_out::Config::Backend::Null) {   // oba nieudane: wyjscie Null (bez dzwieku)
        audio_out::Config nul = audioCfg_;
        nul.backend = audio_out::Config::Backend::Null;
        nul.deviceId.clear();
        auto rn = audio_.open(nul);
        if (rn.ok()) {
            const std::string msg = r.error().message.empty() ? firstError : r.error().message;
            audioFellBack_ = false;
            audioNoOutput_ = true;   // UI pokazuje "brak wyjscia"; ponawiane w pump() i przy Confirm w wierszu urzadzenia
            audioRetryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            audioError_ = msg;
            const auto s = audio_.start();
            if (!s.ok()) audioError_ = s.error().message;
            return;
        }
    }
    audioFellBack_ = fellBack;
    if (!r.ok()) {
        audioNoOutput_ = true;   // takze Null nie dziala: ponawiaj
        audioRetryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        audioError_ = r.error().message;
        return;
    }
    const bool wasNoOutput = audioNoOutput_;
    audioNoOutput_ = false;
    audioError_.clear();
    if (audioFellBack_ && !wasNoOutput) postNotice(ui::tr(ui::Str::SetAudioFallback), {settings_.audio.device});
    const auto s = audio_.start();
    if (!s.ok()) audioError_ = s.error().message;
}

void AppController::selectPlaylist(std::size_t index)
{
    if (!playlistsReady_) return;
    if (model_.setActive(index).ok()) markPlaylistsDirty();
}

void AppController::createPlaylist(const std::string& name)
{
    if (!playlistsReady_) return;
    const std::string n = truncUtf8(trim(name), 200);
    auto r = model_.addPlaylist(n);
    if (!r.ok()) { postNotice(ui::trError(r.error().code), {r.error().message}, true); return; }
    (void)model_.setActive(r.value());
    markPlaylistsDirty();
}

void AppController::renamePlaylist(std::size_t index, const std::string& name)
{
    if (!playlistsReady_ || index >= model_.playlistCount()) return;
    auto r = model_.renamePlaylist(index, truncUtf8(trim(name), 200));
    if (!r.ok()) { postNotice(ui::trError(r.error().code), {r.error().message}, true); return; }
    markPlaylistsDirty();
}

void AppController::deletePlaylist(std::size_t index)
{
    if (!playlistsReady_ || model_.playlistCount() <= 1 || index >= model_.playlistCount()) return;
    if (model_.removePlaylist(index).ok()) markPlaylistsDirty();
}

void AppController::requestAddFolder()
{
    if (dialogHandler_) dialogHandler_(DialogRequest::AddFolder);
}

void AppController::requestImportM3u()
{
    if (dialogHandler_) dialogHandler_(DialogRequest::ImportM3u);
}

void AppController::requestExportM3u()
{
    if (dialogHandler_) dialogHandler_(DialogRequest::ExportM3u);
}

void AppController::cancelScan()
{
    for (ScanEntry& s : scans_) s.job->cancel();
}

} // namespace vgmplay::app
