"""Generuje ziarna startowe dla celow plikowych (settings, playlist_json, library_cache, m3u).

Uzycie: python -I fuzz/make_seeds.py <katalog_wyjsciowy>
(powstaja podkatalogi settings/, playlist_json/, library_cache/, m3u/; ziarna VGM biora sie z tests/data).
"""
import json
import os
import sys

BS = chr(92)  # backslash (jawnie, zeby uniknac pomylek z sekwencjami ucieczki)
base = sys.argv[1]


def w(d, name, data):
    p = os.path.join(base, d)
    os.makedirs(p, exist_ok=True)
    open(os.path.join(p, name), 'wb').write(data if isinstance(data, bytes) else data.encode('utf-8'))


UNC = BS * 2 + 'srv' + BS + 'share' + BS
DEVPATH = BS * 2 + '?' + BS + 'C:' + BS + 'x.vgm'

S = {
    'empty': '{}',
    'min': '{"schemaVersion":1}',
    'full': json.dumps({
        "schemaVersion": 1,
        "audio": {"device": "Głośniki (Realtek)", "bufferMs": 80, "sampleRate": 44100},
        "playback": {"loopCount": 3, "fadeMs": 2000, "volume": 0.5, "nonLoopEnd": "stop",
                     "cores": [{"type": "ym2612", "coreId": "nuked"}], "repeat": "all"},
        "ui": {"language": "en", "gd3Language": "jp", "theme": "light", "fontScale": 1.5, "scopeMode": "master",
               "trackerView": "scopes", "rowTicks": 735, "scopeBar": "large", "showButtonHints": False},
        "input": {"bindings": [{"action": "confirm", "source": "key", "code": 13},
                               {"action": "confirm", "source": "pad", "code": 0}],
                  "repeatDelayMs": 300, "repeatRateMs": 50},
        "library": {"allowNetworkPaths": True},
        "session": {"trackPath": "C:/Muzyka/zażółć/x.vgm", "positionMs": 12345}}, ensure_ascii=False),
    'unc': '{"session":{"trackPath":"//serwer/udzial/a.vgm","positionMs":1}}',
    'url': '{"session":{"trackPath":"http://x/y.vgm"}}',
    'devpath': json.dumps({"session": {"trackPath": DEVPATH}}),
    'clamp': '{"audio":{"bufferMs":1,"sampleRate":100},"playback":{"volume":7.5,"loopCount":5000,"fadeMs":999999},'
             '"ui":{"fontScale":0.01,"rowTicks":5},"input":{"repeatDelayMs":1,"repeatRateMs":99999}}',
    'badtypes': '{"audio":{"bufferMs":"x","device":5},"playback":{"volume":"g","nonLoopEnd":"boom","cores":{"x":1}},'
                '"ui":{"language":"de","theme":3},"input":{"bindings":[{"action":"nonsense","source":"key","code":1},'
                '5,null]},"session":"oops"}',
    'future': '{"schemaVersion":2}',
    'bom': '\ufeff{"ui":{"theme":"light"}}',
    'deep': '{"a":[[[[[[[[1]]]]]]]]}',
    'bigint': '{"audio":{"bufferMs":4294967297},"session":{"positionMs":18446744073709551615}}',
}
for k, v in S.items():
    w('settings', k + '.json', v)

P = {
    'empty': '{}',
    'min': '{"schemaVersion":1,"playlists":[]}',
    'full': json.dumps({"schemaVersion": 1, "active": 1, "playlists": [
        {"name": "Gry NES", "cursor": 1, "entries": [
            {"path": "C:/Muzyka/NES/a.vgm", "title": "Intro", "durationMs": 61000},
            {"path": "C:/Muzyka/zażółć/b.vgz", "title": "", "durationMs": 0}]},
        {"name": "Pusta", "cursor": -1, "entries": []}]}, ensure_ascii=False),
    'unc': json.dumps({"playlists": [{"name": "n", "entries": [
        {"path": "//srv/share/a.vgm"}, {"path": UNC + "b.vgm"}]}]}),
    'urls': json.dumps({"playlists": [{"name": "n", "entries": [
        {"path": "http://x/a.vgm"}, {"path": "file:///C:/a.vgm"}, {"path": DEVPATH},
        {"path": BS * 2 + "." + BS + "pipe" + BS + "x"}]}]}),
    'ctrl': '{"playlists":[{"name":"n","entries":[{"path":"a.vgm","title":"a\\u0000b\\u202e\\u2066c",'
            '"durationMs":99999999999}]}]}',
    'dup': '{"playlists":[{"name":"A","entries":[]},{"name":"a","entries":[]},{"name":"","entries":[]}],"active":9}',
    'future': '{"schemaVersion":2,"playlists":[]}',
    'cursor': '{"playlists":[{"name":"x","cursor":99,"entries":[{"path":"a.vgm"}]}]}',
}
for k, v in P.items():
    w('playlist_json', k + '.json', v)

C = {
    'empty': '{}',
    'min': '{"schemaVersion":1,"items":[]}',
    'full': json.dumps({"schemaVersion": 1, "items": [
        {"path": "C:/Muzyka/a.vgm", "size": 1234, "mtime": 133500000000000000, "durationMs": 61000,
         "loopMs": 20000, "gd3": True, "trackEn": "Intro", "trackJp": "イントロ", "gameEn": "G",
         "gameJp": "ゲーム", "systemEn": "NES", "systemJp": "ファミコン", "authorEn": "A", "authorJp": "作者"},
        {"path": "C:/Muzyka/b.vgz", "size": 5, "mtime": 1, "durationMs": 0, "loopMs": 0}]}, ensure_ascii=False),
    'unc': '{"schemaVersion":1,"items":[{"path":"//srv/share/a.vgm","size":1,"mtime":1,"durationMs":1}]}',
    'dupkeys': '{"schemaVersion":1,"items":[{"path":"C:/A.vgm","size":1,"mtime":1},'
               '{"path":"c:/a.vgm","size":2,"mtime":2}]}',
    'huge': '{"schemaVersion":1,"items":[{"path":"a","size":18446744073709551615,"mtime":-1,'
            '"durationMs":99999999999999,"loopMs":1e30}]}',
    'future': '{"schemaVersion":9,"items":[]}',
}
for k, v in C.items():
    w('library_cache', k + '.json', v)

M = {
    'basic': '#EXTM3U\r\n#EXTINF:61,Intro\r\nmusic/a.vgm\r\n#EXTINF:-1,Bez czasu\r\nb.vgz\r\n',
    'utf8bom': b'\xef\xbb\xbf#EXTM3U\n#EXTINF:10,Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87\n'
               b'za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87/x.vgm\n',
    'cp1250': b'#EXTM3U\n#EXTINF:5,Za\xbf\xf3\xb3\xe6\nza\xbf\xf3\xb3\xe6.vgm\n',
    'traversal': '../../etc/a.vgm\n' + '..' + BS + '..' + BS + 'Windows' + BS + 'b.vgm\n'
                 'ok/../../x.vgm\nsub/./c.vgm\n',
    'abs': 'C:' + BS + 'vgmplay_fuzz_root' + BS + 'a.vgm\nC:/Windows/System32/x.vgm\n/rooted.vgm\nD:rel.vgm\n',
    'urls': 'http://x/a.vgm\nhttps://x/b.vgm\nfile:///C:/c.vgm\n' + UNC + 'd.vgm\n//srv/share/e.vgm\n'
            + DEVPATH + '\n' + BS * 2 + '.' + BS + 'pipe' + BS + 'g\nftp://h/i.vgm\n',
    'weird': '#EXTINF:abc,\n#EXTINF:99999999999999999999,t\n#EXTINF:\n#EXTINF:5\n\n\n   \n\t\nCON\nNUL.vgm\nCOM1\n'
             'a:b.vgm\n"quoted.vgm"\n',
    'ctrl': '#EXTINF:5,a\x00b\u202ec\na\x00b.vgm\n',
    'cr': '#EXTM3U\r#EXTINF:5,t\rx.vgm\r',
}
for k, v in M.items():
    w('m3u', k + '.m3u', v)
