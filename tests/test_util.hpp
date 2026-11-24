// Wspolne narzedzia testow korpusu i golden: wczytanie pliku, SHA-256, hex.
// Nie jest to plik *_test.cpp, wiec nie trafia na liste zrodel; naglowek jest tylko inline.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace vgmplay::testutil {

// Progi wydajnosci: domyslnie luzne (x5), bo testy bywaja uruchamiane pod obciazeniem maszyny (rownolegle
// instancje, fuzz, CI). Zmienna srodowiskowa VGMPLAY_PERF_STRICT=1 przywraca ostre progi (pomiary na spokojnej maszynie).
inline bool perfStrict()
{
    const char* v = std::getenv("VGMPLAY_PERF_STRICT");
    return v && *v && *v != '0';
}
inline double perfLimit(double strictLimit) { return perfStrict() ? strictLimit : strictLimit * 5.0; }

inline std::filesystem::path dataDir() { return std::filesystem::path(VGMPLAY_TEST_DATA_DIR); }

inline std::vector<std::uint8_t> readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Minimalna implementacja SHA-256 (FIPS 180-4), strumieniowa.
class Sha256 {
public:
    Sha256() { reset(); }
    void reset()
    {
        h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        len_ = 0;
        fill_ = 0;
    }
    void update(const void* data, std::size_t n)
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        len_ += n;
        while (n > 0) {
            const std::size_t take = std::min<std::size_t>(64 - fill_, n);
            std::memcpy(buf_.data() + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ == 64) {
                block(buf_.data());
                fill_ = 0;
            }
        }
    }
    std::string finishHex()
    {
        const std::uint64_t bits = len_ * 8;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (fill_ != 56) update(&zero, 1);
        std::uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(lenBytes, 8);
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (std::uint32_t w : h_)
            for (int i = 3; i >= 0; --i) {
                const std::uint8_t b = static_cast<std::uint8_t>(w >> (8 * i));
                s += hex[b >> 4];
                s += hex[b & 15];
            }
        return s;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const std::uint8_t* p)
    {
        static const std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h_;
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const std::uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = S0 + mj;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) h_[i] += v[i];
    }

    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t fill_ = 0;
    std::uint64_t len_ = 0;
};

inline std::string sha256Hex(const void* data, std::size_t n)
{
    Sha256 s;
    s.update(data, n);
    return s.finishHex();
}

} // namespace vgmplay::testutil
