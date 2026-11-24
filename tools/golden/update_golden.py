#!/usr/bin/env python3
"""Aktualizacja wzorcow golden: tests/golden/golden.json.

Golden audio to TEST REGRESJI, nie poprawnosci: zapisuje, jak dekoder gral w chwili aktualizacji.
Dla kazdego pliku z tagiem "golden" w tests/data/manifest.json uruchamia `vgmrender` (ten sam dekoder co silnik)
przy 44100 Hz, petle 2, wyciszenie 1000 ms (domyslne rdzenie vgmplay), czyta WAV (int16 stereo) i zapisuje:
liczbe ramek, SHA-256 surowego PCM, RMS, szczyt oraz obwiednie RMS w oknach 100 ms.

Uzycie (z korzenia repozytorium; Python 3.10+, tylko biblioteka standardowa):
    python3 -I tools/golden/update_golden.py --vgmrender out/build/msvc-x64-release/tools/vgmrender/vgmrender.exe
    python3 -I tools/golden/update_golden.py --check      # tylko porownaj, nic nie zapisuj (kod 1 = roznice)

Wzorce zmieniaj PO PRZEGLADZIE (odsluch, uzasadnienie w commicie). Test C++ (tests/golden_test.cpp) liczy te same
skroty, renderujac przez dekoder bezposrednio - zgodnosc obu drog jest dodatkowa kontrola.
"""
import argparse
import glob
import hashlib
import json
import math
import os
import struct
import subprocess
import sys
import tempfile

RATE = 44100
LOOPS = 2
FADE_MS = 1000
WINDOW = RATE // 10


def find_vgmrender(root):
    cands = sorted(glob.glob(os.path.join(root, "out", "build", "*", "tools", "vgmrender", "vgmrender*")),
                   key=os.path.getmtime, reverse=True)
    cands = [c for c in cands if os.path.isfile(c) and c.lower().endswith(".exe")]
    return cands[0] if cands else None


def read_wav_pcm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "to nie WAV"
    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif tag == b"data":
            assert fmt == (1, 2, RATE, RATE * 4, 4, 16), "nieoczekiwany format WAV: %r" % (fmt,)
            return body
        pos += 8 + size + (size & 1)
    raise AssertionError("brak chunku data")


def metrics(pcm):
    n = len(pcm) // 2
    samples = struct.unpack("<%dh" % n, pcm)
    frames = n // 2
    sumsq = 0.0
    peak = 0.0
    wins = []
    wsq = 0.0
    wfill = 0
    for i in range(frames):
        for ch in (0, 1):
            v = samples[2 * i + ch] / 32768.0
            sumsq += v * v
            wsq += v * v
            peak = max(peak, abs(v))
        wfill += 1
        if wfill == WINDOW:
            wins.append(math.sqrt(wsq / (2.0 * WINDOW)))
            wsq = 0.0
            wfill = 0
    if wfill:
        wins.append(math.sqrt(wsq / (2.0 * wfill)))
    rms = math.sqrt(sumsq / (2.0 * frames)) if frames else 0.0
    return frames, rms, peak, wins


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vgmrender", help="sciezka do vgmrender (domyslnie: najnowszy w out/build/*)")
    ap.add_argument("--data", default=os.path.join(root, "tests", "data"))
    ap.add_argument("--out", default=os.path.join(root, "tests", "golden", "golden.json"))
    ap.add_argument("--check", action="store_true", help="nie zapisuj, tylko porownaj z istniejacym plikiem")
    args = ap.parse_args()

    exe = os.path.abspath(args.vgmrender) if args.vgmrender else find_vgmrender(root)
    if not exe or not os.path.isfile(exe):
        sys.exit("nie znaleziono vgmrender; podaj --vgmrender")
    with open(os.path.join(args.data, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    paths = [e["path"] for e in manifest["files"] if "golden" in e.get("tags", [])]

    files = []
    with tempfile.TemporaryDirectory() as tmp:
        for p in paths:
            wav = os.path.join(tmp, "out.wav")
            r = subprocess.run([exe, os.path.join(args.data, p), wav, "--rate", str(RATE), "--loops", str(LOOPS),
                                "--fade", str(FADE_MS)], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit("vgmrender zawiodl dla %s:\n%s" % (p, r.stderr))
            pcm = read_wav_pcm(wav)
            frames, rms, peak, wins = metrics(pcm)
            files.append({"path": p, "frames": frames, "sha256": hashlib.sha256(pcm).hexdigest(),
                          "rms": round(rms, 8), "peak": round(peak, 8), "windowRms": [round(w, 8) for w in wins]})
            print("%-60s %8d ramek  rms=%.5f  peak=%.5f  %s" % (p, frames, rms, peak, files[-1]["sha256"][:16]))

    doc = {
        "schemaVersion": 1,
        "note": ("Wzorce golden = TEST REGRESJI, nie poprawnosci brzmienia (korpus syntetyczny, nieodsluchany). "
                 "Generowane przez tools/golden/update_golden.py; zmieniaj po przegladzie, z uzasadnieniem w commicie."),
        "render": {"sampleRate": RATE, "loops": LOOPS, "fadeMs": FADE_MS,
                   "pcm": "int16 LE stereo interleaved (clamp, *32767, zaokraglenie - jak vgmrender), SHA-256 surowych bajtow",
                   "cores": "domyslne rdzenie vgmplay (polityka src/engine/chip_map.cpp)"},
        "files": files,
    }
    if args.check:
        with open(args.out, encoding="utf-8") as f:
            old = json.load(f)
        bad = [a["path"] for a, b in zip(files, old["files"]) if a["sha256"] != b["sha256"] or a["frames"] != b["frames"]]
        if len(files) != len(old["files"]) or bad:
            print("ROZNICE:", bad or "inna liczba plikow")
            sys.exit(1)
        print("zgodne ze wzorcami")
        return
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("zapisano", args.out)


if __name__ == "__main__":
    main()
