#!/usr/bin/env python3
"""build_corpus - buduje korpus testowy vgmplay w tests/data/ (valid/, malformed/, manifest.json).

Uruchom:  python -I tools/vgmgen/build_corpus.py [--out KATALOG] [--allow-missing-names]
Korpus jest deterministyczny (te same bajty przy kazdym uruchomieniu na tej samej wersji zlib).
Nazwy kodow w manifescie sa weryfikowane z include/vgmplay/{core,chips}.hpp.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import sys
from pathlib import Path

# W trybie -I katalog skryptu NIE trafia do sys.path - dodajemy go jawnie.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import vgmgen as g  # noqa: E402
from vgmgen import (CLK_AY, CLK_OPL2, CLK_OPL3, CLK_SN, CLK_YM2151, CLK_YM2413, CLK_YM2612,  # noqa: E402
                    MELODY, SCALE, Gd3, Stream, Vgm)

REPO = Path(__file__).resolve().parents[2]
MANIFEST_SCHEMA = 1
MAX_CORPUS_BYTES = 1 << 20

ENTRIES: list[dict] = []


# ---------------------------------------------------------------------------
# Rejestracja plikow
# ---------------------------------------------------------------------------
def ok(warnings=(), alternatives=None):
    e = {"result": "ok", "warnings": list(warnings)}
    if alternatives:
        e["alternatives"] = alternatives
    return e


def err(code, alternatives=None, warnings=()):
    e = {"result": "error", "error": code, "warnings": list(warnings)}
    if alternatives:
        e["alternatives"] = alternatives
    return e


def alt_err(code):
    return {"result": "error", "error": code}


def alt_ok(*warnings):
    return {"result": "ok", "warnings": list(warnings)}


def add(kind, name, data, desc, expect, *, meta=None, raw=None, limits=None, tags=(),
        hazard=None, note=None, header_total=None, gd3_expect=None, scan=True):
    path = "%s/%s" % (kind, name)
    entry = {
        "path": path,
        "kind": kind,
        "description": desc,
        "sizeBytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    if name.endswith(".vgz"):
        entry["gzip"] = True
        if raw is not None:
            entry["rawSizeBytes"] = len(raw)
            entry["rawSha256"] = hashlib.sha256(raw).hexdigest()
    if hazard:
        entry["hazard"] = hazard
    if tags:
        entry["tags"] = list(tags)
    if limits:
        entry["limits"] = dict(limits)
    if note:
        entry["note"] = note
    entry["expect"] = expect
    if meta is not None:
        rawdata = raw if raw is not None else data
        header = {k: meta[k] for k in ("versionBcd", "loopSamples", "hasLoop", "rate", "dataOffset",
                                       "dataEnd", "gd3Offset", "fileSize", "chips")}
        header["totalSamples"] = header_total if header_total is not None else meta["totalSamples"]
        entry["header"] = dict(sorted(header.items()))
        if scan:
            data_ofs = meta["dataOffset"]
            loop_abs = 0
            if meta["hasLoop"] or meta["loopStartSample"]:
                loop_abs = 0x1C + int.from_bytes(rawdata[0x1C:0x20], "little")
            sc = g.scan_stream(rawdata, data_ofs, meta["dataEnd"], loop_abs)
            assert sc["error"] is None, (path, sc["error"])
            assert sc["totalSamples"] == meta["totalSamples"], (path, sc, meta["totalSamples"])
            if loop_abs:
                assert sc["loopStartSample"] == meta["loopStartSample"], (path, sc)
            sc.pop("error")
            entry["stream"] = sc
        if "gd3" in meta:
            entry["gd3"] = meta["gd3"]
        else:
            entry["gd3"] = {"present": False}
    if gd3_expect is not None:
        entry["gd3"] = gd3_expect
    ENTRIES.append({"entry": entry, "data": data})
    return entry


# ---------------------------------------------------------------------------
# Pomocnicze generatory
# ---------------------------------------------------------------------------
GD3_FULL = Gd3(
    track_en="Ode to Joy (test melody)", track_jp="歓喜の歌 (テスト旋律)",
    game_en="vgmplay synthetic corpus", game_jp="vgmplay 合成コーパス",
    system_en="Sega Mega Drive", system_jp="メガドライブ",
    author_en="Ludwig van Beethoven (arr. vgmgen)", author_jp="ベートーヴェン",
    date="2026/11", converter="vgmgen.py", notes="Synthetic test file. Public domain melody.")
GD3_EN = Gd3(track_en="Scale (test)", game_en="vgmplay synthetic corpus",
             system_en="Sega Master System", author_en="vgmgen", date="2026/11",
             converter="vgmgen.py")


def finish(s: Stream, loop_at_start_of=None):
    s.end()
    return s


def sn_file(version, melody, *, gd3=None, loop=False, rate=0, **kw):
    v = Vgm(version, {"SN76496": CLK_SN}, rate=rate, **kw)
    s = Stream()
    voice = g.SnVoice(s)
    if loop:
        s.mark_loop()
    g.play(s, voice, melody)
    s.end()
    return v.build(s, gd3)


def base(version=0x151, *, loop=False, gd3=None, melody=SCALE[:4]):
    """Mala poprawna baza do patchowania."""
    return sn_file(version, melody, gd3=gd3, loop=loop)


def stream_file(version, build_fn, *, clocks=None, dual=(), gd3=None, end=True, **kw):
    v = Vgm(version, clocks if clocks is not None else {"SN76496": CLK_SN, "YM2612": CLK_YM2612},
            dual, **kw)
    s = Stream()
    build_fn(s)
    if end:
        s.end()
    return v.build(s, gd3)


def fm_header_setup(s):
    g.Ym2612Voice(s).setup()
    s.wait(100)


# ---------------------------------------------------------------------------
# valid/
# ---------------------------------------------------------------------------
def build_valid():
    K = "valid"

    # 1. SN76489 gama, v1.00, bez GD3 i bez petli
    d, m = sn_file(0x100, SCALE)
    add(K, "sn76489_scale_v100.vgm", d,
        "v1.00, SN76489, gama C-dur, bez GD3, bez petli; opoznienia 0x61/0x7n.", ok(),
        meta=m, tags=["golden", "minimal"])

    # 2. v1.01 + rate 60 + synchronizacja klatkami 0x62, GD3 tylko EN
    v = Vgm(0x101, {"SN76496": CLK_SN}, rate=60)
    s = Stream()
    voice = g.SnVoice(s)
    voice.setup()
    for note, beats in SCALE:
        voice.on(note)
        s.wait_frames(12 * int(beats))
        voice.off()
        s.wait_frames(1)
    s.wait_frames(5)
    s.end()
    d, m = v.build(s, GD3_EN)
    add(K, "sn76489_frames_v101_gd3en.vgm", d,
        "v1.01, rate=60, opoznienia wylacznie 0x62 (ramki 60 Hz), GD3 tylko pola EN (JP puste).",
        ok(), meta=m, tags=["golden", "gd3"])

    # 3. v1.10, petla, Game Gear stereo, pola SN, GD3 EN+JP
    v = Vgm(0x110, {"SN76496": CLK_SN}, rate=60, sn_fb=0x0009, sn_sw=16)
    s = Stream()
    s.gg_stereo(0xFF)
    voice = g.SnVoice(s)
    g.play(s, voice, SCALE)                 # wstep
    s.mark_loop()
    g.play(s, voice, MELODY)                # petla
    s.end()
    d, m = v.build(s, GD3_FULL)
    add(K, "sn76489_loop_gg_v110_gd3jp.vgm", d,
        "v1.10, petla, komenda 0x4F (GG stereo), feedback/shift SN, GD3 EN+JP (japonskie pola).",
        ok(), meta=m, tags=["golden", "loop", "gd3", "jp"])

    # 4. YM2612 v1.50, petla, GD3 JP
    v = Vgm(0x150, {"YM2612": CLK_YM2612})
    s = Stream()
    voice = g.Ym2612Voice(s)
    voice.setup()
    s.wait(2205)
    s.mark_loop()
    g.play(s, voice, MELODY)
    s.end()
    d, m = v.build(s, GD3_FULL)
    add(K, "ym2612_melody_loop_v150_gd3jp.vgm", d,
        "v1.50 (dataOfs wzgledne), YM2612 4-op FM, melodia z petla, GD3 EN+JP.", ok(),
        meta=m, tags=["golden", "loop", "gd3", "jp"])

    # 5. Mega Drive: YM2612 + SN76489, v1.51, gzip
    v = Vgm(0x151, {"SN76496": CLK_SN, "YM2612": CLK_YM2612})
    s = Stream()
    fm = g.Ym2612Voice(s)
    sn = g.SnVoice(s)
    sn.shift = 12
    g.play(s, [fm, sn], MELODY)
    s.end()
    d, m = v.build(s)
    gz = g.gzip_bytes(d)
    add(K, "megadrive_ym2612_sn76489_v151.vgz", gz,
        "v1.51, YM2612 + SN76489 razem, bez GD3, bez petli, skompresowany gzip (mtime=0).",
        ok(), meta=m, raw=d, tags=["golden", "gzip", "multichip"])

    # 6. SN gzip v1.50 z GD3
    d, m = sn_file(0x150, SCALE, gd3=GD3_EN)
    add(K, "sn76489_scale_v150_gd3en.vgz", g.gzip_bytes(d),
        "v1.50, SN76489, gama, GD3 EN, .vgz.", ok(), meta=m, raw=d, tags=["gzip", "gd3"])

    # 7. AY8910 v1.51, petla
    v = Vgm(0x151, {"AY8910": CLK_AY}, ay_type=0x00)
    s = Stream()
    voice = g.AyVoice(s)
    voice.setup()
    s.mark_loop()
    g.play(s, voice, MELODY)
    s.end()
    d, m = v.build(s, GD3_EN)
    add(K, "ay8910_melody_loop_v151_gd3en.vgm", d,
        "v1.51 (naglowek 0x80), AY8910 (typ 0), melodia z petla, GD3 EN.", ok(), meta=m,
        tags=["golden", "loop", "gd3"])

    # 8. YM2151 v1.61, petla
    v = Vgm(0x161, {"YM2151": CLK_YM2151})
    s = Stream()
    voice = g.Ym2151Voice(s)
    voice.setup()
    s.wait(441)
    s.mark_loop()
    g.play(s, voice, MELODY)
    s.end()
    d, m = v.build(s, GD3_FULL)
    add(K, "ym2151_melody_loop_v161_gd3jp.vgm", d,
        "v1.61, YM2151 (OPM), melodia z petla, GD3 EN+JP.", ok(), meta=m,
        tags=["golden", "loop", "gd3", "jp"])

    # 9. OPL2 v1.51, bez GD3, bez petli
    v = Vgm(0x151, {"YM3812": CLK_OPL2})
    s = Stream()
    g.play(s, g.Opl2Voice(s), MELODY)
    s.end()
    d, m = v.build(s)
    add(K, "ym3812_melody_v151.vgm", d, "v1.51, YM3812 (OPL2), melodia, bez GD3, bez petli.",
        ok(), meta=m, tags=["golden"])

    # 10. OPL3 v1.70, volume modifier, petla
    v = Vgm(0x170, {"YMF262": CLK_OPL3}, volume_mod=0x10, loop_mod=0x10)
    s = Stream()
    voice = g.Opl3Voice(s)
    voice.setup()
    s.mark_loop()
    g.play(s, voice, MELODY)
    s.end()
    d, m = v.build(s, GD3_EN)
    add(K, "ymf262_melody_loop_v170.vgm", d,
        "v1.70 (naglowek 0xC0), YMF262 (OPL3), bajty 0x7C/0x7F ustawione, petla, GD3 EN.", ok(),
        meta=m, tags=["golden", "loop", "gd3"])

    # 11. OPLL v1.71 (naglowek 0x100), petla
    v = Vgm(0x171, {"YM2413": CLK_YM2413})
    s = Stream()
    voice = g.OpllVoice(s)
    voice.setup()
    s.mark_loop()
    g.play(s, voice, MELODY)
    s.end()
    d, m = v.build(s, GD3_FULL)
    add(K, "ym2413_melody_loop_v171_gd3jp.vgm", d,
        "v1.71 (naglowek 0x100), YM2413 (OPLL), petla, GD3 EN+JP.", ok(), meta=m,
        tags=["golden", "loop", "gd3", "jp"])

    # 12. dual chip: 2x SN76489, 2x YM2612
    v = Vgm(0x151, {"SN76496": CLK_SN, "YM2612": CLK_YM2612}, dual={"SN76496", "YM2612"})
    s = Stream()
    voices = [g.Ym2612Voice(s, chip=0), g.Ym2612Voice(s, chip=1),
              g.SnVoice(s, chip=0), g.SnVoice(s, chip=1)]
    voices[1].shift = 7
    voices[2].shift = 12
    voices[3].shift = 19
    g.play(s, voices, MELODY)
    s.end()
    d, m = v.build(s, GD3_EN)
    add(K, "dual_chip_ym2612_sn76489_v151.vgm", d,
        "v1.51, bit 30 zegara: 2x YM2612 (0x52/0xA2) i 2x SN76489 (0x50/0x30), kwinta w gore.",
        ok(), meta=m, tags=["golden", "dual", "gd3"])

    # 13. PCM w bloku 0x67 + 0x8n
    pcm = g.sine_wave_8bit(4410)
    v = Vgm(0x150, {"YM2612": CLK_YM2612})
    s = Stream()
    s.data_block(0x00, pcm)
    s.ym2612(0, 0x2B, 0x80)
    s.pcm_seek(0)
    for _ in range(len(pcm)):
        s.ym2612_pcm_write_wait(5)
    s.wait(441)
    s.end()
    d, m = v.build(s)
    add(K, "ym2612_dac_block_v150.vgm", d,
        "v1.50, blok 0x67 typ 0x00 (4410 B PCM), DAC przez 0xE0 + 0x8n (8820 Hz, sinus 441 Hz).",
        ok(), meta=m, tags=["golden", "pcm"])

    # 14. DAC stream 0x90-0x95
    pcm = g.sine_wave_8bit(2000)
    v = Vgm(0x151, {"YM2612": CLK_YM2612})
    s = Stream()
    s.data_block(0x00, pcm)
    s.ym2612(0, 0x2B, 0x80)
    s.dac_setup(0, 0x02, 0x00, 0x2A)
    s.dac_data(0, 0x00, 1, 0)
    s.dac_freq(0, 8820)
    s.dac_start(0, 0, 0x01, 2000)
    s.wait(10000)
    s.dac_stop(0)
    s.wait(441)
    s.dac_fast(0, 0, 0x00)
    s.wait(10000)
    s.dac_stop(0)
    s.wait(441)
    s.end()
    d, m = v.build(s)
    add(K, "ym2612_dac_stream_v151.vgm", d,
        "v1.51, DAC stream: 0x90 (setup), 0x91 (data), 0x92 (czestotliwosc), 0x93 (start), 0x94 "
        "(stop), 0x95 (fast call); blok PCM 2000 B.", ok(), meta=m, tags=["golden", "pcm", "dacstream"])

    # 15. blok skompresowany + tabela
    table = bytes(k * 17 for k in range(16))
    idx = [min(15, max(0, round((math.sin(2 * math.pi * i / 20) * 0.5 + 0.5) * 15)))
           for i in range(2000)]
    v = Vgm(0x151, {"YM2612": CLK_YM2612})
    s = Stream()
    s.data_block(0x7F, g.table_block_payload(0, 2, 8, 4, 16, table))
    s.data_block(0x40, g.cmp_block_payload(0, 2000, 8, 4, 2, 0, g.pack_bits(idx, 4)))
    s.ym2612(0, 0x2B, 0x80)
    s.dac_setup(0, 0x02, 0x00, 0x2A)
    s.dac_data(0, 0x00, 1, 0)
    s.dac_freq(0, 8820)
    s.dac_start(0, 0, 0x01, 2000)
    s.wait(10000)
    s.dac_stop(0)
    s.wait(441)
    s.end()
    d, m = v.build(s)
    add(K, "ym2612_dac_compressed_v151.vgm", d,
        "v1.51, blok 0x7F (tabela 16 wartosci) + blok 0x40 (bit-packing 4->8 bitow, tryb tabelowy, "
        "2000 B po dekompresji), DAC stream.", ok(), meta=m, tags=["pcm", "dacstream", "compressed"],
        note="Kolejnosc bitow w paczce (MSB first) to zalozenie generatora - golden audio tylko "
             "jako regresja, nie jako wzorzec poprawnosci.")

    # 16. petla z loopSamples == 0 => brak petli
    d, m = sn_file(0x150, SCALE, loop=True)
    d = g.patch(d, 0x20, 0)
    m = dict(m, loopSamples=0, hasLoop=False)
    add(K, "sn76489_loop_zero_samples_v150.vgm", d,
        "v1.50, loopOfs ustawiony, ale loopSamples=0 => brak petli (hasLoop()==false).", ok(),
        meta=m, tags=["loop"], note="stream.loopStartSample jest sensowny tylko gdy hasLoop().")

    # 17. naglowek klamie co do dlugosci
    v = Vgm(0x150, {"SN76496": CLK_SN})
    s = Stream()
    g.play(s, g.SnVoice(s), SCALE)
    s.end()
    d, m = v.build(s, total_override=0x00100000)
    add(K, "sn76489_header_total_lies_v150.vgm", d,
        "v1.50, totalSamples w naglowku (1048576) ~4x wieksze niz prawdziwa dlugosc; "
        "prawdziwa = suma opoznien.", ok(), meta=m, header_total=0x00100000, tags=["length"],
        note="Oczekiwana dlugosc to stream.totalSamples; header.totalSamples jest NIEZAUFANE.")

    # 18. naglowek rozszerzony (v1.70)
    v = Vgm(0x170, {"SN76496": CLK_SN}, extra_volumes=[(0x00, 0x00, 0x00C0)])
    s = Stream()
    g.play(s, g.SnVoice(s), SCALE)
    s.end()
    d, m = v.build(s)
    add(K, "sn76489_extra_header_v170.vgm", d,
        "v1.70, naglowek rozszerzony (0xBC) z lista glosnosci ukladow (SN76489: 0x00C0), dane za nim.",
        ok(), meta=m, tags=["extraheader"],
        note="Struktura naglowka rozszerzonego i ID ukladu (0 = SN76489) wg pamieci o specyfikacji "
             "1.70 - NIE zweryfikowane z oryginalnym tekstem; chips[].volume do potwierdzenia.")


# ---------------------------------------------------------------------------
# malformed/
# ---------------------------------------------------------------------------
def build_malformed():
    K = "malformed"
    H = "libvgm-parser-hazards.md"

    # --- naglowek
    d, _ = base(0x150)
    add(K, "hdr_short_0x3f.vgm", d[:0x3F], "Plik 0x3F B (krocej niz naglowek 0x40), poprawna sygnatura.",
        err("Truncated", [alt_err("BadHeader")]), hazard="H1")
    add(K, "hdr_short_0x20.vgm", d[:0x20], "Plik 0x20 B (sygnatura + polowa naglowka).",
        err("Truncated", [alt_err("BadHeader")]), hazard="H1")
    add(K, "hdr_empty.vgm", b"", "Pusty plik (0 B).",
        err("Truncated", [alt_err("BadSignature")]), hazard="ADR8.3")
    add(K, "hdr_bad_signature.vgm", g.patch(d, 0, int.from_bytes(b"Vgm!", "little")),
        "Sygnatura 'Vgm!' zamiast 'Vgm '.", err("BadSignature"), hazard="ADR8.3")
    d151, _ = base(0x151)
    add(K, "hdr_v151_shorter_than_dataofs.vgm", d151[:0x70],
        "v1.51: dataOfs wskazuje 0x80, plik ma tylko 0x70 B.",
        err("BadHeader", [alt_err("Truncated")]), hazard="H1/ADR8.3")
    add(K, "hdr_dataofs_beyond_file_v150.vgm", g.patch(d, 0x34, 0x10000),
        "v1.50: dataOfs (0x34 + 0x10000) poza plikiem.", err("BadHeader"), hazard="H1/ADR8.4")
    add(K, "hdr_dataofs_overflow_u32_v150.vgm", g.patch(d, 0x34, 0xFFFFFFF0),
        "v1.50: dataOfs rel=0xFFFFFFF0 - w UINT32 0x34+rel zawija sie do 0x24; w UINT64 poza plikiem.",
        err("BadHeader"), hazard="H1/ADR8.4")
    add(K, "hdr_version_unknown_0180.vgm", g.patch(d, 8, 0x0180),
        "Wersja 1.80 (nowsza niz znana 1.71), reszta poprawna.", ok(["UnknownVersion"]),
        hazard="ADR8.3", meta=None)
    add(K, "hdr_version_unsupported_0099.vgm", g.patch(d, 8, 0x0099),
        "Wersja 0.99 (ponizej 1.00).", err("UnsupportedVersion"), hazard="ADR8.3",
        note="Granice 'sensownej' wersji z ADR 8.3: 0x100..0x2FF; wyzsze = ostrzezenie.")

    # --- eof
    add(K, "hdr_eof_beyond_file.vgm", g.patch(d, 4, 0x100000),
        "eofOfs wskazuje daleko za koniec pliku.", ok(["EofOffsetMismatch"]), hazard="H1/ADR8.5",
        note="Naprawialne: loader przycina do rozmiaru pliku.")
    add(K, "hdr_eof_overflow_u32.vgm", g.patch(d, 4, 0xFFFFFFFF),
        "eofOfs rel=0xFFFFFFFF (zawija sie w UINT32).", ok(["EofOffsetMismatch"]),
        hazard="H1/ADR8.5")
    add(K, "hdr_eof_before_dataofs.vgm", g.patch(d, 4, 0x1C),
        "eofOfs = 0x20, czyli przed dataOfs.", err("BadHeader"), hazard="ADR8.5")

    # --- gd3Ofs
    add(K, "hdr_gd3ofs_beyond_file.vgm", g.patch(base(0x150, gd3=GD3_EN)[0], 0x14, 0x10000),
        "gd3Ofs poza plikiem.", ok(["Gd3Invalid"], [alt_err("BadHeader")]), hazard="H4/ADR8.6",
        note="GD3 jest niekrytyczny; BadHeader dopuszczalny jako alternatywa.")
    add(K, "hdr_gd3ofs_overflow_u32.vgm", g.patch(base(0x150, gd3=GD3_EN)[0], 0x14, 0xFFFFFFF0),
        "gd3Ofs rel=0xFFFFFFF0 (0x14+rel zawija sie w UINT32 do 0x4).",
        ok(["Gd3Invalid"], [alt_err("BadHeader")]), hazard="H4/ADR8.6")

    # --- loopOfs
    dl, ml = base(0x150, loop=True)
    add(K, "hdr_loopofs_beyond_file.vgm", g.patch(dl, 0x1C, 0x10000),
        "loopOfs poza plikiem (loopSamples != 0).", err("BadHeader"), hazard="ADR8.6",
        note="Alternatywnie loader moze zignorowac petle z ostrzezeniem - patrz README (niejednoznaczne).")
    ENTRIES[-1]["entry"]["expect"]["alternatives"] = [alt_ok("Other")]
    add(K, "hdr_loopofs_overflow_u32.vgm", g.patch(dl, 0x1C, 0xFFFFFFF0),
        "loopOfs rel=0xFFFFFFF0 (zawija sie w UINT32 do 0x0C).", err("BadHeader"),
        hazard="H1/ADR8.6")
    ENTRIES[-1]["entry"]["expect"]["alternatives"] = [alt_ok("Other")]
    dl2, _ = base(0x151, loop=True)
    add(K, "hdr_loopofs_before_dataofs.vgm", g.patch(dl2, 0x1C, 0x40 - 0x1C),
        "v1.51: loopOfs = 0x40 (wewnatrz naglowka), dataOfs = 0x80.", err("BadHeader"),
        hazard="ADR8.6")
    ENTRIES[-1]["entry"]["expect"]["alternatives"] = [alt_ok("Other")]

    # --- edge: bez ukladow i bez komend
    v = Vgm(0x100, {})
    s = Stream()
    s.end()
    d0, m0 = v.build(s)
    add(K, "edge_no_chips_only_end_v100.vgm", d0,
        "v1.00, brak ukladow w naglowku, strumien = samo 0x66 (dlugosc 0).",
        ok(["Other"], [alt_err("BadHeader")]), meta=m0, hazard="edge",
        note="Niejednoznaczne: implementacja moze uznac plik bez ukladow za bledny. "
             "Implementacja (E1) przyjmuje plik z ostrzezeniem Other (\"header lists no sound chips\").")

    # --- gzip
    good, _ = base(0x150, gd3=GD3_EN, melody=SCALE)
    gz = g.gzip_bytes(good)
    add(K, "gz_truncated.vgz", gz[:int(len(gz) * 0.6)], "gzip ucety w polowie strumienia deflate.",
        err("DecompressionFailed"), hazard="H5/ADR8.2", raw=None)
    add(K, "gz_truncated_trailer.vgz", gz[:-5], "gzip bez konca trailera (brak pelnego CRC/ISIZE).",
        err("DecompressionFailed"), hazard="H5/ADR8.2")
    add(K, "gz_header_only.vgz", gz[:10], "Tylko 10-bajtowy naglowek gzip.",
        err("DecompressionFailed"), hazard="H5/ADR8.2")
    add(K, "gz_bad_crc.vgz", g.gzip_bytes(good, crc=0xDEADBEEF), "Poprawny deflate, zly CRC32.",
        err("DecompressionFailed"), hazard="H5/ADR8.2")
    add(K, "gz_isize_huge.vgz", g.gzip_bytes(good, isize=0xFFFFFFFF),
        "ISIZE=0xFFFFFFFF przy kilkuset bajtach danych (libvgm alokowalby 4 GiB).",
        err("DecompressionFailed", [alt_ok()]), hazard="H5/ADR8.2",
        note="Wynik zalezy od tego, czy implementacja sprawdza ISIZE (zlib w trybie gzip: tak => "
             "DecompressionFailed, to jest wynik podstawowy); wymog: brak alokacji ~4 GiB, wynik w obu przypadkach dopuszczalny.")
    add(K, "gz_isize_small.vgz", g.gzip_bytes(good, isize=4),
        "ISIZE=4 (mniejsze niz prawdziwa dlugosc; libvgm cicho przycinalby dane).",
        err("DecompressionFailed", [alt_ok()]), hazard="H5/ADR8.2",
        note="Jw.; nie wolno przycinac danych do ISIZE bez bledu.")
    add(K, "gz_nested.vgz", g.gzip_bytes(gz), "gzip w gzip: po rozpakowaniu znowu 1F 8B.",
        err("BadSignature"), hazard="ADR8.2")

    # bomba (domyslne limity): ~66 MiB zer po rozpakowaniu
    n = 66 << 20
    vb = Vgm(0x150, {"SN76496": CLK_SN})
    sb = Stream()
    sb.end()
    db, _ = vb.build(sb)
    db = bytearray(db) + bytes(n - len(db))
    db[4:8] = (n - 4).to_bytes(4, "little")
    bomb = g.gzip_bytes(bytes(db))
    del db
    add(K, "gz_bomb_66mib.vgz", bomb,
        "Bomba gzip: ~%d KiB na dysku, po rozpakowaniu 66 MiB (> domyslne 64 MiB)." % (len(bomb) // 1024),
        err("TooLarge"), hazard="H5/ADR8.1", note="Rozpakowanie musi sie zatrzymac na limicie, nie na koncu.")
    n2 = 2 << 20
    db2 = bytearray(base(0x150)[0]) + bytes(n2 - len(base(0x150)[0]))
    db2[4:8] = (n2 - 4).to_bytes(4, "little")
    add(K, "gz_bomb_2mib_small_limit.vgz", g.gzip_bytes(bytes(db2)),
        "Mala bomba: 2 MiB po rozpakowaniu; test z Limits::maxDecompressedBytes = 1 MiB.",
        err("TooLarge"), hazard="H5/ADR8.1", limits={"maxDecompressedBytes": 1 << 20})

    # --- GD3
    def with_gd3(chunk, name, desc, expect, **kw):
        v = Vgm(0x150, {"SN76496": CLK_SN})
        s = Stream()
        g.play(s, g.SnVoice(s), SCALE[:3], tail=441)
        s.end()
        data, meta = v.build(s, chunk)
        return add(K, name, data, desc, expect, meta=meta, **kw)

    valid_fields = GD3_EN.data()
    with_gd3(g.gd3_raw_chunk(b"".join(c.encode("utf-16-le") for c in "NoTerminatorsAtAll!!!!")),
             "gd3_no_terminators.vgm", "GD3: dane bez zadnego terminatora U+0000 (jedno pole bez konca).",
             ok(["Gd3Invalid"], [alt_ok()]), hazard="H4/ADR8.12",
             gd3_expect={"present": False},
             note="Alternatywa: tolerancyjny parser dekoduje jedno pole bez terminatora (implementacja E1).")
    with_gd3(g.gd3_raw_chunk("Only\0Three\0Fields\0".encode("utf-16-le")),
             "gd3_too_few_fields.vgm", "GD3: tylko 3 z 11 pol.", ok(["Gd3Invalid"]),
             hazard="H4/ADR8.12", gd3_expect={"present": False},
             note="Alternatywa: tolerancyjny parser uzupelnia puste pola - wtedy brak ostrzezenia.")
    ENTRIES[-1]["entry"]["expect"]["alternatives"] = [alt_ok()]
    with_gd3(g.gd3_raw_chunk(valid_fields[:-2]),
             "gd3_last_field_unterminated.vgm", "GD3: ostatnie (11.) pole bez terminatora.",
             ok(["Gd3Invalid"], [alt_ok()]), hazard="H4/ADR8.12", gd3_expect={"present": False},
             note="Niejednoznaczne: parser moze zaakceptowac ostatnie pole bez terminatora.")
    with_gd3(g.gd3_raw_chunk(valid_fields + b"\x41"),
             "gd3_odd_length.vgm", "GD3: nieparzysta deklarowana dlugosc danych (reszta 1 B).",
             ok(["Gd3Invalid"], [alt_ok()]), hazard="H4",
             gd3_expect={"present": False},
             note="Niejednoznaczne: parser moze zignorowac nadmiarowy bajt.")
    with_gd3(g.gd3_raw_chunk(valid_fields, length=0xFFFFFFF0),
             "gd3_huge_declared_length.vgm", "GD3: deklarowana dlugosc 0xFFFFFFF0 (dane maja ~100 B).",
             ok(["Gd3Invalid"], [alt_ok()]), hazard="H4",
             gd3_expect={"present": False},
             note="Wymog: brak odczytu/alokacji poza plikiem; dopuszczalne tez przyciecie do konca pliku.")
    with_gd3(g.gd3_raw_chunk(valid_fields, sig=b"Gd4 "),
             "gd3_bad_signature.vgm", "GD3: sygnatura 'Gd4 '.", ok(["Gd3Invalid"]),
             hazard="H4", gd3_expect={"present": False})
    with_gd3(g.gd3_raw_chunk(valid_fields, version=0x200),
             "gd3_bad_version.vgm", "GD3: wersja 0x00000200 (poza 0x100..0x1FF).",
             ok(["Gd3Invalid"]), hazard="H4", gd3_expect={"present": False})

    # niesparowane surogaty / znaki sterujace
    def fields_with(first_units):
        parts = [g.utf16_units(first_units) + b"\0\0"]
        for _ in range(10):
            parts.append("x".encode("utf-16-le") + b"\0\0")
        return b"".join(parts)

    with_gd3(g.gd3_raw_chunk(fields_with([0x41, 0xD800, 0x42, 0xDC00, 0xD83D, 0x43])),
             "gd3_unpaired_surrogates.vgm",
             "GD3: pole utworu zawiera niesparowane surogaty (U+D800, U+DC00, U+D83D na koncu).",
             ok(["TagSanitized"]), hazard="H4/ADR8.12",
             gd3_expect={"present": True, "trackEnContains": ["A", "B", "C"],
                         "mustBeValidUtf8": True, "gameEn": "x", "notes": "x"})
    with_gd3(g.gd3_raw_chunk(fields_with([0x41, 0x01, 0x07, 0x1B, 0x7F, 0x42])),
             "gd3_control_chars.vgm", "GD3: znaki sterujace (U+0001, U+0007, U+001B, U+007F) w polu.",
             ok(["TagSanitized"]), hazard="H4/ADR8.12",
             gd3_expect={"present": True, "trackEnContains": ["A", "B"],
                         "mustBeValidUtf8": True, "mustNotContainControl": True,
                         "gameEn": "x", "notes": "x"})
    long_track = Gd3(track_en="A" * 5000, game_en="g", system_en="s", author_en="a")
    with_gd3(long_track, "gd3_long_field.vgm",
             "GD3: pole utworu 5000 B UTF-8 (> maxTagFieldBytes=4096).", ok(["TagTruncated"]),
             hazard="H4/ADR8.12",
             gd3_expect={"present": True, "trackEnMaxBytes": 4096, "trackEnPrefix": "AAAA",
                         "gameEn": "g", "systemEn": "s", "authorEn": "a"})

    # --- bloki danych 0x67
    def blk(fn, name, desc, expect, version=0x151, **kw):
        data, _meta = stream_file(version, fn, clocks={"SN76496": CLK_SN, "YM2612": CLK_YM2612})
        return add(K, name, data, desc, expect, **kw)

    def pre(s):
        g.Ym2612Voice(s).setup()
        s.wait(100)

    blk(lambda s: (pre(s), s.data_block_raw(0x00, 0x00100000, b"\x01" * 16)),
        "blk_longer_than_file.vgm", "Blok 0x67: dblkLen=1 MiB, w pliku tylko 16 B.",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block_raw(0x00, 0xFFFFFFFF, b"\x01" * 16)),
        "blk_len_2gib_bit31.vgm", "Blok 0x67: dblkLen=0xFFFFFFFF (po masce bitu 31: 2 GiB-1).",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block_raw(0x00, 0x80000010, b"\x01" * 16), s.wait(10)),
        "edge_blk_bit31_masked_fits.vgm",
        "Blok 0x67: dblkLen=0x80000010 - po zamaskowaniu bitu 31 = 16 B i miesci sie w pliku.",
        ok(alternatives=[alt_err("BadDataBlock")]), hazard="ADR8.9",
        note="ADR 8.9: dlugosc liczona po zamaskowaniu bitu 31.")
    blk(lambda s: (pre(s), s.data_block(0x80, g.u32(0xF0000000) + g.u32(0)), s.wait(10)),
        "blk_rom_memsize_over_limit.vgm",
        "Blok ROM 0x80: memSize=0xF0000000 (3.75 GiB) przy dblkLen=8 (> maxSingleBlockBytes).",
        err("LimitExceeded", [alt_err("BadDataBlock")]), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s),
                   [s.data_block(0x80 + i, g.u32(0x04000000) + g.u32(0)) for i in range(5)],
                   s.wait(10)),
        "blk_rom_budget_sum.vgm",
        "5 blokow ROM (typy 0x80..0x84, rozne uklady) po memSize=64 MiB (kazdy <= limit pojedynczy, "
        "suma 320 MiB > budzet 256 MiB).",
        err("LimitExceeded", [alt_err("BadDataBlock")]), hazard="H3/ADR8.9",
        note="Rozne typy ROM => osobne pamieci; budzet liczy SUME. Wersja z jednym typem: blk_rom_same_slot_not_summed.")
    blk(lambda s: (pre(s),
                   [s.data_block(0x80, g.u32(0x04000000) + g.u32(0)) for _ in range(5)],
                   s.wait(10)),
        "blk_rom_same_slot_not_summed.vgm",
        "5 blokow ROM 0x80 (ten sam typ i uklad) po memSize=64 MiB: rdzen robi realloc do memSize, "
        "wiec pamiec = max (64 MiB), nie suma.",
        ok(alternatives=[alt_err("LimitExceeded")]), hazard="H3/ADR8.9",
        note="Implementacja liczy najwiekszy memSize na (typ, uklad); ADR 8.9 mowi 'suma memSize' - "
             "kontrakt do doprecyzowania (alternatywa = naiwna suma).")

    cmp_ok_tail = b"\x00" * 8
    blk(lambda s: (pre(s), s.data_block(0x40, g.cmp_block_payload(0, 16, 8, 0, 0, 0, cmp_ok_tail))),
        "blk_cmp_bitscmp_zero.vgm", "Blok 0x40: bitsCmp=0 (libvgm: dzielenie przez zero).",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block(0x40, g.cmp_block_payload(0, 16, 4, 8, 0, 0, cmp_ok_tail))),
        "blk_cmp_bitscmp_gt_bitsdec.vgm", "Blok 0x40: bitsCmp=8 > bitsDec=4.",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block(0x40, g.cmp_block_payload(0, 16, 8, 17, 0, 0, cmp_ok_tail))),
        "blk_cmp_bitscmp_gt_16.vgm", "Blok 0x40: bitsCmp=17 (> 16).", err("BadDataBlock"),
        hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block_raw(0x40, 4, b"\x00\x10\x00\x00")),
        "blk_cmp_too_short.vgm", "Blok 0x40: dblkLen=4 (< 5, naglowek kompresji ucety).",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block(0x40, g.cmp_block_payload(5, 16, 8, 4, 0, 0, cmp_ok_tail))),
        "blk_cmp_unknown_type.vgm", "Blok 0x40: nieznany typ kompresji 5 (znane: 0, 1).",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block(0x40, g.cmp_block_payload(0, 0xFFFFFFFF, 8, 8, 0, 0, cmp_ok_tail))),
        "blk_cmp_decmplen_huge.vgm", "Blok 0x40: decmpLen=0xFFFFFFFF (4 GiB) z kilkoma bajtami danych.",
        err("LimitExceeded", [alt_err("BadDataBlock")]), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block(0x40, g.cmp_block_payload(0, 16, 8, 4, 2, 0, cmp_ok_tail))),
        "blk_cmp_table_missing.vgm", "Blok 0x40 w trybie tabelowym bez wczesniejszego bloku 0x7F.",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s),
                   s.data_block(0x7F, g.table_block_payload(0, 2, 8, 4, 4, bytes([0, 10, 20, 30]))),
                   s.data_block(0x40, g.cmp_block_payload(0, 16, 8, 4, 2, 0, cmp_ok_tail))),
        "blk_cmp_table_too_small.vgm",
        "Tabela 0x7F ma 4 wartosci, a bitsCmp=4 wymaga 16 (odczyt poza tabela w libvgm).",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block(0x7F, g.table_block_payload(0, 2, 8, 4, 0x1000, b"\x00\x00"))),
        "blk_table_truncated.vgm", "Blok 0x7F: valueCount=0x1000, a w bloku tylko 2 B tabeli.",
        err("BadDataBlock"), hazard="H3/ADR8.9")
    blk(lambda s: (pre(s), s.data_block_raw(0xC0, 1, b"\x00")),
        "blk_ram_too_short.vgm", "Blok zapisu RAM 0xC0: dblkLen=1 (< 2).", err("BadDataBlock"),
        hazard="H3/ADR8.9")

    # kolejnosc blok / 0x91 / petla
    def after_set_data(s):
        pre(s)
        s.data_block(0x00, g.sine_wave_8bit(64))
        s.dac_setup(0, 0x02, 0x00, 0x2A)
        s.dac_data(0, 0x00, 1, 0)
        s.data_block(0x00, g.sine_wave_8bit(32))
        s.dac_freq(0, 8820)
        s.dac_start(0, 0, 0x01, 64)
        s.wait(441)

    d, m = stream_file(0x151, after_set_data, clocks={"SN76496": CLK_SN, "YM2612": CLK_YM2612})
    add(K, "blk_after_set_data.vgm", d,
        "Blok 0x67 typ 0x00 PO komendzie 0x91 (Set Data) banku, ktory juz ma dane (realokacja = wiszacy "
        "wskaznik strumienia DAC, H3).",
        err("BadDataBlock"), hazard="H3/ADR8.10",
        note="Decyzja: odrzucenie (BadDataBlock), nie ostrzezenie BlockAfterSetData.")

    def after_set_data_empty(s):
        pre(s)
        s.dac_setup(0, 0x02, 0x00, 0x2A)
        s.dac_data(0, 0x00, 1, 0)
        s.data_block(0x00, g.sine_wave_8bit(64))
        s.dac_freq(0, 8820)
        s.dac_start(0, 0, 0x01, 64)
        s.wait(441)

    d, m = stream_file(0x151, after_set_data_empty, clocks={"SN76496": CLK_SN, "YM2612": CLK_YM2612})
    add(K, "blk_after_set_data_empty_bank.vgm", d,
        "Blok 0x67 po 0x91, ale bank byl pusty w chwili 0x91 (wskaznik NULL, dlugosc 0): brak wiszacego wskaznika.",
        ok(alternatives=[alt_err("BadDataBlock")]), hazard="H3/ADR8.10", meta=m,
        note="Implementacja przyjmuje (bank pusty = bezpieczny); alternatywa = odrzucenie jak w blk_after_set_data.")

    def after_loop(s):
        pre(s)
        s.mark_loop()
        s.wait(1000)
        s.data_block(0x00, g.sine_wave_8bit(64))
        s.wait(1000)

    d, m = stream_file(0x151, after_loop, clocks={"SN76496": CLK_SN, "YM2612": CLK_YM2612})
    add(K, "blk_after_loop.vgm", d, "Blok 0x67 po znaczniku petli (loopOfs).",
        ok(["BlockAfterLoop"]), hazard="H3/ADR8.10", meta=m)

    # --- strumien komend
    def short_melody(s):
        g.play(s, g.SnVoice(s), SCALE[:2], tail=441)

    d, m = stream_file(0x150, short_melody, clocks={"SN76496": CLK_SN}, end=False)
    m = dict(m)
    add(K, "stream_no_end_command.vgm", d, "Strumien bez komendy 0x66 (konczy sie na dataEnd).",
        ok(["EndsEarly"]), hazard="ADR8.8d", meta=m, scan=False)
    sc = g.scan_stream(d, m["dataOffset"], m["dataEnd"])
    ENTRIES[-1]["entry"]["stream"] = {k: v for k, v in sc.items() if k != "error"}

    def tail(raw_bytes):
        def f(s):
            short_melody(s)
            s.raw(raw_bytes)
        return f

    for nm, raw_b, desc in (
            ("stream_last_cmd_truncated_ym2612", b"\x52", "Ostatnia komenda 0x52 (3 B) ucieta: brak operandow."),
            ("stream_last_cmd_truncated_wait", b"\x61\x10", "Ostatnia komenda 0x61 ucieta po 1 z 2 operandow."),
            ("stream_last_cmd_truncated_block_header", b"\x67\x66\x00", "Ostatnia komenda 0x67 ucieta w naglowku bloku."),
            ("stream_last_cmd_truncated_dacstart", b"\x93\x00\x00\x00", "Ostatnia komenda 0x93 (11 B) ucieta po 4 B.")):
        d, m = stream_file(0x151, tail(raw_b), clocks={"SN76496": CLK_SN}, end=False)
        add(K, nm + ".vgm", d, desc, err("BadCommandStream", [alt_err("Truncated")]), hazard="H2/ADR8.8a",
            note="ErrorCode nie jest rozstrzygniety w ADR; implementacja zwraca Truncated (dane urwane).")

    d, m = stream_file(0x150, tail(b"\x3A\x55\x66"), clocks={"SN76496": CLK_SN}, end=False)
    add(K, "stream_unknown_cmd_skippable.vgm", d,
        "Zarezerwowana komenda 0x3A z 1 operandem (znana dlugosc), potem 0x66.",
        ok(["UnknownCommand"], [alt_err("BadCommandStream")]), hazard="ADR8.8b")
    d, m = stream_file(0x150, tail(b"\x69\x00\x00\x66"), clocks={"SN76496": CLK_SN}, end=False)
    add(K, "stream_unknown_cmd_unskippable.vgm", d,
        "Komenda 0x69 bez znanej dlugosci - dalsze parsowanie niemozliwe.",
        err("BadCommandStream", [alt_ok("UnknownCommand")]), hazard="ADR8.8b",
        note="ZALOZENIE: 0x69 nie ma zdefiniowanej dlugosci w tabeli libvgm (niezweryfikowane z kodem).")

    def ym_no_clock(s):
        g.Ym2612Voice(s).setup()
        s.wait(100)
        g.Ym2612Voice(s).on(60)
        s.wait(441)

    d, m = stream_file(0x151, ym_no_clock, clocks={"SN76496": CLK_SN})
    add(K, "stream_chip_without_clock.vgm", d,
        "Zapisy YM2612 w strumieniu, ale zegar YM2612 w naglowku = 0.",
        ok(["ChipNotInHeader"]), hazard="ADR8.11", meta=m)

    # --- przepelnienie dlugosci (>2^32 tickow, > 24 h)
    v = Vgm(0x150, {"SN76496": CLK_SN})
    s = Stream()
    for _ in range(70000):
        s.wait_61(65535)
    s.end()
    d, m = v.build(s)
    add(K, "stream_delay_overflow.vgz", g.gzip_bytes(d),
        "70000 x 0x61 0xFFFF = 4.59e9 tickow (> 2^32 i > 24 h); .vgz zajmuje kilkaset bajtow.",
        err("LimitExceeded", [alt_err("BadCommandStream")]), raw=d, hazard="ADR8.8c/Limits.maxTotalSamples")
    # (brak 'meta' - spodziewany blad)

    # --- sygnatura gzip w srodku
    # (zakres: gz_nested powyzej)


# ---------------------------------------------------------------------------
# Przypadki limitow (te same pliki, mniejsze Limits)
# ---------------------------------------------------------------------------
LIMIT_CASES = [
    {"id": "limit_input_bytes", "path": "valid/sn76489_scale_v100.vgm",
     "limits": {"maxInputBytes": 64}, "expect": {"result": "error", "error": "TooLarge"},
     "description": "Plik wejsciowy wiekszy niz maxInputBytes."},
    {"id": "limit_decompressed_bytes", "path": "valid/megadrive_ym2612_sn76489_v151.vgz",
     "limits": {"maxDecompressedBytes": 100}, "expect": {"result": "error", "error": "TooLarge"},
     "description": "Poprawny .vgz, wynik > maxDecompressedBytes."},
    {"id": "limit_max_commands", "path": "valid/sn76489_scale_v100.vgm",
     "limits": {"maxCommands": 5}, "expect": {"result": "error", "error": "LimitExceeded"},
     "description": "Liczba komend w pre-skanie > maxCommands."},
    {"id": "limit_total_samples", "path": "valid/sn76489_scale_v100.vgm",
     "limits": {"maxTotalSamples": 1000}, "expect": {"result": "error", "error": "LimitExceeded"},
     "description": "Suma opoznien > maxTotalSamples."},
    {"id": "limit_pcm_budget", "path": "valid/ym2612_dac_block_v150.vgm",
     "limits": {"maxPcmBudgetBytes": 1000}, "expect": {"result": "error", "error": "LimitExceeded"},
     "description": "Blok PCM 4410 B > budzet 1000 B."},
    {"id": "limit_single_block", "path": "valid/ym2612_dac_block_v150.vgm",
     "limits": {"maxSingleBlockBytes": 1000},
     "expect": {"result": "error", "error": "LimitExceeded",
                "alternatives": [{"result": "error", "error": "BadDataBlock"}]},
     "description": "Pojedynczy blok 4410 B > maxSingleBlockBytes."},
    {"id": "limit_tag_field", "path": "valid/sn76489_loop_gg_v110_gd3jp.vgm",
     "limits": {"maxTagFieldBytes": 8},
     "expect": {"result": "ok", "warnings": ["TagTruncated"]},
     "description": "Pola GD3 dluzsze niz maxTagFieldBytes sa przycinane (ostrzezenie)."},
]


# ---------------------------------------------------------------------------
# Weryfikacja nazw wzgledem naglowkow
# ---------------------------------------------------------------------------
def enum_names(text: str, enum: str) -> set[str]:
    m = re.search(r"enum class %s\b[^{]*\{(.*?)\};" % enum, text, re.S)
    if not m:
        return set()
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return {n for n in re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\b\s*(?:=\s*\d+)?\s*(?:,|$)", body)}


def collect_names(manifest):
    errs, warns, chips = set(), set(), set()

    def visit_expect(e):
        if not e:
            return
        if "error" in e:
            errs.add(e["error"])
        warns.update(e.get("warnings", []))
        for a in e.get("alternatives", []):
            visit_expect(a)

    for f in manifest["files"]:
        visit_expect(f["expect"])
        for c in f.get("header", {}).get("chips", []):
            chips.add(c["type"])
    for lc in manifest["limitCases"]:
        visit_expect(lc["expect"])
    return errs, warns, chips


def verify_names(manifest):
    core = (REPO / "include/vgmplay/core.hpp").read_text(encoding="utf-8")
    chips_h = (REPO / "include/vgmplay/chips.hpp").read_text(encoding="utf-8")
    known_err = enum_names(core, "ErrorCode")
    known_warn = enum_names(core, "WarningCode")
    known_chip = enum_names(chips_h, "ChipType")
    errs, warns, chips = collect_names(manifest)
    return {
        "ErrorCode": sorted(errs - known_err),
        "WarningCode": sorted(warns - known_warn),
        "ChipType": sorted(chips - known_chip),
    }, (len(known_err), len(known_warn), len(known_chip))


# ---------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO / "tests" / "data"))
    ap.add_argument("--allow-missing-names", action="store_true")
    args = ap.parse_args(argv)
    out = Path(args.out)

    build_valid()
    build_malformed()

    names = [e["entry"]["path"] for e in ENTRIES]
    assert len(names) == len(set(names)), "duplikaty nazw"
    ENTRIES.sort(key=lambda e: e["entry"]["path"])

    manifest = {
        "schemaVersion": MANIFEST_SCHEMA,
        "generator": "tools/vgmgen/build_corpus.py",
        "note": "Plik generowany - nie edytowac recznie. Opis pol: tests/data/README.md.",
        "files": [e["entry"] for e in ENTRIES],
        "limitCases": LIMIT_CASES,
    }
    for lc in LIMIT_CASES:
        assert lc["path"] in names, lc

    missing, counts = verify_names(manifest)
    if counts[0] == 0 or counts[1] == 0 or counts[2] == 0:
        print("BLAD: nie odczytano enumow z naglowkow (%s)" % (counts,), file=sys.stderr)
        return 2
    manifest["unverifiedNames"] = {k: v for k, v in missing.items() if v}
    for k, v in missing.items():
        if v:
            print("BRAK w kontrakcie (%s): %s" % (k, ", ".join(v)), file=sys.stderr)
    if any(missing.values()) and not args.allow_missing_names:
        print("Przerwano: nazwy spoza kontraktu (uzyj --allow-missing-names).", file=sys.stderr)
        return 3

    total = sum(len(e["data"]) for e in ENTRIES)
    if total >= MAX_CORPUS_BYTES:
        print("BLAD: korpus %d B >= 1 MiB" % total, file=sys.stderr)
        return 4

    for sub in ("valid", "malformed"):
        d = out / sub
        if d.exists():
            for f in d.iterdir():
                if f.suffix in (".vgm", ".vgz"):
                    f.unlink()
        d.mkdir(parents=True, exist_ok=True)
    for e in ENTRIES:
        (out / e["entry"]["path"]).write_bytes(e["data"])
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    (out / "manifest.json").write_bytes(text.encode("utf-8"))

    nv = sum(1 for e in ENTRIES if e["entry"]["kind"] == "valid")
    print("valid: %d, malformed: %d, razem %d B (%.1f KiB), manifest: %d B"
          % (nv, len(ENTRIES) - nv, total, total / 1024, len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
