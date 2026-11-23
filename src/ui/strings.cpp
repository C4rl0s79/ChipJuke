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
    /*BtnPrev*/ {"Previous", "Poprzedni"},
    /*BtnPlay*/ {"Play", "Odtwarzaj"},
    /*BtnPause*/ {"Pause", "Pauza"},
    /*BtnStop*/ {"Stop", "Stop"},
    /*BtnNext*/ {"Next", "Następny"},
    /*GdTitle*/ {"Title", "Tytuł"},
    /*GdTitleJp*/ {"Title (JP)", "Tytuł (JP)"},
    /*GdGame*/ {"Game", "Gra"},
    /*GdSystem*/ {"System", "System"},
    /*GdAuthor*/ {"Author", "Autor"},
    /*GdDate*/ {"Release", "Wydanie"},
    /*GdRipper*/ {"Ripper", "Rip"},
    /*GdNotes*/ {"Notes", "Uwagi"},
    /*Idle*/ {"IDLE", "BEZCZYNNOŚĆ"},
    /*Fps*/ {"FPS", "KL/S"},
};
static_assert(sizeof(kTexts) / sizeof(kTexts[0]) == static_cast<std::size_t>(Str::Count),
              "tablica tekstow musi pokrywac cale enum Str");

Lang g_lang = Lang::En;

} // namespace

void setLang(Lang l) { g_lang = l; }
Lang lang() { return g_lang; }

const char* tr(Str s)
{
    const Entry& e = kTexts[static_cast<std::size_t>(s)];
    return g_lang == Lang::Pl ? e.pl : e.en;
}

} // namespace vgmplay::ui
