// Test AppController na backendzie audio "null": ladowanie na workerze, Load do silnika, transport,
// scope zsynchronizowany z pozycja, bledy ladowania. Pliki: tests/data (syntetyczne).
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "app_controller.h"

using namespace vgmplay;
using namespace vgmplay::app;
using engine::TransportState;

namespace {

const std::filesystem::path kData = VGMPLAY_TEST_DATA_DIR;

template <class Pred>
bool pumpUntil(AppController& c, Pred p, int timeoutMs = 8000)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeoutMs)) {
        c.pump();
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_SUITE("app/controller")
{
    TEST_CASE("zaladowanie pliku, odtwarzanie, pauza, seek i oscyloskop")
    {
        ControllerOptions opt;
        opt.nullAudio = true;
        AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "valid" / "ym2612_melody_loop_v150_gd3jp.vgm");

        REQUIRE(pumpUntil(c, [&] { return c.view().hasTrack(); }));
        CHECK_FALSE(c.view().error.has_value());
        CHECK(c.view().info->layout.channels.size() > 0);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Playing &&
                                          c.view().transport.audibleFrame > 4800; }));
        CHECK(c.view().scope != nullptr);
        CHECK(c.view().scope->epoch() == c.view().transport.epoch);

        c.togglePlay();
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Paused; }));
        c.seekToFrame(48000);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.audibleFrame >= 48000 &&
                                          c.view().transport.audibleFrame < 48000 + 4800; }));
        c.seekBySeconds(5.0);
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.audibleFrame >= 48000 + 5 * c.view().sampleRate - 4800; }));
        c.stop();
        REQUIRE(pumpUntil(c, [&] { return c.view().transport.state == TransportState::Stopped; }));
    }

    TEST_CASE("blad ladowania: kod w widoku, poprzedni utwor zostaje; nastepny poprawny czysci blad")
    {
        ControllerOptions opt;
        opt.nullAudio = true;
        opt.autoplay = false;
        AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "valid" / "sn76489_scale_v100.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().hasTrack(); }));
        const auto first = c.view().track;

        c.loadFile(kData / "malformed" / "hdr_bad_signature.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().error.has_value(); }));
        CHECK(c.view().error->code == ErrorCode::BadSignature);
        CHECK(c.view().errorFile == "hdr_bad_signature.vgm");
        CHECK(c.view().track == first);
        CHECK_FALSE(c.view().loading);

        c.dismissError();
        c.pump();
        CHECK_FALSE(c.view().error.has_value());

        c.loadFile(kData / "malformed" / "no_such_file.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().error.has_value(); }));
        CHECK(c.view().error->code == ErrorCode::NotFound);
    }

    TEST_CASE("BlockAfterLoop: plik laduje sie, ostrzezenie widoczne, petla wylaczona")
    {
        ControllerOptions opt;
        opt.nullAudio = true;
        opt.autoplay = false;
        AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "malformed" / "blk_after_loop.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().hasTrack() || c.view().error.has_value(); }));
        REQUIRE(c.view().hasTrack());
        CHECK(c.view().track->stream.loopDisabled);
        bool found = false;
        for (const Warning& w : c.view().track->warnings) found |= w.code == WarningCode::BlockAfterLoop;
        CHECK(found);
    }

    TEST_CASE("ostatnie zadanie ladowania wygrywa")
    {
        ControllerOptions opt;
        opt.nullAudio = true;
        opt.autoplay = false;
        AppController c(opt);
        REQUIRE(c.start());
        c.loadFile(kData / "valid" / "sn76489_scale_v100.vgm");
        c.loadFile(kData / "valid" / "ym3812_melody_v151.vgm");
        REQUIRE(pumpUntil(c, [&] { return c.view().hasTrack() && !c.view().loading; }));
        CHECK(c.view().track->path.filename() == "ym3812_melody_v151.vgm");
    }
}
