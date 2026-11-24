// vgmplay - library: ScanJob (skan katalogow we wlasnym watku, bez parsowania plikow).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <set>
#include <thread>

#include "vgmplay/library.hpp"

#include "fs_util.hpp"

namespace vgmplay::library {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxWarnings = 100;
constexpr std::size_t kMaxPendingDirs = 100000;   // limit stosu katalogow do odwiedzenia

std::wstring lowerKey(std::wstring s)
{
    if (!s.empty()) ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// Sciezka rozszerzona (\\?\...), by skan nie ograniczal sie do MAX_PATH.
std::wstring toExtended(std::wstring w)
{
    std::replace(w.begin(), w.end(), L'/', L'\\');
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}

bool isLinkReparse(const WIN32_FIND_DATAW& fd)
{
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    // Tylko dowiazania symboliczne i junctions; inne znaczniki (chmura OneDrive, dedup) to zwykle pliki.
    return fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
}

} // namespace

struct ScanJob::Impl {
    fs::path root;
    ScanOptions options;
    CancelToken cancel;
    std::atomic<std::uint64_t> filesFound{0};
    std::atomic<std::uint64_t> dirsVisited{0};
    std::atomic<bool> done{false};
    std::mutex mu;
    std::optional<ScanResult> result;
    std::thread worker;

    void warn(ScanResult& r, WarningCode code, std::string msg) const
    {
        if (r.warnings.size() < kMaxWarnings) r.warnings.push_back(Warning{code, kNoOffset, std::move(msg)});
    }

    void run() noexcept
    {
        ScanResult r;
        try {
            scan(r);
        } catch (const std::bad_alloc&) {
            warn(r, WarningCode::Other, "out of memory during scan");
        } catch (...) {
            warn(r, WarningCode::Other, "unexpected error during scan");
        }
        try {
            std::stable_sort(r.files.begin(), r.files.end(), [](const FoundFile& a, const FoundFile& b) {
                const std::wstring ka = lowerKey(a.path.native()), kb = lowerKey(b.path.native());
                if (ka != kb) return ka < kb;
                return a.path.native() < b.path.native();
            });
        } catch (...) {
        }
        if (cancel.isCancelled()) r.cancelled = true;
        {
            std::lock_guard<std::mutex> lk(mu);
            result = std::move(r);
        }
        done.store(true, std::memory_order_release);
    }

    void scan(ScanResult& r)
    {
        std::vector<std::wstring> exts;
        for (const std::string& e : options.extensions) {
            auto w = fsutil::utf8ToWide(e, false);
            if (!w || w->empty()) continue;
            if ((*w)[0] != L'.') w->insert(w->begin(), L'.');
            exts.push_back(lowerKey(*w));
        }
        // Dopasowanie po koncowce nazwy (obsluguje tez rozszerzenia wieloczlonowe, np. ".vgm.gz"); sama koncowka
        // bez nazwy (np. plik ".vgm") nie pasuje.
        auto matches = [&](const std::wstring& name) {
            const std::wstring low = lowerKey(name);
            for (const std::wstring& ext : exts)
                if (ext.size() > 1 && low.size() > ext.size() && low.compare(low.size() - ext.size(), ext.size(), ext) == 0) return true;
            return false;
        };

        struct Dir { std::wstring path; std::uint32_t depth; };
        std::vector<Dir> stack;
        stack.push_back({root.native(), 0});
        bool dirLimitWarned = false;
        std::set<std::wstring> visitedLinks;   // ochrona przed cyklami przy followReparsePoints=true
        const std::size_t rootLen = root.native().size();
        auto rel = [&](const std::wstring& p) {
            std::wstring s = p.size() > rootLen ? p.substr(rootLen) : std::wstring();
            while (!s.empty() && s.front() == L'\\') s.erase(s.begin());
            return fsutil::wideToUtf8(s.empty() ? L"." : s);
        };

        while (!stack.empty()) {
            if (cancel.isCancelled()) return;
            Dir dir = std::move(stack.back());
            stack.pop_back();

            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileExW((toExtended(dir.path) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                          nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) {
                const DWORD e = ::GetLastError();
                warn(r, e == ERROR_ACCESS_DENIED ? WarningCode::AccessDenied : WarningCode::Other,
                     "cannot list directory: " + rel(dir.path) + " (win32 error " + std::to_string(e) + ")");
                continue;
            }
            ++r.directoriesVisited;
            dirsVisited.store(r.directoriesVisited, std::memory_order_relaxed);

            std::vector<Dir> subdirs;
            do {
                if (cancel.isCancelled()) { ::FindClose(h); return; }
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                const std::wstring full = dir.path + (dir.path.back() == L'\\' ? L"" : L"\\") + name;
                const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                // Nazwy, ktorych Win32 nie traktuje doslownie (CON, NUL, koncowa kropka/spacja): pomijamy.
                if (fsutil::isUnsafeComponentName(name)) {
                    warn(r, WarningCode::EntrySkipped, "entry with reserved or unsafe name skipped: " + rel(full));
                    continue;
                }

                if (!options.followReparsePoints && isLinkReparse(fd)) {
                    ++r.reparsePointsSkipped;
                    warn(r, WarningCode::ReparsePointSkipped, "reparse point skipped: " + rel(full));
                    continue;
                }
                if (isDir) {
                    if (!options.recursive) continue;
                    if (dir.depth >= options.maxDepth) {
                        if (!r.truncatedByMaxDepth) warn(r, WarningCode::LimitReached, "maximum scan depth reached");
                        r.truncatedByMaxDepth = true;
                        continue;
                    }
                    if (isLinkReparse(fd)) {   // tylko przy follow=true
                        fs::path canon;
                        if (!fsutil::weaklyResolve(fs::path(full).lexically_normal(), canon) ||
                            !visitedLinks.insert(lowerKey(canon.native())).second) {
                            warn(r, WarningCode::ReparsePointSkipped, "link cycle or unresolved link skipped: " + rel(full));
                            continue;
                        }
                    }
                    if (stack.size() + subdirs.size() >= kMaxPendingDirs) {
                        if (!dirLimitWarned) warn(r, WarningCode::LimitReached, "too many pending directories; some skipped");
                        dirLimitWarned = true;
                        continue;
                    }
                    subdirs.push_back({full, dir.depth + 1});
                    continue;
                }
                if (!matches(name)) continue;
                if (r.files.size() >= options.maxFiles) {
                    r.truncatedByMaxFiles = true;
                    warn(r, WarningCode::LimitReached, "maximum file count reached");
                    ::FindClose(h);
                    return;
                }
                FoundFile ff;
                ff.path = fs::path(full);
                ff.sizeBytes = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                ff.attributes = fd.dwFileAttributes;
                if (isCloudPlaceholder(ff.attributes))
                    warn(r, WarningCode::Other, "cloud placeholder (content not local): " + rel(full));
                r.files.push_back(std::move(ff));
                filesFound.store(r.files.size(), std::memory_order_relaxed);
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);

            // Odwrocona kolejnosc na stosie => odwiedzanie mniej wiecej alfabetyczne; wynik i tak sortowany.
            for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(std::move(*it));
        }
    }
};

ScanJob::ScanJob() : impl_(std::make_unique<Impl>()) {}

ScanJob::~ScanJob()
{
    if (impl_) {
        impl_->cancel.requestCancel();
        if (impl_->worker.joinable()) impl_->worker.join();
    }
}

Result<std::unique_ptr<ScanJob>> ScanJob::start(const fs::path& root, const ScanOptions& options)
{
    try {
        std::error_code ec;
        if (root.empty()) return makeError(ErrorCode::InvalidArgument, "scan root is empty");
        fs::path abs = fs::absolute(root, ec);
        if (ec) return makeError(ErrorCode::InvalidArgument, "cannot resolve scan root");
        abs = abs.lexically_normal();
        if (abs.has_filename() == false && abs.native().size() > 3) abs = abs.parent_path();   // bez konczacego '\'
        const auto st = fs::status(abs, ec);
        if (ec || !fs::exists(st)) return makeError(ErrorCode::NotFound, "scan root does not exist");
        if (!fs::is_directory(st)) return makeError(ErrorCode::InvalidArgument, "scan root is not a directory");

        std::unique_ptr<ScanJob> job(new ScanJob());
        job->impl_->root = std::move(abs);
        job->impl_->options = options;
        Impl* impl = job->impl_.get();
        try {
            impl->worker = std::thread([impl] { impl->run(); });
        } catch (const std::system_error&) {
            return makeError(ErrorCode::Internal, "cannot start scan thread");
        }
        return job;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory starting scan");
    }
}

void ScanJob::cancel() noexcept { impl_->cancel.requestCancel(); }

bool ScanJob::done() const noexcept { return impl_->done.load(std::memory_order_acquire); }

ScanProgress ScanJob::progress() const noexcept
{
    ScanProgress p;
    p.filesFound = impl_->filesFound.load(std::memory_order_relaxed);
    p.directoriesVisited = impl_->dirsVisited.load(std::memory_order_relaxed);
    return p;
}

std::optional<ScanResult> ScanJob::takeResult()
{
    if (!done()) return std::nullopt;
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::optional<ScanResult> out = std::move(impl_->result);
    impl_->result.reset();
    return out;
}

} // namespace vgmplay::library
