#!/usr/bin/env python3
"""vgmgen - generator syntetycznych plikow VGM/VGZ dla korpusu testowego vgmplay.

Tylko biblioteka standardowa. Uruchamiaj z `python -I` (tryb izolowany).
Zadne dane nie pochodza z rippow - melodie sa wlasne/public domain, patche to proste
ustawienia rejestrow. Wyniki sa deterministyczne (brak czasu, losowosci, mtime=0 w gzip).

Uzycie jako biblioteka: patrz build_corpus.py. CLI:
    python -I tools/vgmgen/vgmgen.py scan PLIK.vgm|PLIK.vgz   # niezalezny skaner strumienia
"""
from __future__ import annotations

import math
import struct
import sys
import zlib

SAMPLE_RATE = 44100

# ---------------------------------------------------------------------------
# Uklady: nazwa (zgodna z vgmplay::ChipType) -> (offset zegara w naglowku, min. wersja)
# ---------------------------------------------------------------------------
CHIP_FIELDS = {
    "SN76496": (0x0C, 0x100),
    "YM2413": (0x10, 0x100),
    "YM2612": (0x2C, 0x110),
    "YM2151": (0x30, 0x110),
    "SegaPcm": (0x38, 0x151),
    "RF5C68": (0x40, 0x151),
    "YM2203": (0x44, 0x151),
    "YM2608": (0x48, 0x151),
    "YM2610": (0x4C, 0x151),
    "YM3812": (0x50, 0x151),
    "YM3526": (0x54, 0x151),
    "Y8950": (0x58, 0x151),
    "YMF262": (0x5C, 0x151),
    "YMF278B": (0x60, 0x151),
    "YMF271": (0x64, 0x151),
    "YMZ280B": (0x68, 0x151),
    "RF5C164": (0x6C, 0x151),
    "PWM32X": (0x70, 0x151),
    "AY8910": (0x74, 0x151),
}
DUAL_BIT = 0x40000000

# Typowe zegary
CLK_SN = 3579545
CLK_YM2413 = 3579545
CLK_YM2612 = 7670454
CLK_YM2151 = 3579545
CLK_AY = 1789773
CLK_OPL2 = 3579545
CLK_OPL3 = 14318180


def header_size(version: int) -> int:
    """Rozmiar naglowka (== domyslny dataOfs) dla wersji BCD."""
    if version < 0x150:
        return 0x40
    if version < 0x151:
        return 0x40
    if version < 0x170:
        return 0x80
    if version < 0x171:
        return 0xC0
    return 0x100


def u32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def u16(v: int) -> bytes:
    return struct.pack("<H", v & 0xFFFF)


def patch(data: bytes, off: int, value: int, size: int = 4) -> bytes:
    """Zwraca kopie data z wartoscia LE wpisana pod off."""
    b = bytearray(data)
    b[off:off + size] = int(value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
    return bytes(b)


# ---------------------------------------------------------------------------
# gzip (deterministyczny: mtime=0, OS=255)
# ---------------------------------------------------------------------------
def gzip_bytes(raw: bytes, level: int = 9, *, crc: int | None = None,
               isize: int | None = None) -> bytes:
    """Gzip bez zaleznosci od czasu. crc/isize pozwalaja wpisac falszywe wartosci."""
    co = zlib.compressobj(level, zlib.DEFLATED, -15, 9)
    body = bytearray()
    for i in range(0, len(raw), 1 << 20):
        body += co.compress(raw[i:i + (1 << 20)])
    body += co.flush()
    xfl = 2 if level == 9 else (4 if level == 1 else 0)
    hdr = bytes([0x1F, 0x8B, 8, 0, 0, 0, 0, 0, xfl, 0xFF])
    c = zlib.crc32(raw) & 0xFFFFFFFF if crc is None else crc
    n = len(raw) & 0xFFFFFFFF if isize is None else isize
    return hdr + bytes(body) + u32(c) + u32(n)


def gunzip(data: bytes) -> bytes:
    return zlib.decompress(data, 16 + 15)


# ---------------------------------------------------------------------------
# GD3
# ---------------------------------------------------------------------------
GD3_FIELDS = ("track_en", "track_jp", "game_en", "game_jp", "system_en", "system_jp",
              "author_en", "author_jp", "date", "converter", "notes")


class Gd3:
    def __init__(self, **kw):
        for f in GD3_FIELDS:
            setattr(self, f, kw.pop(f, ""))
        if kw:
            raise TypeError("nieznane pola GD3: %s" % ", ".join(kw))

    def fields(self):
        return [getattr(self, f) for f in GD3_FIELDS]

    def data(self) -> bytes:
        return b"".join(s.encode("utf-16-le") + b"\0\0" for s in self.fields())

    def chunk(self) -> bytes:
        d = self.data()
        return b"Gd3 " + u32(0x100) + u32(len(d)) + d

    def expected(self) -> dict:
        """Oczekiwane pola (nazwy zgodne z vgmplay::format::Gd3Tag)."""
        return {
            "present": True,
            "trackEn": self.track_en, "trackJp": self.track_jp,
            "gameEn": self.game_en, "gameJp": self.game_jp,
            "systemEn": self.system_en, "systemJp": self.system_jp,
            "authorEn": self.author_en, "authorJp": self.author_jp,
            "releaseDate": self.date, "converter": self.converter, "notes": self.notes,
        }


def gd3_raw_chunk(data: bytes, *, length: int | None = None, version: int = 0x100,
                  sig: bytes = b"Gd3 ") -> bytes:
    """Dowolny kawalek GD3 (do plikow uszkodzonych)."""
    return sig + u32(version) + u32(len(data) if length is None else length) + data


def utf16_units(units) -> bytes:
    """Surowe jednostki UTF-16 (pozwala na niesparowane surogaty)."""
    return b"".join(struct.pack("<H", u) for u in units)


# ---------------------------------------------------------------------------
# Strumien komend
# ---------------------------------------------------------------------------
class Stream:
    def __init__(self):
        self.buf = bytearray()
        self.samples = 0                  # suma opoznien (ticki 44100)
        self.loop = None                  # (offset w buf, samples w chwili znacznika)

    # -- podstawy
    def raw(self, b: bytes):
        self.buf += b
        return self

    def mark_loop(self):
        self.loop = (len(self.buf), self.samples)
        return self

    def end(self):
        self.buf.append(0x66)
        return self

    def wait(self, n: int):
        """Najkrotszy sensowny zapis opoznienia n tickow (0x62/0x63/0x7n/0x61)."""
        while n > 0:
            if n >= 65535:
                self.buf += b"\x61\xff\xff"
                self.samples += 65535
                n -= 65535
            elif n == 735:
                self.buf.append(0x62)
                self.samples += n
                n = 0
            elif n == 882:
                self.buf.append(0x63)
                self.samples += n
                n = 0
            elif n <= 16:
                self.buf.append(0x70 + n - 1)
                self.samples += n
                n = 0
            else:
                self.buf += b"\x61" + u16(n)
                self.samples += n
                n = 0
        return self

    def wait_61(self, n: int):
        self.buf += b"\x61" + u16(n)
        self.samples += n
        return self

    def wait_frames(self, frames: int, pal: bool = False):
        for _ in range(frames):
            self.buf.append(0x63 if pal else 0x62)
            self.samples += 882 if pal else 735
        return self

    # -- zapisy do ukladow (chip: 0 lub 1 = drugi uklad)
    def sn(self, dd: int, chip: int = 0):
        self.buf += bytes([0x30 if chip else 0x50, dd])
        return self

    def gg_stereo(self, dd: int, chip: int = 0):
        self.buf += bytes([0x3F if chip else 0x4F, dd])
        return self

    def ym2612(self, port: int, reg: int, val: int, chip: int = 0):
        self.buf += bytes([(0xA2 if chip else 0x52) + port, reg, val])
        return self

    def ym2151(self, reg: int, val: int, chip: int = 0):
        self.buf += bytes([0xA4 if chip else 0x54, reg, val])
        return self

    def ay(self, reg: int, val: int, chip: int = 0):
        self.buf += bytes([0xA0, reg | (0x80 if chip else 0), val])
        return self

    def opl2(self, reg: int, val: int, chip: int = 0):
        self.buf += bytes([0xAA if chip else 0x5A, reg, val])
        return self

    def opl3(self, port: int, reg: int, val: int, chip: int = 0):
        self.buf += bytes([(0xAE if chip else 0x5E) + port, reg, val])
        return self

    def opll(self, reg: int, val: int, chip: int = 0):
        self.buf += bytes([0xA1 if chip else 0x51, reg, val])
        return self

    # -- bloki danych i DAC
    def data_block(self, typ: int, payload: bytes):
        self.buf += b"\x67\x66" + bytes([typ]) + u32(len(payload)) + payload
        return self

    def data_block_raw(self, typ: int, declared_len: int, payload: bytes):
        """Blok z dowolnie 'klamliwa' deklarowana dlugoscia."""
        self.buf += b"\x67\x66" + bytes([typ]) + u32(declared_len) + payload
        return self

    def ym2612_pcm_write_wait(self, n: int):
        """0x8n: zapis bajtu z banku PCM do rejestru 0x2A + czekanie n (0..15) tickow."""
        assert 0 <= n <= 15
        self.buf.append(0x80 + n)
        self.samples += n
        return self

    def pcm_seek(self, offset: int):
        self.buf += b"\xe0" + u32(offset)
        return self

    def dac_setup(self, ss: int, chip_type: int, port: int, cmd: int):
        self.buf += bytes([0x90, ss, chip_type, port, cmd])
        return self

    def dac_data(self, ss: int, bank: int, step_size: int = 1, step_base: int = 0):
        self.buf += bytes([0x91, ss, bank, step_size, step_base])
        return self

    def dac_freq(self, ss: int, hz: int):
        self.buf += bytes([0x92, ss]) + u32(hz)
        return self

    def dac_start(self, ss: int, start: int, mode: int, length: int):
        self.buf += bytes([0x93, ss]) + u32(start) + bytes([mode]) + u32(length)
        return self

    def dac_stop(self, ss: int):
        self.buf += bytes([0x94, ss])
        return self

    def dac_fast(self, ss: int, block_id: int, flags: int = 0):
        self.buf += bytes([0x95, ss]) + u16(block_id) + bytes([flags])
        return self


# ---------------------------------------------------------------------------
# Bloki skompresowane (0x40..0x7E) i tabela (0x7F)
# ---------------------------------------------------------------------------
def pack_bits(values, bits: int) -> bytes:
    """Pakuje wartosci po `bits` bitow, od najstarszego bitu (MSB first)."""
    acc = 0
    nacc = 0
    out = bytearray()
    for v in values:
        acc = (acc << bits) | (v & ((1 << bits) - 1))
        nacc += bits
        while nacc >= 8:
            nacc -= 8
            out.append((acc >> nacc) & 0xFF)
        acc &= (1 << nacc) - 1
    if nacc:
        out.append((acc << (8 - nacc)) & 0xFF)
    return bytes(out)


def cmp_block_payload(cmp_type: int, decmp_len: int, bits_dec: int, bits_cmp: int,
                      sub_type: int, add_val: int, packed: bytes) -> bytes:
    """Naglowek (10 B) + dane skompresowanego bloku 0x40..0x7E."""
    return (bytes([cmp_type]) + u32(decmp_len) + bytes([bits_dec, bits_cmp, sub_type])
            + u16(add_val) + packed)


def table_block_payload(cmp_type: int, sub_type: int, bits_dec: int, bits_cmp: int,
                        value_count: int, values_bytes: bytes) -> bytes:
    """Blok 0x7F: naglowek 6 B + tabela."""
    return bytes([cmp_type, sub_type, bits_dec, bits_cmp]) + u16(value_count) + values_bytes


# ---------------------------------------------------------------------------
# Plik VGM
# ---------------------------------------------------------------------------
class Vgm:
    """Sklada naglowek + (naglowek rozszerzony) + strumien + GD3.

    clocks: {nazwa_ukladu: zegar_Hz}; dual: zbior nazw z drugim ukladem (bit 30).
    """

    def __init__(self, version: int, clocks: dict | None = None, dual=(), *, rate: int = 0,
                 sn_fb: int = 0, sn_sw: int = 0, sn_flags: int = 0, ay_type: int = 0,
                 volume_mod: int = 0, loop_base: int = 0, loop_mod: int = 0,
                 extra_volumes=None):
        self.version = version
        self.clocks = dict(clocks or {})
        self.dual = set(dual)
        self.rate = rate
        self.sn_fb, self.sn_sw, self.sn_flags = sn_fb, sn_sw, sn_flags
        self.ay_type = ay_type
        self.volume_mod, self.loop_base, self.loop_mod = volume_mod, loop_base, loop_mod
        self.extra_volumes = list(extra_volumes or [])   # [(chipId, flags, volume)]

    def build(self, stream: Stream, gd3=None, *, total_override: int | None = None,
              loop_samples_override: int | None = None):
        """Zwraca (bajty, meta). gd3: Gd3 | bytes (gotowy kawalek) | None."""
        v = self.version
        hs = header_size(v)
        hdr = bytearray(hs)
        hdr[0:4] = b"Vgm "
        hdr[8:12] = u32(v)

        def put(off, val, size=4):
            if off + size > hs:
                raise ValueError("pole 0x%X poza naglowkiem 0x%X (wersja %X)" % (off, hs, v))
            hdr[off:off + size] = int(val).to_bytes(size, "little")

        chips_meta = []
        for name, clk in self.clocks.items():
            off, minv = CHIP_FIELDS[name]
            if v < minv:
                raise ValueError("%s wymaga wersji >= %X" % (name, minv))
            val = clk
            if name in self.dual:
                if v < 0x151:
                    raise ValueError("dual chip wymaga wersji >= 1.51")
                val |= DUAL_BIT
            put(off, val)
            if clk:
                chips_meta.append((off, name, 0, clk))
                if name in self.dual:
                    chips_meta.append((off, name, 1, clk))
        chips_meta.sort(key=lambda t: (t[0], t[2]))

        if self.rate:
            if v < 0x101:
                raise ValueError("rate wymaga >= 1.01")
            put(0x24, self.rate)
        if self.sn_fb or self.sn_sw:
            if v < 0x110:
                raise ValueError("SN feedback wymaga >= 1.10")
            put(0x28, self.sn_fb, 2)
            put(0x2A, self.sn_sw, 1)
        if self.sn_flags:
            put(0x2B, self.sn_flags, 1)
        if self.ay_type:
            put(0x78, self.ay_type, 1)
        if self.volume_mod:
            put(0x7C, self.volume_mod & 0xFF, 1)
        if self.loop_base:
            put(0x7E, self.loop_base & 0xFF, 1)
        if self.loop_mod:
            put(0x7F, self.loop_mod, 1)

        extra = b""
        volumes_meta = {}
        if self.extra_volumes:
            if hs < 0xC0 or v < 0x170:
                raise ValueError("naglowek rozszerzony wymaga wersji >= 1.70")
            vol_block = bytes([len(self.extra_volumes)])
            for cid, flags, vol in self.extra_volumes:
                vol_block += bytes([cid, flags]) + u16(vol)
                volumes_meta[(cid, flags & 1)] = vol
            extra = u32(0x0C) + u32(0) + u32(4) + vol_block   # rozmiar, clockOfs=0, volOfs=4
            put(0xBC, hs - 0xBC)

        data_pos = hs + len(extra)
        if v >= 0x150:
            put(0x34, data_pos - 0x34)
        body = bytes(stream.buf)
        gd3_pos = data_pos + len(body)
        if gd3 is None:
            chunk = b""
        elif isinstance(gd3, Gd3):
            chunk = gd3.chunk()
        else:
            chunk = bytes(gd3)
        if chunk:
            put(0x14, gd3_pos - 0x14)
        total = stream.samples
        put(0x18, (total if total_override is None else total_override) & 0xFFFFFFFF)
        loop_samples = 0
        loop_start = 0
        if stream.loop is not None:
            loff, lsamp = stream.loop
            put(0x1C, data_pos + loff - 0x1C)
            loop_samples = total - lsamp
            loop_start = lsamp
        if loop_samples_override is not None:
            loop_samples = loop_samples_override
        put(0x20, loop_samples & 0xFFFFFFFF)
        file_size = data_pos + len(body) + len(chunk)
        put(0x04, file_size - 4)

        data = bytes(hdr) + extra + body + chunk
        assert len(data) == file_size
        meta = {
            "versionBcd": v,
            "totalSamples": total,
            "loopSamples": loop_samples if stream.loop is not None else 0,
            "hasLoop": stream.loop is not None and loop_samples != 0,
            "loopStartSample": loop_start if stream.loop is not None else 0,
            "rate": self.rate,
            "dataOffset": data_pos,
            "dataEnd": gd3_pos if chunk else file_size,
            "gd3Offset": gd3_pos if chunk else 0,
            "fileSize": file_size,
            "chips": [
                {"type": n, "instance": inst, "clockHz": clk,
                 **({"volume": volumes_meta[(0, inst)]}
                    if n == "SN76496" and (0, inst) in volumes_meta else {})}
                for (_, n, inst, clk) in chips_meta
            ],
        }
        if isinstance(gd3, Gd3):
            meta["gd3"] = gd3.expected()
        return data, meta


# ---------------------------------------------------------------------------
# Niezalezny skaner strumienia (do samokontroli generatora i do `scan`)
# ---------------------------------------------------------------------------
def cmd_length(op: int):
    """Dlugosc komendy w bajtach (bez 0x67) albo None dla nieznanej/zmiennej."""
    if op == 0x66:
        return 1
    if 0x30 <= op <= 0x3F:
        return 2
    if 0x40 <= op <= 0x4E:
        return 3
    if op in (0x4F, 0x50):
        return 2
    if 0x51 <= op <= 0x5F:
        return 3
    if op == 0x61:
        return 3
    if op in (0x62, 0x63):
        return 1
    if op == 0x64:
        return 4
    if op == 0x68:
        return 12
    if 0x70 <= op <= 0x8F:
        return 1
    if op in (0x90, 0x91, 0x95):
        return 5
    if op == 0x92:
        return 6
    if op == 0x93:
        return 11
    if op == 0x94:
        return 2
    if 0xA0 <= op <= 0xBF:
        return 3
    if 0xC0 <= op <= 0xDF:
        return 4
    if 0xE0 <= op <= 0xFF:
        return 5
    return None


def scan_stream(data: bytes, start: int, end: int, loop_abs: int = 0) -> dict:
    """Skanuje komendy [start,end). Zwraca statystyki albo pole 'error'."""
    pos = start
    r = {"commandCount": 0, "totalSamples": 0, "loopStartSample": 0, "pcmBytes": 0,
         "romWriteBytes": 0, "hasEndCommand": False, "error": None}
    while pos < end:
        if loop_abs and pos == loop_abs:
            r["loopStartSample"] = r["totalSamples"]
        op = data[pos]
        if op == 0x67:
            if pos + 7 > end or data[pos + 1] != 0x66:
                r["error"] = "blok 0x67 uciety/zly przy 0x%X" % pos
                return r
            typ = data[pos + 2]
            dlen = struct.unpack_from("<I", data, pos + 3)[0] & 0x7FFFFFFF
            if pos + 7 + dlen > end:
                r["error"] = "blok 0x67 dluzszy niz dane przy 0x%X" % pos
                return r
            p = pos + 7
            if typ < 0x40:
                r["pcmBytes"] += dlen
            elif typ < 0x7F and dlen >= 5:
                r["pcmBytes"] += struct.unpack_from("<I", data, p + 1)[0]
            elif 0x80 <= typ < 0xC0 and dlen >= 4:
                r["romWriteBytes"] += struct.unpack_from("<I", data, p)[0]
            pos += 7 + dlen
            r["commandCount"] += 1
            continue
        n = cmd_length(op)
        if n is None:
            r["error"] = "nieznana komenda 0x%02X przy 0x%X" % (op, pos)
            return r
        if pos + n > end:
            r["error"] = "komenda 0x%02X ucieta przy 0x%X" % (op, pos)
            return r
        r["commandCount"] += 1
        if op == 0x61:
            r["totalSamples"] += struct.unpack_from("<H", data, pos + 1)[0]
        elif op == 0x62:
            r["totalSamples"] += 735
        elif op == 0x63:
            r["totalSamples"] += 882
        elif 0x70 <= op <= 0x7F:
            r["totalSamples"] += (op & 0xF) + 1
        elif 0x80 <= op <= 0x8F:
            r["totalSamples"] += op & 0xF
        pos += n
        if op == 0x66:
            r["hasEndCommand"] = True
            break
    return r


def scan_file(data: bytes) -> dict:
    if data[:2] == b"\x1f\x8b":
        data = gunzip(data)
    ver = struct.unpack_from("<I", data, 8)[0]
    data_ofs = 0x40
    if ver >= 0x150:
        data_ofs = 0x34 + struct.unpack_from("<I", data, 0x34)[0]
    eof = 4 + struct.unpack_from("<I", data, 4)[0]
    gd3 = struct.unpack_from("<I", data, 0x14)[0]
    end = eof
    if gd3:
        end = min(end, 0x14 + gd3)
    loop = struct.unpack_from("<I", data, 0x1C)[0]
    loop_abs = 0x1C + loop if loop else 0
    return scan_stream(data, data_ofs, end, loop_abs)


# ---------------------------------------------------------------------------
# Muzyka: melodia (public domain), brzmienia ukladow
# ---------------------------------------------------------------------------
# "Oda do radosci" (Beethoven, domena publiczna), (nuta MIDI, dlugosc w cwiercnutach)
MELODY = [
    (64, 1), (64, 1), (65, 1), (67, 1), (67, 1), (65, 1), (64, 1), (62, 1),
    (60, 1), (60, 1), (62, 1), (64, 1), (64, 1.5), (62, 0.5), (62, 2),
]
SCALE = [(60, 1), (62, 1), (64, 1), (65, 1), (67, 1), (69, 1), (71, 1), (72, 2)]


def midi_hz(m: int) -> float:
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


class SnVoice:
    def __init__(self, s: Stream, clock=CLK_SN, ch=0, chip=0):
        self.s, self.clock, self.ch, self.chip = s, clock, ch, chip

    def setup(self):
        self.off()

    def on(self, m):
        n = max(1, min(1023, round(self.clock / (32.0 * midi_hz(m)))))
        self.s.sn(0x80 | (self.ch << 5) | (n & 0xF), self.chip)
        self.s.sn((n >> 4) & 0x3F, self.chip)
        self.s.sn(0x90 | (self.ch << 5) | 0x01, self.chip)

    def off(self):
        self.s.sn(0x90 | (self.ch << 5) | 0x0F, self.chip)


class Ym2612Voice:
    OPS = (0x00, 0x04, 0x08, 0x0C)    # op1, op3, op2, op4 (kolejnosc rejestrow)

    def __init__(self, s: Stream, clock=CLK_YM2612, ch=0, chip=0):
        self.s, self.clock, self.ch, self.chip = s, clock, ch, chip

    def w(self, reg, val):
        self.s.ym2612(0, reg, val, self.chip)

    def setup(self):
        c = self.ch
        self.w(0x22, 0x00)
        self.w(0x27, 0x00)
        for off in self.OPS:
            carrier = off in (0x08, 0x0C)         # algorytm 4: op2 i op4 nosne
            self.w(0x30 + off + c, 0x01)
            self.w(0x40 + off + c, 0x00 if carrier else 0x28)
            self.w(0x50 + off + c, 0x1F)
            self.w(0x60 + off + c, 0x0A)
            self.w(0x70 + off + c, 0x05)
            self.w(0x80 + off + c, 0x26)
            self.w(0x90 + off + c, 0x00)
        self.w(0xB0 + c, (3 << 3) | 4)
        self.w(0xB4 + c, 0xC0)

    def on(self, m):
        hz = midi_hz(m)
        for block in range(8):
            fnum = round(hz * 144 * (1 << 20) / self.clock / (2.0 ** (block - 1)))
            if fnum < 2048:
                break
        self.w(0xA4 + self.ch, (block << 3) | (fnum >> 8))
        self.w(0xA0 + self.ch, fnum & 0xFF)
        self.w(0x28, 0xF0 | self.ch)

    def off(self):
        self.w(0x28, self.ch)


class Ym2151Voice:
    CODES = (0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14)

    def __init__(self, s: Stream, ch=0, chip=0):
        self.s, self.ch, self.chip = s, ch, chip

    def w(self, reg, val):
        self.s.ym2151(reg, val, self.chip)

    def setup(self):
        c = self.ch
        self.w(0x20 + c, 0xC0 | (3 << 3) | 4)
        for i, carrier in enumerate((False, False, True, True)):
            o = i * 8 + c
            self.w(0x40 + o, 0x01)
            self.w(0x60 + o, 0x00 if carrier else 0x20)
            self.w(0x80 + o, 0x1F)
            self.w(0xA0 + o, 0x08)
            self.w(0xC0 + o, 0x04)
            self.w(0xE0 + o, 0x25)

    def on(self, m):
        n = m - 61
        kc = ((4 + n // 12) << 4) | self.CODES[n % 12]
        self.w(0x28 + self.ch, kc)
        self.w(0x30 + self.ch, 0x00)
        self.w(0x08, 0x78 | self.ch)

    def off(self):
        self.w(0x08, self.ch)


class AyVoice:
    def __init__(self, s: Stream, clock=CLK_AY, chip=0):
        self.s, self.clock, self.chip = s, clock, chip

    def setup(self):
        self.s.ay(7, 0x3E, self.chip)
        self.off()

    def on(self, m):
        n = max(1, min(4095, round(self.clock / (16.0 * midi_hz(m)))))
        self.s.ay(0, n & 0xFF, self.chip)
        self.s.ay(1, (n >> 8) & 0x0F, self.chip)
        self.s.ay(8, 0x0F, self.chip)

    def off(self):
        self.s.ay(8, 0x00, self.chip)


class Opl2Voice:
    def __init__(self, s: Stream, clock=CLK_OPL2, chip=0):
        self.s, self.clock, self.chip = s, clock, chip

    def w(self, reg, val):
        self.s.opl2(reg, val, self.chip)

    def setup(self):
        self.w(0x01, 0x20)
        self.w(0x20, 0x21); self.w(0x40, 0x10); self.w(0x60, 0xF4); self.w(0x80, 0x77)
        self.w(0x23, 0x21); self.w(0x43, 0x00); self.w(0x63, 0xF3); self.w(0x83, 0x77)
        self.w(0xC0, 0x04)

    def on(self, m):
        hz = midi_hz(m)
        for block in range(8):
            fnum = round(hz * (1 << (20 - block)) / (self.clock / 72.0))
            if fnum < 1024:
                break
        self.w(0xA0, fnum & 0xFF)
        self.w(0xB0, 0x20 | (block << 2) | (fnum >> 8))

    def off(self):
        self.w(0xB0, 0x00)


class Opl3Voice:
    def __init__(self, s: Stream, clock=CLK_OPL3, chip=0):
        self.s, self.clock, self.chip = s, clock, chip

    def w(self, reg, val, port=0):
        self.s.opl3(port, reg, val, self.chip)

    def setup(self):
        self.w(0x05, 0x01, 1)               # tryb OPL3
        self.w(0x20, 0x21); self.w(0x40, 0x10); self.w(0x60, 0xF4); self.w(0x80, 0x77)
        self.w(0x23, 0x21); self.w(0x43, 0x00); self.w(0x63, 0xF3); self.w(0x83, 0x77)
        self.w(0xC0, 0x34)                  # L+R, FB=2, FM

    def on(self, m):
        hz = midi_hz(m)
        for block in range(8):
            fnum = round(hz * (1 << (20 - block)) / (self.clock / 288.0))
            if fnum < 1024:
                break
        self.w(0xA0, fnum & 0xFF)
        self.w(0xB0, 0x20 | (block << 2) | (fnum >> 8))

    def off(self):
        self.w(0xB0, 0x00)


class OpllVoice:
    def __init__(self, s: Stream, clock=CLK_YM2413, chip=0):
        self.s, self.clock, self.chip = s, clock, chip

    def setup(self):
        self.s.opll(0x30, 0x13, self.chip)          # instrument 1 (skrzypce), glosnosc 3

    def on(self, m):
        hz = midi_hz(m)
        for block in range(8):
            fnum = round(hz * (1 << (19 - block)) / (self.clock / 72.0))
            if fnum < 512:
                break
        self.s.opll(0x10, fnum & 0xFF, self.chip)
        self.s.opll(0x20, 0x30 | (block << 1) | (fnum >> 8), self.chip)   # key on + sustain

    def off(self):
        self.s.opll(0x20, 0x00, self.chip)


def play(s: Stream, voices, melody, beat: int = 8820, gap: int = 441, tail: int = 4410):
    """Gra melodie na liscie glosow (wszystkie rownolegle, ta sama melodia)."""
    if not isinstance(voices, (list, tuple)):
        voices = [voices]
    for v in voices:
        v.setup()
    for m, beats in melody:
        for v in voices:
            v.on(m + getattr(v, "shift", 0))
        s.wait(int(beat * beats) - gap)
        for v in voices:
            v.off()
        s.wait(gap)
    s.wait(tail)


def sine_wave_8bit(n: int, period: int = 20) -> bytes:
    return bytes(int(round(128 + 100 * math.sin(2 * math.pi * i / period))) for i in range(n))


def _main(argv):
    if len(argv) == 3 and argv[1] == "scan":
        with open(argv[2], "rb") as f:
            r = scan_file(f.read())
        for k, val in r.items():
            print("%s: %s" % (k, val))
        return 0 if r["error"] is None else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
