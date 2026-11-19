"""练习进度：dictation_progress.json（两套练习的出题状态 + 统计 + 变形默写），首次启动把 C 版的
dictation_progress.bin 转过来。"""

from __future__ import annotations

import json
import os
import struct

from .session import Kind, Phase, Session

VERSION = 1

# ---------------------------------------------------------------- C 版 dictation_progress.bin
# struct ProgressFile { u32 magic "DXP1"; u32 sizeof(Session); i32 nwords; i32 dir; { Session s; i32 answered, correct; } drill[2]; }
# Session 里的数组都是 int[2000]（DICT_MAX_WORDS），偏移见 dict_session.h

_MAGIC = 0x31505844
_N = 2000
_OFF = {
    "kind": 0, "phase": 4, "nwords": 8, "queue": 12, "qlen": 12 + 4 * _N, "pos": 16 + 4 * _N, "round": 20 + 4 * _N,
    "round_words": 24 + 4 * _N, "rwlen": 24 + 8 * _N, "wrong": 28 + 8 * _N, "wlen": 28 + 12 * _N,
    "group_size": 32 + 12 * _N, "group": 36 + 12 * _N, "gstart": 40 + 12 * _N, "gend": 44 + 12 * _N,
    "seen": 48 + 12 * _N, "restarts": 52 + 12 * _N, "rng": 56 + 12 * _N, "pool": 64 + 12 * _N,
    "npool": 64 + 16 * _N, "hits": 68 + 16 * _N, "fumbled": 68 + 20 * _N, "retired": 72 + 20 * _N,
    "joined": 76 + 20 * _N,
}
_SIZE = 80 + 20 * _N                               # 2026-11-14 起（有「对满 15 次移出」）
_SIZE_V2 = (_OFF["npool"] + 4 + 7) & ~7            # 2026-11-07 起（有错词池），末尾带对齐填充
_SIZE_OLD = _OFF["pool"]                           # 更早
_HEAD = 16


def _session_from_c(rec: bytes, size: int) -> tuple[Session, int, int]:
    def i32(name: str) -> int:
        return struct.unpack_from("<i", rec, _OFF[name])[0]

    def arr(name: str, n: int) -> list[int]:
        n = max(0, min(n, _N))
        return list(struct.unpack_from(f"<{n}i", rec, _OFF[name]))

    s = Session()
    s.kind = Kind(i32("kind"))
    s.phase = Phase(i32("phase"))
    s.nwords = i32("nwords")
    s.queue = arr("queue", i32("qlen"))
    s.pos = i32("pos")
    s.round = i32("round")
    s.round_words = arr("round_words", i32("rwlen"))
    s.wrong = arr("wrong", i32("wlen"))
    s.group_size = i32("group_size") or 20
    s.group = i32("group")
    s.gstart = i32("gstart")
    s.gend = i32("gend")
    s.seen = i32("seen")
    s.restarts = i32("restarts")
    s.rng = struct.unpack_from("<Q", rec, _OFF["rng"])[0] or s.rng
    if size >= _SIZE_V2:
        s.pool = arr("pool", i32("npool"))
    if size >= _SIZE:
        s.hits = {i: h for i, h in enumerate(arr("hits", min(s.nwords, _N))) if h}
        s.fumbled = bool(i32("fumbled"))
    answered, correct = struct.unpack_from("<2i", rec, size)
    return s, answered, correct


def load_c(path: str, nwords: int) -> dict | None:
    """读 C 版进度；格式不认 / 词表变短了（换了词表）返回 None。"""
    try:
        with open(path, "rb") as f:
            buf = f.read()
    except OSError:
        return None
    if len(buf) < _HEAD:
        return None
    magic, ss, nw, direction = struct.unpack_from("<IIii", buf, 0)
    if magic != _MAGIC or ss not in (_SIZE, _SIZE_V2, _SIZE_OLD) or len(buf) != _HEAD + 2 * (ss + 8):
        return None
    if not 0 < nw <= nwords or direction not in (0, 1):
        return None
    drills = []
    for i in range(2):
        rec = buf[_HEAD + i * (ss + 8):_HEAD + (i + 1) * (ss + 8)]
        s, answered, correct = _session_from_c(rec, ss)
        drills.append({"session": s.to_json(), "answered": answered, "correct": correct})
    return {"version": VERSION, "nwords": nw, "dir": direction, "drills": drills}


# ---------------------------------------------------------------- JSON


def save(path: str, data: dict) -> bool:
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
        os.replace(tmp, path)
        return True
    except OSError:
        return False


def load(path: str, legacy: str, nwords: int) -> dict | None:
    """读 JSON 进度；没有就试着转 C 版的 .bin（转成功后写成 JSON，.bin 留着不动）。"""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) and data.get("version") == VERSION else None
    except (OSError, ValueError):
        pass
    data = load_c(legacy, nwords)
    if data is not None:
        save(path, data)
    return data
