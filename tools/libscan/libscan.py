#!/usr/bin/env python3
"""Skaner biblioteki VGM: ktore uklady i specjalne tryby wystepuja w plikach - do doboru utworow testowych.

Dla kazdego pliku .vgm/.vgz w katalogu (rekursywnie) czyta naglowek (zegary ukladow, tylko pola zdefiniowane
w wersji pliku) i przechodzi strumien komend, szukajac zapisow wlaczajacych tryby, ktore maja osobna logike
w dekoderach nut (src/chipstate) i w mapowaniu kanalow: perkusja OPL/OPLL/OPNA, OPL3 4-op, DAC i kanal 3
specjalny YM2612/OPN, ADPCM YM2608/YM2610, obwiednia AY/SSG, szum SN76489, DPCM/FDS NES, kanal falowy GB.

Wynik: lista gier (folder pierwszego poziomu) z ukladami i trybami, potem dla kazdego ukladu/trybu do trzech
przykladowych utworow z roznych gier i liczba plikow. Pliki .zip sa pomijane (vgmrips: folder obok archiwum).
Skaner tylko czyta biblioteke; nic w niej nie zapisuje.

Uzycie (z korzenia repozytorium; Python 3.10+, tylko biblioteka standardowa):
    python -I tools/libscan/libscan.py D:/emu/vgm
    python -I tools/libscan/libscan.py D:/emu/vgm --json wynik.json   # dodatkowo pelna lista plikow
"""
from __future__ import annotations

import argparse
import collections
import gzip
import json
import os
import struct
import sys

# Offset zegara w naglowku -> nazwa ukladu.
CHIP_CLOCKS = {
    0x0C: "SN76489", 0x10: "YM2413", 0x2C: "YM2612", 0x30: "YM2151", 0x38: "SegaPCM", 0x40: "RF5C68",
    0x44: "YM2203", 0x48: "YM2608", 0x4C: "YM2610", 0x50: "YM3812", 0x54: "YM3526", 0x58: "Y8950",
    0x5C: "YMF262", 0x60: "YMF278B", 0x64: "YMF271", 0x68: "YMZ280B", 0x6C: "RF5C164", 0x70: "PWM",
    0x74: "AY8910", 0x80: "GameBoy", 0x84: "NES", 0x88: "MultiPCM", 0x8C: "uPD7759", 0x90: "OKIM6258",
    0x98: "OKIM6295", 0x9C: "K051649", 0xA0: "K054539", 0xA4: "HuC6280", 0xA8: "C140", 0xAC: "K053260",
    0xB0: "Pokey", 0xB4: "QSound", 0xB8: "SCSP", 0xC0: "WonderSwan", 0xC4: "VSU", 0xC8: "SAA1099",
    0xCC: "ES5503", 0xD0: "ES5506", 0xD8: "X1-010", 0xDC: "C352", 0xE0: "GA20",
}


def min_version(ofs: int) -> int:
    """Najnizsza wersja VGM definiujaca pole naglowka - starsze pliki maja tam smieci albo dane."""
    if ofs < 0x40:
        return 0x100
    if ofs < 0x80:
        return 0x151
    if ofs < 0xB8:
        return 0x161
    return 0x171


def cmd_len(op: int) -> int:
    if op < 0x30:
        return 1
    if op < 0x40 or op in (0x4F, 0x50):
        return 2
    if op < 0x60 or op == 0x61:
        return 3
    if op < 0x90:
        return 1
    if op <= 0x95:
        return (5, 5, 6, 11, 2, 5)[op - 0x90]
    if op < 0xA0:
        return 1
    if op < 0xC0:
        return 3
    if op < 0xE0:
        return 4
    return 5


def scan_file(data: bytes) -> tuple[set[str], set[str]] | None:
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    if len(data) < 0x40 or data[:4] != b"Vgm ":
        return None
    ver = struct.unpack_from("<I", data, 0x08)[0]
    start = struct.unpack_from("<I", data, 0x34)[0] + 0x34 if ver >= 0x150 else 0x40
    start = max(start, 0x40)

    chips: set[str] = set()
    feats: set[str] = set()
    for ofs, name in CHIP_CLOCKS.items():
        if ver < min_version(ofs) or ofs + 4 > start or ofs + 4 > len(data):
            continue
        raw = struct.unpack_from("<I", data, ofs)[0]
        if raw & 0x3FFFFFFF:
            chips.add(name)
            if name == "NES" and raw & 0x80000000:
                feats.add("NES FDS")
            if name == "SN76489" and raw & 0x80000000:
                feats.add("T6W28 (NGP)")

    pos = start
    while pos < len(data):
        op = data[pos]
        if op == 0x66:
            break
        if op == 0x67:
            if pos + 7 > len(data):
                break
            pos += 7 + (struct.unpack_from("<I", data, pos + 3)[0] & 0x7FFFFFFF)
            continue
        if pos + 3 > len(data):
            break
        r, v = data[pos + 1], data[pos + 2]
        if op in (0x5A, 0x5B, 0x5E, 0xAA, 0xAB, 0xAE) and r == 0xBD and v & 0x20:
            feats.add("OPL perkusja")
        elif op in (0x5F, 0xAF) and r == 0x04 and v & 0x3F:
            feats.add("OPL3 4-op")
        elif op == 0x51 and r == 0x0E and v & 0x20:
            feats.add("OPLL perkusja")
        elif op == 0x51 and r <= 0x07:
            feats.add("OPLL wlasny instrument")
        elif op == 0x52 and r == 0x2B and v & 0x80:
            feats.add("YM2612 DAC")
        elif op in (0x52, 0x55, 0x56, 0x58) and r == 0x27 and (v & 0xC0) in (0x40, 0x80):
            feats.add({0x52: "YM2612", 0x55: "YM2203", 0x56: "YM2608", 0x58: "YM2610"}[op] + " kanal 3 specjalny")
        elif op == 0x56 and r == 0x10 and v and not v & 0x80:
            feats.add("YM2608 perkusja")
        elif op == 0x57 and r == 0x00 and v & 0x80:
            feats.add("YM2608 ADPCM-B")
        elif op == 0x59 and r == 0x00 and v and not v & 0x80:
            feats.add("YM2610 ADPCM-A")
        elif op == 0x58 and r == 0x10 and v & 0x80:
            feats.add("YM2610 ADPCM-B")
        elif op in (0x55, 0x56, 0x58) and r in (0x08, 0x09, 0x0A) and v & 0x10:
            feats.add("SSG obwiednia")
        elif op == 0x50 and (r & 0xF0) == 0xE0:
            feats.add("SN76489 szum")
        elif op == 0xA0 and (r & 0x7F) == 0x0D:
            feats.add("AY obwiednia")
        elif op == 0xB4 and (r & 0x7F) == 0x15 and v & 0x10:
            feats.add("NES DPCM")
        elif op == 0xB3 and (r & 0x7F) == 0x0A and v & 0x80:
            feats.add("GB kanal falowy")
        if 0x80 <= op <= 0x8F or op in (0x93, 0x95):
            feats.add("strumien DAC")
        pos += cmd_len(op)
    return chips, feats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("root", help="katalog biblioteki VGM")
    ap.add_argument("--json", help="zapisz pelna liste plikow (gra, plik, uklady, tryby) do tego pliku")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    files = []
    for dirpath, _dirs, names in os.walk(args.root):
        for name in sorted(names):
            if not name.lower().endswith((".vgm", ".vgz")):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, "rb") as fh:
                    res = scan_file(fh.read())
            except (OSError, EOFError, gzip.BadGzipFile, struct.error) as e:
                print(f"POMINIETY {path}: {e}", file=sys.stderr)
                continue
            if res is None:
                continue
            game = os.path.relpath(path, args.root).split(os.sep)[0]
            files.append((game, name, sorted(res[0]), sorted(res[1])))

    games: dict[str, list[set[str]]] = collections.defaultdict(lambda: [set(), set()])
    for game, _name, chips, feats in files:
        games[game][0].update(chips)
        games[game][1].update(feats)
    print(f"== gry ({len(games)}), pliki: {len(files)}")
    for game, (chips, feats) in sorted(games.items()):
        print(f"{game} | {', '.join(sorted(chips))} | {', '.join(sorted(feats))}")

    print("\n== przyklady (uklad/tryb :: do 3 gier, liczba plikow)")
    hits: dict[str, list[tuple[str, str]]] = collections.defaultdict(list)
    for game, name, chips, feats in files:
        for key in chips + feats:
            hits[key].append((game, name))
    for key in sorted(hits):
        picked: list[tuple[str, str]] = []
        for game, name in hits[key]:
            if all(game != g for g, _ in picked):
                picked.append((game, name))
            if len(picked) == 3:
                break
        print(f"{key} :: " + " || ".join(f"{g} / {n}" for g, n in picked) + f" ({len(hits[key])})")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(files, fh, ensure_ascii=False, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
