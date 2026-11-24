// Stan MUTE/SOLO kanalow po stronie UI (lustro stanu silnika) - czysta logika, bez ImGui i bez
// silnika. AppController trzyma instancje, przeksztalca zmiany na komendy silnika
// (cmd::SetMute / cmd::SetSolo) i resetuje stan przy kazdym nowym utworze.
//
// Maski: kanal globalny -> (urzadzenie, indeks w urzadzeniu); indeksy [0, mainChannelCount) naleza do
// MuteMask::main (bit = indeks), reszta do MuteMask::linked (bit = indeks - mainChannelCount).
// Semantyka solo jak w silniku: jesli ktorekolwiek solo jest wlaczone, slyszalne sa tylko kanaly
// z solo (i bez MUTE).
#pragma once

#include "vgmplay/decoder.hpp"
#include "vgmplay/layout.hpp"

#include <cstdint>
#include <vector>

namespace vgmplay::ui {

class ChannelMix {
public:
    // Ustawia uklad i zeruje wszystkie MUTE/SOLO (nowy utwor).
    void setLayout(const ChannelLayout& layout);
    void clear();  // brak utworu

    int channelCount() const { return static_cast<int>(channels_.size()); }
    int deviceCount() const { return static_cast<int>(devices_.size()); }
    ChannelRef ref(int g) const;                // kanal poza zakresem -> {0, 0}

    bool muted(int g) const;
    bool solo(int g) const;
    bool anySolo() const { return soloCount_ > 0; }
    bool anyMuted() const { return mutedCount_ > 0; }
    // Slyszalny: bez MUTE i (brak solo albo ten kanal ma solo).
    bool audible(int g) const { return !muted(g) && (!anySolo() || solo(g)); }

    // Zwracaja false, gdy kanal jest poza zakresem (nic sie nie zmienia).
    bool toggleMute(int g);
    bool toggleSolo(int g);
    void muteAll();
    void unmuteAll();
    void clearSolo();

    // Maska wyciszen uzytkownika dla urzadzenia (jak w cmd::SetMute). Bity >= 32 sa ignorowane.
    MuteMask muteMask(DeviceId device) const;

private:
    struct Dev {
        std::uint16_t channelCount = 0;
        std::uint16_t mainCount = 0;
    };
    int index(int g) const { return g >= 0 && g < channelCount() ? g : -1; }

    std::vector<Dev> devices_;
    std::vector<ChannelRef> channels_;
    std::vector<std::uint8_t> mute_, solo_;
    int mutedCount_ = 0;
    int soloCount_ = 0;
};

} // namespace vgmplay::ui
