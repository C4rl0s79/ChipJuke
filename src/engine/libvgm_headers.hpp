// vgmplay - engine (wewnetrzne): jedyne miejsce dolaczania naglowkow libvgm.
// Naglowki pisane pod C++98/C90 - ostrzezenia /W4 z nich wylaczone (kod libvgm nie jest nasz).
#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stdtype.h>
#include <emu/EmuStructs.h>
#include <emu/SoundDevs.h>
#include <emu/SoundEmu.h>
#include <emu/EmuCores.h>
#include <utils/DataLoader.h>
#include <utils/MemoryLoader.h>
#include <player/playerbase.hpp>
#include <player/playera.hpp>
#include <player/vgmplayer.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
