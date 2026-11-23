// Teksty makiety w jednej tablicy (EN/PL). Zalazek pod pozniejszy system i18n:
// docelowo tablica zostanie zastapiona plikami jezykowymi, API tr(Str) moze zostac.
#pragma once

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
    GdTitle,
    GdTitleJp,
    GdGame,
    GdSystem,
    GdAuthor,
    GdDate,
    GdRipper,
    GdNotes,
    Idle,
    Fps,
    Count
};

void setLang(Lang l);
Lang lang();
const char* tr(Str s);

} // namespace vgmplay::ui
