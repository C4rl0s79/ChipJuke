// Teksty interfejsu w jednej tablicy (EN/PL). Zalazek pod pozniejszy system i18n:
// docelowo tablica zostanie zastapiona plikami jezykowymi, API tr(Str) moze zostac.
// Komunikaty bledow i ostrzezen z modulow (Error::code / Warning::code) tlumaczone sa po kodzie;
// Error::message (angielski, diagnostyczny) pokazujemy obok jako szczegol.
#pragma once

#include "vgmplay/core.hpp"

namespace vgmplay::ui {

enum class Lang { En, Pl };

enum class Str {
    PanelTransport,
    PanelPlaylist,
    PanelInfo,
    PanelTracker,
    PanelScope,
    BtnPrev,
    BtnPlay,
    BtnPause,
    BtnStop,
    BtnNext,
    BtnOpen,
    GdTitle,
    GdGame,
    GdSystem,
    GdAuthor,
    GdDate,
    GdRipper,
    GdNotes,
    InfoFormat,
    InfoLength,
    InfoLoop,
    InfoChips,
    LoopNone,
    LoopDisabled,
    Preview,
    NoFileHint,
    NoFileTitle,
    Loading,
    ErrorPrefix,
    WarningPrefix,
    Volume,
    HintTransport,
    PlaylistNote,
    NoChannels,
    AudioUnavailable,
    AudioSilentFallback,
    Gd3Toggle,
    Diagnostics,
    Idle,
    Fps,
    Count
};

void setLang(Lang l);
Lang lang();
const char* tr(Str s);

// Stabilna nazwa kodu (np. "Truncated") - do wyswietlenia obok komunikatu.
const char* errorCodeName(ErrorCode c);
const char* warningCodeName(WarningCode c);
// Zrozumialy komunikat dla uzytkownika w biezacym jezyku.
const char* trError(ErrorCode c);
const char* trWarning(WarningCode c);

} // namespace vgmplay::ui
