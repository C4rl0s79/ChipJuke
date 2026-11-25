#include "vgmplay_ui/strings.h"

#include <cstddef>

namespace vgmplay::ui {
namespace {

struct Entry {
    const char* en;
    const char* pl;
};

constexpr Entry kTexts[] = {
    /*PanelTransport*/ {"TRANSPORT", "ODTWARZANIE"},
    /*PanelPlaylist*/ {"PLAYLIST", "PLAYLISTA"},
    /*PanelInfo*/ {"GD3 INFO", "INFO GD3"},
    /*PanelTracker*/ {"PATTERN", "WZORZEC"},
    /*PanelScope*/ {"OSCILLOSCOPE", "OSCYLOSKOP"},
    /*BtnPrev*/ {"Previous (P): within the first 3 s, later restarts the track", "Poprzedni (P): w pierwszych 3 s, później początek utworu"},
    /*BtnPlay*/ {"Play", "Odtwarzaj"},
    /*BtnPause*/ {"Pause", "Pauza"},
    /*BtnStop*/ {"Stop", "Stop"},
    /*BtnNext*/ {"Next (N)", "Następny (N)"},
    /*BtnOpen*/ {"Add files (Ctrl+O)", "Dodaj pliki (Ctrl+O)"},
    /*GdTitle*/ {"Title", "Tytuł"},
    /*GdGame*/ {"Game", "Gra"},
    /*GdSystem*/ {"System", "System"},
    /*GdAuthor*/ {"Author", "Autor"},
    /*GdDate*/ {"Release", "Wydanie"},
    /*GdRipper*/ {"VGM by", "VGM wykonał"},
    /*GdNotes*/ {"Notes", "Uwagi"},
    /*InfoFormat*/ {"Format", "Format"},
    /*InfoLength*/ {"Length", "Długość"},
    /*InfoLoop*/ {"Loop", "Pętla"},
    /*InfoChips*/ {"Chips", "Układy"},
    /*LoopNone*/ {"none", "brak"},
    /*LoopDisabled*/ {"loop disabled", "pętla wyłączona"},
    /*PatternBuilding*/ {"building pattern...", "budowanie wzorca..."},
    /*NoFileHint*/ {"Drop a .vgm / .vgz file here or press Ctrl+O",
                    "Upuść plik .vgm / .vgz tutaj lub naciśnij Ctrl+O"},
    /*NoFileTitle*/ {"(no file)", "(brak pliku)"},
    /*Loading*/ {"Loading", "Wczytywanie"},
    /*ErrorPrefix*/ {"Error", "Błąd"},
    /*WarningPrefix*/ {"Note", "Uwaga"},
    /*Volume*/ {"Vol", "Gł."},
    /*HintTransport*/ {"Left/Right seek 5 s   Up/Down volume   Enter play/pause   Esc stop",
                       "Lewo/Prawo przewiń 5 s   Góra/Dół głośność   Enter odtwarzaj/pauza   Esc stop"},
    /*PlaylistNote*/ {"Playlist arrives in a later version.", "Playlista pojawi się w kolejnej wersji."},
    /*NoChannels*/ {"No channels - open a file", "Brak kanałów - otwórz plik"},
    /*AudioUnavailable*/ {"Audio device unavailable", "Urządzenie audio niedostępne"},
    /*AudioSilentFallback*/ {"playing without sound", "odtwarzanie bez dźwięku"},
    /*Gd3Toggle*/ {"Tag language (F3)", "Język tagów (F3)"},
    /*Diagnostics*/ {"DIAGNOSTICS (F12)", "DIAGNOSTYKA (F12)"},
    /*Idle*/ {"IDLE", "BEZCZYNNOŚĆ"},
    /*Fps*/ {"FPS", "KL/S"},
    /*PanelChannels*/ {"CHANNELS", "KANAŁY"},
    /*ViewScopes*/ {"Scopes", "Oscyloskopy"},
    /*RowSpeed*/ {"Row", "Wiersz"},
    /*BtnUnmuteAll*/ {"Unmute all", "Włącz wszystkie"},
    /*BtnClearSolo*/ {"Clear solo", "Wyczyść solo"},
    /*BtnMuteAll*/ {"Mute all", "Wycisz wszystkie"},
    /*TipMute*/ {"Mute", "Wycisz"},
    /*TipSolo*/ {"Solo", "Solo"},
    /*MuteTag*/ {"MUTE", "MUTE"},
    /*ScopeNa*/ {"n/a", "b/d"},
    /*HintChannels*/ {"M mute  S solo  U unmute all  C clear solo  R reset  1-0 channel  F4 scopes  +/- row",
                      "M wycisz  S solo  U włącz wszystkie  C wyczyść solo  R reset  1-0 kanał  F4 oscyl.  +/- wiersz"},
    /*ViewToggle*/ {"Scope strip: small / large / hidden (F4)", "Pasek oscyloskopów: mały / duży / ukryty (F4)"},
    /*StripSmall*/ {"small", "mały"},
    /*StripLarge*/ {"large", "duży"},
    /*StripHidden*/ {"hidden", "ukryty"},
    /*RowSpeedTip*/ {"Row length: +/- (shorter = more rows per second)", "Długość wiersza: +/- (krótszy = więcej wierszy na sekundę)"},
    /*PatternNone*/ {"no pattern data", "brak danych wzorca"},
    /*PlShuffle*/ {"Shuffle (F7)", "Losuj (F7)"},
    /*PlRepeatOff*/ {"Repeat: off", "Powtarzaj: wył."},
    /*PlRepeatOne*/ {"Repeat: one", "Powtarzaj: utwór"},
    /*PlRepeatAll*/ {"Repeat: all", "Powtarzaj: lista"},
    /*PlRepeatTip*/ {"Repeat mode (F6): off / one track / whole list", "Tryb powtarzania (F6): wył. / utwór / cała lista"},
    /*PlAddFiles*/ {"+ Files", "+ Pliki"},
    /*PlAddFolder*/ {"+ Folder", "+ Folder"},
    /*PlImport*/ {"M3U in", "M3U wej."},
    /*PlExport*/ {"M3U out", "M3U wyj."},
    /*PlNew*/ {"New", "Nowa"},
    /*PlRename*/ {"Rename", "Nazwa"},
    /*PlDelete*/ {"Delete", "Usuń"},
    /*PlPrevList*/ {"Previous playlist", "Poprzednia playlista"},
    /*PlNextList*/ {"Next playlist", "Następna playlista"},
    /*PlEmpty*/ {"Empty - drop files or folders here, or press Ctrl+O", "Pusta - upuść tu pliki lub foldery albo naciśnij Ctrl+O"},
    /*PlHint*/ {"Up/Down select  Enter play  Del remove  Ctrl+Up/Down move  Left/Right list  Y menu", "Góra/Dół wybór  Enter graj  Del usuń  Ctrl+Góra/Dół przenieś  Lewo/Prawo lista  Y menu"},
    /*PlScanning*/ {"Scanning: %llu files, %llu folders", "Skanowanie: %llu plików, %llu folderów"},
    /*PlCancel*/ {"Cancel", "Anuluj"},
    /*PlLoadingLists*/ {"Loading playlists...", "Wczytywanie playlist..."},
    /*DlgNewList*/ {"New playlist", "Nowa playlista"},
    /*DlgRenameList*/ {"Rename playlist", "Zmiana nazwy playlisty"},
    /*DlgName*/ {"Name:", "Nazwa:"},
    /*DlgDeleteList*/ {"Delete playlist '%s'?", "Usunąć playlistę '%s'?"},
    /*DlgDeleteEntry*/ {"Remove from the playlist: %s ?", "Usunąć z playlisty: %s ?"},
    /*DlgYes*/ {"Yes (Enter / A)", "Tak (Enter / A)"},
    /*DlgNo*/ {"No (Esc / B)", "Nie (Esc / B)"},
    /*DlgOk*/ {"OK (Enter / A)", "OK (Enter / A)"},
    /*DlgInfo*/ {"INFORMATION", "INFORMACJA"},
    /*DlgAckSave*/ {"Understood - allow saving new data (Enter / A)", "Rozumiem - zezwól na zapis nowych danych (Enter / A)"},
    /*MenuPlay*/ {"Play", "Odtwórz"},
    /*MenuRemove*/ {"Remove", "Usuń"},
    /*MenuMoveUp*/ {"Move up", "Przenieś w górę"},
    /*MenuMoveDown*/ {"Move down", "Przenieś w dół"},
    /*MenuCancel*/ {"Cancel", "Anuluj"},
    /*NtAdded*/ {"Added %u file(s) to '%s'", "Dodano plików: %u do '%s'"},
    /*NtSkipped*/ {"skipped %u non-VGM file(s)", "pominięto plików innych niż VGM: %u"},
    /*NtScanDone*/ {"Folder scan: added %u file(s)", "Skan folderu: dodano plików: %u"},
    /*NtScanCancelled*/ {"Folder scan cancelled", "Skan folderu anulowano"},
    /*NtScanFailed*/ {"Cannot scan the folder", "Nie można przeskanować folderu"},
    /*NtScanTruncated*/ {"scan stopped at a limit", "skan zatrzymano na limicie"},
    /*NtImport*/ {"M3U import '%s': added %u, rejected %u", "Import M3U '%s': dodano %u, odrzucono %u"},
    /*NtImportFail*/ {"M3U import failed", "Import M3U nie powiódł się"},
    /*NtTooBig*/ {"M3U file is too large (limit 16 MB)", "Plik M3U jest za duży (limit 16 MB)"},
    /*NtExportOk*/ {"M3U exported: %s", "Wyeksportowano M3U: %s"},
    /*NtExportFail*/ {"M3U export failed", "Eksport M3U nie powiódł się"},
    /*NtPlaylistsBad*/ {"playlists.json is damaged; the original was kept as a .bak copy. Nothing is written over it until you confirm.", "Plik playlists.json jest uszkodzony; oryginał zachowano jako kopię .bak. Nic go nie nadpisze, dopóki nie potwierdzisz."},
    /*NtSettingsBad*/ {"settings.json is damaged; defaults are used and the original was kept as a .bad copy. Nothing is written over it until you confirm.", "Plik settings.json jest uszkodzony; użyto domyślnych, oryginał zachowano jako kopię .bad. Nic go nie nadpisze, dopóki nie potwierdzisz."},
    /*NtSaveFail*/ {"Saving failed", "Zapis nie powiódł się"},
    /*NtNoAppData*/ {"The AppData folder is unavailable; settings are not saved", "Folder AppData jest niedostępny; ustawienia nie będą zapisywane"},
    /*NtNonVgm*/ {"Not a .vgm/.vgz file", "To nie jest plik .vgm/.vgz"},
    /*NtListGone*/ {"The target playlist no longer exists; added to the active one", "Docelowa playlista już nie istnieje; dodano do aktywnej"},
    /*SetTitle*/ {"SETTINGS (F10)", "USTAWIENIA (F10)"},
    /*SetAudio*/ {"Audio device", "Urządzenie audio"},
    /*SetAudioDefault*/ {"system default", "domyślne systemowe"},
    /*SetLoops*/ {"Loop count", "Liczba pętli"},
    /*SetLoopsInf*/ {"endless", "bez końca"},
    /*SetFade*/ {"Fade-out", "Wyciszanie"},
    /*SetNonLoop*/ {"End without loop", "Koniec bez pętli"},
    /*SetStop*/ {"stop", "stop"},
    /*SetFadeOut*/ {"fade out", "wyciszenie"},
    /*SetVolume*/ {"Volume", "Głośność"},
    /*SetLanguage*/ {"Language", "Język"},
    /*SetGd3*/ {"GD3 tags", "Tagi GD3"},
    /*SetRowLen*/ {"Pattern row length", "Długość wiersza wzorca"},
    /*SetControls*/ {"Controls", "Sterowanie"},
    /*SetHint*/ {"Up/Down select   Left/Right change   Esc/B close", "Góra/Dół wybór   Lewo/Prawo zmiana   Esc/B zamknij"},
    /*SetCtl1*/ {"Tab / Shift+Tab / LB / RB: switch panel   Space / Start: play-pause", "Tab / Shift+Tab / LB / RB: zmiana panelu   Spacja / Start: odtwarzaj-pauza"},
    /*SetCtl2*/ {"Playlist: Enter / A play   Del / X remove   Ctrl+Up/Down move   Y / right-click menu", "Playlista: Enter / A graj   Del / X usuń   Ctrl+Góra/Dół przenieś   Y / prawy klik menu"},
    /*SetCtl3*/ {"P / N / stick click: prev / next   F6 repeat   F7 shuffle   F10 / Y: settings", "P / N / klik drążka: poprz. / nast.   F6 powtarzanie   F7 losowanie   F10 / Y: ustawienia"},
    /*SetCtl4*/ {"Ctrl+O files   Ctrl+Shift+O folder   Ctrl+I import M3U   Ctrl+E export M3U", "Ctrl+O pliki   Ctrl+Shift+O folder   Ctrl+I import M3U   Ctrl+E eksport M3U"},
    /*SetCtl5*/ {"Key and pad bindings: Settings > Bindings (default layout is listed above)", "Przypisania klawiszy i pada: Ustawienia > Przypisania (układ domyślny wyżej)"},
    /*SetCtl6*/ {"Playlists: F8 new   F9 rename   Shift+Del delete   Left/Right (in the list) switch", "Playlisty: F8 nowa   F9 nazwa   Shift+Del usuń   Lewo/Prawo (w liście) przełączanie"},
    /*DefaultListName*/ {"Playlist", "Playlista"},
    /*CloudTag*/ {"cloud file, not downloaded", "plik w chmurze (nie pobrano)"},
    /*MenuDownload*/ {"Download anyway (cloud file)", "Pobierz mimo to (plik w chmurze)"},
    /*SetNetwork*/ {"Network paths (UNC)", "Ścieżki sieciowe (UNC)"},
    /*SetOn*/ {"allowed", "dozwolone"},
    /*SetOff*/ {"blocked", "zablokowane"},
    /*SetNetworkNote*/ {"enable: --allow-network-paths", "włącz: --allow-network-paths"},
    /*SetHints*/ {"Button hints", "Podpowiedzi przycisków"},
    /*SetHintsAlways*/ {"shown", "widoczne"},
    /*SetHintsPadOnly*/ {"hidden", "ukryte"},
    /*SetBindings*/ {"Key and pad bindings", "Przypisania klawiszy i pada"},
    /*SetBindingsOpen*/ {"change...", "zmień..."},
    /*SetNetworkForced*/ {"forced by --allow-network-paths", "wymuszone flagą --allow-network-paths"},
    /*SetAudioRefresh*/ {"A / Enter: refresh list", "A / Enter: odśwież listę"},
    /*SetAudioNone*/ {"(no devices found)", "(nie znaleziono urządzeń)"},
    /*SetAudioFallback*/ {"Device not found - using system default", "Nie znaleziono urządzenia - użyto domyślnego"},
    /*BindTitle*/ {"BINDINGS", "PRZYPISANIA"},
    /*BindKeyboard*/ {"Keyboard", "Klawiatura"},
    /*BindPad*/ {"Gamepad", "Pad"},
    /*BindRestore*/ {"Restore defaults", "Przywróć domyślne"},
    /*BindPress*/ {"Press a button... (Esc / hold B: cancel)", "Naciśnij przycisk... (Esc / przytrzymaj B: anuluj)"},
    /*BindPressKey*/ {"Press a key... (Esc: cancel)", "Naciśnij klawisz... (Esc: anuluj)"},
    /*BindCancelled*/ {"Cancelled", "Anulowano"},
    /*BindTimeout*/ {"Time is up", "Upłynął czas"},
    /*BindConflict*/ {"%s is already used by %s. Enter / A: swap, Esc / B: cancel", "%s jest już przypisany do %s. Enter / A: zamień, Esc / B: anuluj"},
    /*BindCritical*/ {"This would leave a required action without a pad button", "To zostawiłoby wymaganą akcję bez przycisku pada"},
    /*BindHint*/ {"Left/Right column   Enter / A assign   Del / X clear   Esc / B back", "Lewo/Prawo kolumna   Enter / A przypisz   Del / X wyczyść   Esc / B wstecz"},
    /*HkOk*/ {"OK", "OK"},
    /*HkSelect*/ {"Select", "Wybierz"},
    /*HkBack*/ {"Back", "Wstecz"},
    /*HkMenu*/ {"Menu", "Menu"},
    /*HkPanel*/ {"Panel", "Panel"},
    /*HkMove*/ {"Move", "Nawigacja"},
    /*HkAdjust*/ {"Change", "Zmień"},
    /*HkPlayPause*/ {"Play/Pause", "Odtwórz/Pauza"},
    /*HkPlay*/ {"Play", "Odtwórz"},
    /*HkStop*/ {"Stop", "Stop"},
    /*HkSettings*/ {"Settings", "Ustawienia"},
    /*HkRemove*/ {"Remove", "Usuń"},
    /*HkEntryMenu*/ {"Entry menu", "Menu wpisu"},
    /*HkSolo*/ {"Solo", "Solo"},
    /*HkMute*/ {"Mute", "Wycisz"},
    /*HkResetMix*/ {"Reset mix", "Reset miksu"},
    /*HkChannel*/ {"Channel", "Kanał"},
    /*HkScroll*/ {"Scroll", "Przewiń"},
    /*HkTagLang*/ {"Tag language", "Język tagów"},
    /*HkAssign*/ {"Assign", "Przypisz"},
    /*HkClear*/ {"Clear", "Wyczyść"},
    /*HkYes*/ {"Yes", "Tak"},
    /*HkCancel*/ {"Cancel", "Anuluj"},
    /*HkSwap*/ {"Swap", "Zamień"},
    /*HkTextKbd*/ {"Name editing needs a keyboard", "Edycja nazwy wymaga klawiatury"},
    /*DlgNetworkWarn*/ {"Warning: opening a network path (\\\\server\\share) can make Windows send your login credentials "
                        "to the server named in a playlist or settings file. Enable network paths only for files you trust.",
                        "Uwaga: otwarcie ścieżki sieciowej (\\\\serwer\\udział) może sprawić, że Windows wyśle dane logowania "
                        "do serwera wskazanego w pliku playlisty lub ustawień. Włącz ścieżki sieciowe tylko dla zaufanych plików."},
    /*SetAudioNoOutput*/ {"no audio output - retrying", "brak wyjścia audio - ponawiam"},
    // [E4] przegladarka plikow (pad)
    /*BrwTitleAdd*/ {"ADD FILES", "DODAJ PLIKI"},
    /*BrwTitleFolder*/ {"ADD FOLDER", "DODAJ FOLDER"},
    /*BrwTitleImport*/ {"IMPORT M3U", "IMPORT M3U"},
    /*BrwTitleExport*/ {"EXPORT M3U - choose a folder", "EKSPORT M3U - wybierz folder"},
    /*BrwRoot*/ {"This computer", "Ten komputer"},
    /*BrwLoading*/ {"Reading folder...", "Wczytuję folder..."},
    /*BrwEmpty*/ {"Nothing to show here", "Brak pozycji do pokazania"},
    /*BrwTruncated*/ {"Only the first %u entries are shown (limit)", "Pokazano tylko pierwsze %u pozycji (limit)"},
    /*BrwTimedOut*/ {"Reading took too long - the list is incomplete", "Wczytywanie trwało zbyt długo - lista jest niepełna"},
    /*BrwBlockedNet*/ {"Network paths are blocked (Settings > Network paths)", "Ścieżki sieciowe są zablokowane (Ustawienia > Ścieżki sieciowe)"},
    /*BrwError*/ {"Cannot read the folder: %s", "Nie można odczytać folderu: %s"},
    /*BrwMarked*/ {"marked: %u", "zaznaczone: %u"},
    /*BrwBtnUp*/ {"Up", "W górę"},
    /*BrwBtnAddHere*/ {"Add this folder", "Dodaj ten folder"},
    /*BrwBtnAddMarked*/ {"Add marked", "Dodaj zaznaczone"},
    /*BrwBtnSaveHere*/ {"Save here", "Zapisz tutaj"},
    /*BrwBtnClose*/ {"Close", "Zamknij"},
    /*BrwSaveAs*/ {"Will be saved as: %s", "Zapiszę jako: %s"},
    /*BrwLink*/ {"link", "dowiązanie"},
    /*BrwCloud*/ {"cloud", "chmura"},
    /*BrwDriveNet*/ {"network", "sieciowy"},
    /*BrwDriveRem*/ {"removable", "wymienny"},
    /*BrwDriveOpt*/ {"optical", "optyczny"},
    /*BrwNetBlockedTag*/ {"network - enable network paths in Settings", "sieciowy - włącz ścieżki sieciowe w Ustawieniach"},
    /*BrwRecent*/ {"recent", "ostatni"},
    /*HkPage*/ {"Page", "Strona"},
    /*HkAddFolder*/ {"Add folder", "Dodaj folder"},
    /*HkMark*/ {"Mark", "Zaznacz"},
    /*HkUp*/ {"Up", "W górę"},
    /*HkSaveHere*/ {"Save here", "Zapisz tutaj"},
    /*HkClose*/ {"Close", "Zamknij"},
    /*HkOpenDir*/ {"Open", "Otwórz"},
};
static_assert(sizeof(kTexts) / sizeof(kTexts[0]) == static_cast<std::size_t>(Str::Count),
              "tablica tekstow musi pokrywac cale enum Str");

Lang g_lang = Lang::En;

const char* pick(const char* en, const char* pl) { return g_lang == Lang::Pl ? pl : en; }

} // namespace

void setLang(Lang l) { g_lang = l; }
Lang lang() { return g_lang; }

const char* tr(Str s)
{
    const Entry& e = kTexts[static_cast<std::size_t>(s)];
    return g_lang == Lang::Pl ? e.pl : e.en;
}

const char* errorCodeName(ErrorCode c)
{
    switch (c) {
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::Io: return "Io";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AccessDenied: return "AccessDenied";
    case ErrorCode::TooLarge: return "TooLarge";
    case ErrorCode::DecompressionFailed: return "DecompressionFailed";
    case ErrorCode::Truncated: return "Truncated";
    case ErrorCode::BadSignature: return "BadSignature";
    case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    case ErrorCode::BadHeader: return "BadHeader";
    case ErrorCode::BadCommandStream: return "BadCommandStream";
    case ErrorCode::BadDataBlock: return "BadDataBlock";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::OutOfMemory: return "OutOfMemory";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::PathRejected: return "PathRejected";
    case ErrorCode::ParseError: return "ParseError";
    case ErrorCode::UnsupportedSchema: return "UnsupportedSchema";
    case ErrorCode::DecoderFailure: return "DecoderFailure";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::CloudPlaceholder: return "CloudPlaceholder";
    }
    return "Unknown";
}

const char* warningCodeName(WarningCode c)
{
    switch (c) {
    case WarningCode::UnknownVersion: return "UnknownVersion";
    case WarningCode::EofOffsetMismatch: return "EofOffsetMismatch";
    case WarningCode::HeaderLengthMismatch: return "HeaderLengthMismatch";
    case WarningCode::EndsEarly: return "EndsEarly";
    case WarningCode::UnknownCommand: return "UnknownCommand";
    case WarningCode::ChipNotInHeader: return "ChipNotInHeader";
    case WarningCode::TooManyChipInstances: return "TooManyChipInstances";
    case WarningCode::BlockAfterLoop: return "BlockAfterLoop";
    case WarningCode::BlockAfterSetData: return "BlockAfterSetData";
    case WarningCode::Gd3Invalid: return "Gd3Invalid";
    case WarningCode::TagTruncated: return "TagTruncated";
    case WarningCode::TagSanitized: return "TagSanitized";
    case WarningCode::RomMissing: return "RomMissing";
    case WarningCode::EntrySkipped: return "EntrySkipped";
    case WarningCode::PathRejected: return "PathRejected";
    case WarningCode::ReparsePointSkipped: return "ReparsePointSkipped";
    case WarningCode::AccessDenied: return "AccessDenied";
    case WarningCode::LimitReached: return "LimitReached";
    case WarningCode::Other: return "Other";
    }
    return "Unknown";
}

const char* trError(ErrorCode c)
{
    switch (c) {
    case ErrorCode::InvalidArgument: return pick("Invalid argument.", "Nieprawidłowy argument.");
    case ErrorCode::Io: return pick("The file could not be read.", "Nie udało się odczytać pliku.");
    case ErrorCode::NotFound: return pick("File not found.", "Nie znaleziono pliku.");
    case ErrorCode::AccessDenied: return pick("Access to the file was denied.", "Brak dostępu do pliku.");
    case ErrorCode::TooLarge: return pick("The file is too large.", "Plik jest zbyt duży.");
    case ErrorCode::DecompressionFailed:
        return pick("The compressed file is damaged or incomplete.", "Plik skompresowany jest uszkodzony lub niepełny.");
    case ErrorCode::Truncated: return pick("The file is cut short.", "Plik jest ucięty.");
    case ErrorCode::BadSignature: return pick("Not a VGM file (wrong signature).", "To nie jest plik VGM (zła sygnatura).");
    case ErrorCode::UnsupportedVersion: return pick("Unsupported VGM version.", "Nieobsługiwana wersja VGM.");
    case ErrorCode::BadHeader: return pick("The VGM header is inconsistent.", "Nagłówek VGM jest niespójny.");
    case ErrorCode::BadCommandStream: return pick("The VGM data stream is invalid.", "Strumień danych VGM jest nieprawidłowy.");
    case ErrorCode::BadDataBlock: return pick("A VGM data block is invalid.", "Blok danych VGM jest nieprawidłowy.");
    case ErrorCode::LimitExceeded: return pick("A safety limit was exceeded.", "Przekroczono limit bezpieczeństwa.");
    case ErrorCode::OutOfMemory: return pick("Out of memory.", "Brak pamięci.");
    case ErrorCode::Cancelled: return pick("Cancelled.", "Anulowano.");
    case ErrorCode::PathRejected: return pick("The path was rejected.", "Ścieżka została odrzucona.");
    case ErrorCode::ParseError: return pick("Syntax error.", "Błąd składni.");
    case ErrorCode::UnsupportedSchema: return pick("Unsupported data version.", "Nieobsługiwana wersja danych.");
    case ErrorCode::DecoderFailure: return pick("The sound emulator failed.", "Emulator dźwięku zgłosił błąd.");
    case ErrorCode::Internal: return pick("Internal error.", "Błąd wewnętrzny.");
    case ErrorCode::CloudPlaceholder: return pick("The file is in the cloud and not downloaded.", "Plik jest w chmurze i nie jest pobrany.");
    }
    return pick("Unknown error.", "Nieznany błąd.");
}

const char* trWarning(WarningCode c)
{
    switch (c) {
    case WarningCode::UnknownVersion: return pick("newer VGM version than known", "wersja VGM nowsza niż znana");
    case WarningCode::EofOffsetMismatch: return pick("file size mismatch (corrected)", "rozmiar pliku niezgodny (poprawiono)");
    case WarningCode::HeaderLengthMismatch: return pick("header length differs from the data", "długość w nagłówku różni się od danych");
    case WarningCode::EndsEarly: return pick("no end command", "brak komendy końca");
    case WarningCode::UnknownCommand: return pick("unknown command skipped", "pominięto nieznaną komendę");
    case WarningCode::ChipNotInHeader: return pick("chip used without a clock in the header", "układ użyty bez zegara w nagłówku");
    case WarningCode::TooManyChipInstances: return pick("extra chip instances ignored", "nadmiarowe układy zignorowano");
    case WarningCode::BlockAfterLoop: return pick("loop disabled (data block after loop point)", "pętla wyłączona (blok danych po punkcie pętli)");
    case WarningCode::BlockAfterSetData: return pick("data block after bank selection", "blok danych po wyborze banku");
    case WarningCode::Gd3Invalid: return pick("GD3 tag is damaged (ignored)", "tag GD3 uszkodzony (pominięto)");
    case WarningCode::TagTruncated: return pick("a GD3 field was shortened", "pole GD3 skrócono");
    case WarningCode::TagSanitized: return pick("control characters removed from tags", "usunięto znaki sterujące z tagów");
    case WarningCode::RomMissing: return pick("OPL4 sample ROM yrw801.rom not found (put it next to ChipJuke.exe or the track) - ROM instruments may be silent",
                                              "brak ROM-u próbek OPL4 yrw801.rom (połóż go obok ChipJuke.exe albo utworu) - instrumenty z ROM-u mogą być ciche");
    case WarningCode::EntrySkipped: return pick("entry skipped", "pominięto pozycję");
    case WarningCode::PathRejected: return pick("path rejected", "ścieżka odrzucona");
    case WarningCode::ReparsePointSkipped: return pick("link skipped", "pominięto dowiązanie");
    case WarningCode::AccessDenied: return pick("access denied", "brak dostępu");
    case WarningCode::LimitReached: return pick("limit reached", "osiągnięto limit");
    case WarningCode::Other: return pick("warning", "ostrzeżenie");
    }
    return pick("warning", "ostrzeżenie");
}

} // namespace vgmplay::ui
