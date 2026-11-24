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
    /*BtnPrev*/ {"Previous (playlist: later)", "Poprzedni (playlista: później)"},
    /*BtnPlay*/ {"Play", "Odtwarzaj"},
    /*BtnPause*/ {"Pause", "Pauza"},
    /*BtnStop*/ {"Stop", "Stop"},
    /*BtnNext*/ {"Next (playlist: later)", "Następny (playlista: później)"},
    /*BtnOpen*/ {"Open file (Ctrl+O)", "Otwórz plik (Ctrl+O)"},
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
    /*Preview*/ {"preview", "podgląd"},
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
