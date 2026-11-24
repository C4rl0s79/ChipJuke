// vgmplay - settings: JSON, migracje, lokalizacja pliku i zapis atomowy.
#include "vgmplay/settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

#include <nlohmann/json.hpp>

#include "fs_util.hpp"
#include "json_limits.hpp"

namespace vgmplay::settings {

namespace {

using nlohmann::json;

constexpr std::uint64_t kMaxFileBytes = 4ull << 20;
constexpr int kMaxJsonDepth = 8;               // root, sekcja, tablica, obiekt = 4 poziomy
constexpr std::size_t kMaxJsonValues = 50000;
constexpr std::size_t kMaxBindings = 1024;
constexpr std::size_t kMaxCores = 128;
constexpr std::size_t kMaxStringBytes = 4096;
constexpr std::size_t kMaxDeviceIdBytes = 512;
constexpr const char* kSettingsFileName = "settings.json";

// ---- nazwy enumow w pliku (trwale; wartosci liczbowe enumow NIE sa zapisywane) ----

// Nazwy akcji i zrodel pochodza z modulu input (input::actionName/sourceKindName) - jedno zrodlo
// prawdy; dopisanie akcji w input.hpp nie wymaga zmian tutaj.

// Czyta "action" i "source" wiazania po nazwach z modulu input. false = brak/zly typ/nieznana nazwa.
bool readBindingNames(const json& jb, input::Binding& b)
{
    auto ja = jb.find("action");
    auto js = jb.find("source");
    if (ja == jb.end() || js == jb.end() || !ja->is_string() || !js->is_string()) return false;
    const auto a = input::actionFromName(ja->get_ref<const std::string&>());
    const auto k = input::sourceKindFromName(js->get_ref<const std::string&>());
    if (!a || !k || *a == input::Action::None) return false;
    if (!input::actionRebindable(*a)) return false;   // [E3-sec] akcje zablokowane (np. wyjscie/menu) nie sa przypisywalne z pliku
    b.action = *a;
    b.source.kind = *k;
    return true;
}

struct Ctx {
    std::vector<Warning>* out;
    void warn(std::string msg) const
    {
        if (out && out->size() < 100) out->push_back(Warning{WarningCode::Other, kNoOffset, std::move(msg)});
    }
};

// double: 0.8f po rozszerzeniu to 0.800000011920929; zaokraglone do 3 miejsc daje czytelne 0.8.
double round3(float v) { return std::round(static_cast<double>(v) * 1000.0) / 1000.0; }

template <class T>
T clampWarn(const Ctx& c, const char* name, T v, T lo, T hi)
{
    if (v < lo || v > hi) {
        c.warn(std::string("value out of range, clamped: ") + name);
        return v < lo ? lo : hi;
    }
    return v;
}

// Pola o zlym typie -> domyslna wartosc + ostrzezenie. Brak pola = cicho domyslna.
bool getUint(const Ctx& c, const json& o, const char* key, std::uint64_t& out)
{
    auto it = o.find(key);
    if (it == o.end()) return false;
    if (!it->is_number_integer() || (it->is_number_integer() && !it->is_number_unsigned() && it->get<std::int64_t>() < 0)) {
        c.warn(std::string("invalid number, default used: ") + key);
        return false;
    }
    out = it->get<std::uint64_t>();
    return true;
}

bool getFloat(const Ctx& c, const json& o, const char* key, float& out)
{
    auto it = o.find(key);
    if (it == o.end()) return false;
    if (!it->is_number()) {
        c.warn(std::string("invalid number, default used: ") + key);
        return false;
    }
    const double d = it->get<double>();
    if (!std::isfinite(d)) { c.warn(std::string("non-finite number, default used: ") + key); return false; }
    out = static_cast<float>(d);
    return true;
}

bool getString(const Ctx& c, const json& o, const char* key, std::string& out)
{
    auto it = o.find(key);
    if (it == o.end()) return false;
    if (!it->is_string()) {
        c.warn(std::string("invalid string, default used: ") + key);
        return false;
    }
    std::string s = it->get<std::string>();
    if (s.size() > kMaxStringBytes) { c.warn(std::string("string too long, default used: ") + key); return false; }
    out = std::move(s);
    return true;
}

const json* getObject(const Ctx& c, const json& o, const char* key)
{
    auto it = o.find(key);
    if (it == o.end()) return nullptr;
    if (!it->is_object()) {
        c.warn(std::string("section is not an object, defaults used: ") + key);
        return nullptr;
    }
    return &*it;
}

template <class E, std::size_t N>
bool getEnumByName(const Ctx& c, const json& o, const char* key, const std::array<const char*, N>& names, E& out)
{
    std::string s;
    if (!getString(c, o, key, s)) return false;
    for (std::size_t i = 0; i < N; ++i)
        if (s == names[i]) { out = static_cast<E>(i); return true; }
    c.warn(std::string("unknown value, default used: ") + key);
    return false;
}

constexpr std::array<const char*, 2> kThemes = {"dark", "light"};
constexpr std::array<const char*, 3> kScopeModes = {"master", "perDevice", "perChannel"};
constexpr std::array<const char*, 2> kTrackerViews = {"notes", "scopes"};
constexpr std::array<const char*, 2> kGd3Langs = {"en", "jp"};
constexpr std::array<const char*, 2> kNonLoopEnds = {"stop", "fadeOut"};
constexpr std::array<const char*, 3> kRepeats = {"off", "one", "all"};
constexpr std::array<const char*, 3> kScopeBars = {"small", "large", "hidden"};

// Nazwa enuma do zapisu (wartosc spoza zakresu -> pierwsza, czyli domyslna).
template <class E, std::size_t N>
const char* enumName(const std::array<const char*, N>& names, E v)
{
    const auto i = static_cast<std::size_t>(v);
    return names[i < N ? i : 0];
}

bool getBool(const Ctx& c, const json& o, const char* key, bool& out)
{
    auto it = o.find(key);
    if (it == o.end()) return false;
    if (!it->is_boolean()) {
        c.warn(std::string("invalid boolean, default used: ") + key);
        return false;
    }
    out = it->get<bool>();
    return true;
}

ChipType chipTypeFromName(std::string_view name)
{
    for (std::uint8_t i = 1; i < static_cast<std::uint8_t>(ChipType::Count_); ++i)
        if (chipTypeName(static_cast<ChipType>(i)) == name) return static_cast<ChipType>(i);
    return ChipType::Unknown;
}

// ---- migracje: kazdy krok podnosi schemaVersion o 1 ----
// Wersja 0 = plik sprzed wprowadzenia schemaVersion (brak pola); ma ten sam uklad co v1.
void migrate(json& root, std::uint64_t from, const Ctx& c)
{
    while (from < kSettingsSchemaVersion) {
        switch (from) {
        case 0:
            c.warn("settings migrated from schema 0 to 1");
            break;
        default: break;
        }
        ++from;
    }
    root["schemaVersion"] = kSettingsSchemaVersion;
}

} // namespace

Result<Settings> fromJson(std::string_view text, std::vector<Warning>* warnings, bool allowNetworkPaths)
{
    const Ctx c{warnings};
    try {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            text.remove_prefix(3);
        auto parsed = fsutil::parseJsonLimited(text, kMaxJsonDepth, kMaxJsonValues);
        if (!parsed.ok()) return parsed.error();
        json root = std::move(parsed.value());
        if (!root.is_object()) return makeError(ErrorCode::ParseError, "invalid settings JSON");

        std::uint64_t ver = 0;
        if (auto it = root.find("schemaVersion"); it != root.end()) {
            if (!it->is_number_integer() || it->get<std::int64_t>() < 0)
                return makeError(ErrorCode::ParseError, "invalid schemaVersion");
            ver = it->get<std::uint64_t>();
        }
        if (ver > kSettingsSchemaVersion) return makeError(ErrorCode::UnsupportedSchema, "settings schema is newer than supported");
        migrate(root, ver, c);

        Settings s;
        // Flaga zapamietanej zgody na sciezki sieciowe: czytana PRZED walidacja session.trackPath.
        if (const json* lib = getObject(c, root, "library")) {
            bool b = false;
            if (getBool(c, *lib, "allowNetworkPaths", b)) s.library.allowNetworkPaths = b;
        }
        // [E3-sec] UNC w session.trackPath honorowane TYLKO przez parametr (flaga lub efektywna wartosc z aplikacji);
        // library.allowNetworkPaths z tego samego pliku nie moze sam odblokowac sciezki.
        const bool effectiveAllowNetwork = allowNetworkPaths;
        std::uint64_t u = 0;
        float f = 0;
        std::string str;

        if (const json* a = getObject(c, root, "audio")) {
            if (getString(c, *a, "device", str)) {
                // Id urzadzenia z pliku: bez znakow sterujacych/kierunkowych, max 512 B.
                s.audio.device = fsutil::sanitizeTitle(str, kMaxDeviceIdBytes);
                if (s.audio.device != str) c.warn("audio.device sanitized");
            }
            if (getUint(c, *a, "bufferMs", u)) s.audio.bufferMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 20));
            if (getUint(c, *a, "sampleRate", u)) s.audio.sampleRate = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 24));
        }
        s.audio.bufferMs = clampWarn<std::uint32_t>(c, "audio.bufferMs", s.audio.bufferMs, 10, 500);
        if (s.audio.sampleRate != 0) s.audio.sampleRate = clampWarn<std::uint32_t>(c, "audio.sampleRate", s.audio.sampleRate, 8000, 192000);

        if (const json* p = getObject(c, root, "playback")) {
            if (getUint(c, *p, "loopCount", u)) s.playback.loopCount = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 20));
            if (getUint(c, *p, "fadeMs", u)) s.playback.fadeMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 20));
            if (getFloat(c, *p, "volume", f)) s.playback.volume = f;
            getEnumByName(c, *p, "nonLoopEnd", kNonLoopEnds, s.playback.nonLoopEnd);
            getEnumByName(c, *p, "repeat", kRepeats, s.playback.repeat);
            if (auto it = p->find("cores"); it != p->end()) {
                if (!it->is_array()) c.warn("playback.cores is not an array, ignored");
                else {
                    for (const json& jc : *it) {
                        if (s.playback.cores.size() >= kMaxCores) { c.warn("too many cores entries, rest ignored"); break; }
                        std::string tn;
                        std::uint64_t inst = 0, id = 0;
                        if (!jc.is_object() || !getString(c, jc, "type", tn) || !getUint(c, jc, "coreId", id) || id > 0xFFFFFFFFu) {
                            c.warn("invalid core entry skipped");
                            continue;
                        }
                        (void)getUint(c, jc, "instance", inst);
                        const ChipType t = chipTypeFromName(tn);
                        if (t == ChipType::Unknown || inst > 1) { c.warn("unknown chip or instance in core entry skipped"); continue; }
                        s.playback.cores.push_back(CoreChoice{t, static_cast<std::uint8_t>(inst), static_cast<std::uint32_t>(id)});
                    }
                }
            }
        }
        s.playback.loopCount = clampWarn<std::uint32_t>(c, "playback.loopCount", s.playback.loopCount, 0, 99);
        s.playback.fadeMs = clampWarn<std::uint32_t>(c, "playback.fadeMs", s.playback.fadeMs, 0, 30000);
        s.playback.volume = clampWarn<float>(c, "playback.volume", s.playback.volume, 0.0f, 1.0f);

        if (const json* ui = getObject(c, root, "ui")) {
            if (getString(c, *ui, "language", str)) {
                // i18n::parseLanguageCode nie ma jeszcze implementacji w repo: lokalna tabela "pl"/"en".
                if (str == "pl") s.ui.language = i18n::Language::Pl;
                else if (str == "en") s.ui.language = i18n::Language::En;
                else c.warn("unknown ui.language, default used");
            }
            getEnumByName(c, *ui, "gd3Language", kGd3Langs, s.ui.gd3Language);
            getEnumByName(c, *ui, "theme", kThemes, s.ui.theme);
            if (getFloat(c, *ui, "fontScale", f)) s.ui.fontScale = f;
            getEnumByName(c, *ui, "scopeMode", kScopeModes, s.ui.scopeMode);
            getEnumByName(c, *ui, "trackerView", kTrackerViews, s.ui.trackerView);
            getEnumByName(c, *ui, "scopeBar", kScopeBars, s.ui.scopeBar);
            (void)getBool(c, *ui, "showButtonHints", s.ui.showButtonHints);
            if (getUint(c, *ui, "rowTicks", u)) s.ui.rowTicks = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 24));
        }
        s.ui.fontScale = clampWarn<float>(c, "ui.fontScale", s.ui.fontScale, 0.5f, 3.0f);
        s.ui.rowTicks = clampWarn<std::uint32_t>(c, "ui.rowTicks", s.ui.rowTicks, 64, 44100);

        if (const json* in = getObject(c, root, "input")) {
            if (getUint(c, *in, "repeatDelayMs", u)) s.input.repeatDelayMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 20));
            if (getUint(c, *in, "repeatRateMs", u)) s.input.repeatRateMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(u, 1u << 20));
            if (auto it = in->find("bindings"); it != in->end()) {
                if (!it->is_array()) c.warn("input.bindings is not an array, ignored");
                else {
                    for (const json& jb : *it) {
                        if (s.input.bindings.size() >= kMaxBindings) { c.warn("too many bindings, rest ignored"); break; }
                        input::Binding b;
                        std::uint64_t unused = 0;
                        (void)unused;
                        auto code = jb.is_object() ? jb.find("code") : jb.end();
                        // [E3-sec] unsigned > INT32_MAX odrzucamy PRZED rzutowaniem na int64 (bez zawijania).
                        const bool codeOk = jb.is_object() && code != jb.end() && code->is_number_integer() &&
                                            (code->is_number_unsigned() ? code->get<std::uint64_t>() <= static_cast<std::uint64_t>(INT32_MAX)
                                                                        : (code->get<std::int64_t>() >= INT32_MIN &&
                                                                           code->get<std::int64_t>() <= INT32_MAX));
                        if (!codeOk || !readBindingNames(jb, b)) {
                            c.warn("invalid binding skipped");
                            continue;
                        }
                        b.source.code = static_cast<std::int32_t>(code->is_number_unsigned() ? static_cast<std::int64_t>(code->get<std::uint64_t>())
                                                                                              : code->get<std::int64_t>());
                        s.input.bindings.push_back(b);
                    }
                }
            }
        }
        s.input.repeatDelayMs = clampWarn<std::uint32_t>(c, "input.repeatDelayMs", s.input.repeatDelayMs, 100, 2000);
        s.input.repeatRateMs = clampWarn<std::uint32_t>(c, "input.repeatRateMs", s.input.repeatRateMs, 10, 1000);

        if (const json* se = getObject(c, root, "session")) {
            if (getString(c, *se, "trackPath", str)) {
                if (str.empty() || !fsutil::isUnsafeStoredPath(str, effectiveAllowNetwork)) s.session.trackPath = str;
                else c.warn("session.trackPath rejected (URL, network or device path)");
            }
            if (getUint(c, *se, "positionMs", u)) s.session.positionMs = (std::min<std::uint64_t>)(u, 24ull * 3600 * 1000);
        }
        return s;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory parsing settings");
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::ParseError, std::string("settings JSON: ") + ex.what());
    }
}

std::string toJson(const Settings& s)
{
    json root = json::object();
    root["schemaVersion"] = kSettingsSchemaVersion;
    root["audio"] = {{"device", s.audio.device}, {"bufferMs", s.audio.bufferMs}, {"sampleRate", s.audio.sampleRate}};

    json cores = json::array();
    for (const CoreChoice& ch : s.playback.cores)
        cores.push_back({{"type", std::string(chipTypeName(ch.type))}, {"instance", ch.instance}, {"coreId", ch.coreId}});
    const auto nl = static_cast<std::size_t>(s.playback.nonLoopEnd);
    root["playback"] = {{"loopCount", s.playback.loopCount},
                        {"fadeMs", s.playback.fadeMs},
                        {"volume", round3(s.playback.volume)},
                        {"nonLoopEnd", kNonLoopEnds[nl < kNonLoopEnds.size() ? nl : 0]},
                        {"repeat", enumName(kRepeats, s.playback.repeat)},
                        {"cores", std::move(cores)}};

    root["ui"] = {{"language", s.ui.language == i18n::Language::En ? "en" : "pl"},
                  {"gd3Language", kGd3Langs[static_cast<std::size_t>(s.ui.gd3Language) < 2 ? static_cast<std::size_t>(s.ui.gd3Language) : 0]},
                  {"theme", kThemes[static_cast<std::size_t>(s.ui.theme) < 2 ? static_cast<std::size_t>(s.ui.theme) : 0]},
                  {"fontScale", round3(s.ui.fontScale)},
                  {"scopeMode", kScopeModes[static_cast<std::size_t>(s.ui.scopeMode) < 3 ? static_cast<std::size_t>(s.ui.scopeMode) : 0]},
                  {"trackerView", kTrackerViews[static_cast<std::size_t>(s.ui.trackerView) < 2 ? static_cast<std::size_t>(s.ui.trackerView) : 0]},
                  {"scopeBar", enumName(kScopeBars, s.ui.scopeBar)},
                  {"showButtonHints", s.ui.showButtonHints},
                  {"rowTicks", s.ui.rowTicks}};

    json binds = json::array();
    for (const input::Binding& b : s.input.bindings) {
        const std::string_view an = input::actionName(b.action);
        const std::string_view kn = input::sourceKindName(b.source.kind);
        if (an.empty() || kn.empty()) continue;
        binds.push_back({{"action", std::string(an)}, {"source", std::string(kn)}, {"code", b.source.code}});
    }
    root["input"] = {{"bindings", std::move(binds)}, {"repeatDelayMs", s.input.repeatDelayMs}, {"repeatRateMs", s.input.repeatRateMs}};
    root["library"] = {{"allowNetworkPaths", s.library.allowNetworkPaths}};
    root["session"] = {{"trackPath", s.session.trackPath}, {"positionMs", s.session.positionMs}};
    return root.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

bool isDirectoryWritable(const std::filesystem::path& dir)
{
    if (dir.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    // Proba utworzenia pliku, ktory system sam usunie przy zamknieciu uchwytu.
    static std::atomic<std::uint32_t> counter{0};
    std::filesystem::path probe = dir / (L".vgmplay-write-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                         std::to_wstring(counter.fetch_add(1)) + L".tmp");
    // DELETE jest wymagane przez FILE_FLAG_DELETE_ON_CLOSE (bez niego folder zapisywalny moglby byc uznany za niezapisywalny).
    HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

SettingsLocation resolveSettingsLocation(const std::filesystem::path& exeDir, const std::filesystem::path& appDataDir)
{
    if (isDirectoryWritable(exeDir)) return SettingsLocation{exeDir / kSettingsFileName, true};
    // appDataDir ze sciezka sieciowa / przestrzenia urzadzen (UNC, \?\, \.\) jest odrzucany: traktujemy jak brak.
    if (appDataDir.empty() || fsutil::startsWithDoubleSeparator(appDataDir.native())) return SettingsLocation{exeDir / kSettingsFileName, false};   // ostatnia deska ratunku
    return SettingsLocation{appDataDir / L"vgmplay" / kSettingsFileName, false};
}

Result<Settings> loadSettings(const std::filesystem::path& file, std::vector<Warning>* warnings, bool allowNetworkPaths)
{
    auto text = fsutil::readFileLimited(file, kMaxFileBytes);
    if (!text.ok()) {
        if (text.error().code == ErrorCode::NotFound) return Settings{};   // pierwsze uruchomienie
        return text.error();
    }
    auto s = fromJson(text.value(), warnings, allowNetworkPaths);
    if (!s.ok()) {
        // Oryginal zostaje; kopia .bad pozwala uzytkownikowi odzyskac reczne zmiany.
        (void)fsutil::preserveCorruptCopy(file, L".bad");
    }
    return s;
}

Result<void> saveSettings(const std::filesystem::path& file, const Settings& settings)
{
    try {
        return fsutil::atomicWriteFile(file, toJson(settings));
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory saving settings");
    }
}

} // namespace vgmplay::settings
