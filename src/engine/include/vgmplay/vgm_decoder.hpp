// vgmplay - engine: dekoder VGM (IDecoder) na libvgm (PlayerA + TapVgmPlayer).
// Naglowek nie zalezy od libvgm. Uzywany przez Engine, tools/vgmrender i testy.
//
// Zalozenia open(): bufor jest JUZ rozpakowany i zwalidowany (loader/format). Dekoder kopiuje go z
// dopelnieniem format::kParserPadding zer na czas wczytania do libvgm i zwalnia kopie po open().
// TODO(merge format/loader): dolozyc obrone w glab - format::parseVgmHeader +
// validateCommandStream w open(); TrackInfo::totalFrames/tags wypelniac wtedy z wlasnego skanu
// (do tego czasu: z naglowka libvgm, a Engine nadpisuje je z LoadedTrack).
#pragma once

#include <memory>

#include "vgmplay/decoder.hpp"

namespace vgmplay::engine {

[[nodiscard]] std::unique_ptr<IDecoder> makeVgmDecoder();

} // namespace vgmplay::engine
