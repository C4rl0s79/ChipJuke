// vgmplay - engine: dekoder VGM (IDecoder) na libvgm (PlayerA + TapVgmPlayer).
// Naglowek nie zalezy od libvgm. Uzywany przez Engine, tools/vgmrender i testy.
//
// open(): wolajacy podaje bufor rozpakowany (LoadedTrack::raw()), ale dekoder waliduje go SAM (obrona
// w glebi, ADR 0002 pkt 8): format::parseVgmHeader + format::validateCommandStream; bledy zwraca bez
// dotykania libvgm. Wypelnia z wlasnego skanu: TrackInfo::totalFrames/introFrames/loopFrames, tags (GD3)
// i warnings. Dekoder kopiuje bufor z dopelnieniem format::kParserPadding zer na czas wczytania do
// libvgm i zwalnia kopie po open(). Gdy skan zglosi loopDisabled (blok PCM w/za punktem petli), pola
// petli 0x1C/0x20 sa zerowane w tej kopii (jak w loaderze).
#pragma once

#include <memory>

#include "vgmplay/decoder.hpp"

namespace vgmplay::engine {

[[nodiscard]] std::unique_ptr<IDecoder> makeVgmDecoder();

} // namespace vgmplay::engine
