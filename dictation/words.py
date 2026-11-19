"""词表：读写 dictation_extra.json（[["word", "pos", "meaning"], ...]）、批量导入、去标注。"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field

from .text import has_cjk

MAX_WORDS = 20000


@dataclass
class Word:
    word: str
    pos: str
    meaning: str


@dataclass
class AddStat:
    added: int = 0
    dup: int = 0
    bad: int = 0
    err: str = ""  # 非空 = 整批被拒


# ---------------------------------------------------------------- 读写


def _top_level_end(text: str) -> int:
    """顶层数组收尾 ']' 的位置（按 JSON 字符串语义，释义里的 ']' 不会认错）；找不到返回 -1。"""
    depth, in_str, esc = 0, False, False
    for i, c in enumerate(text):
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth <= 0:
                return i
    return -1


def _entry_line(w: Word) -> str:
    return "  " + json.dumps([w.word, w.pos, w.meaning], ensure_ascii=False)


@dataclass
class WordList:
    path: str
    words: list[Word] = field(default_factory=list)

    @classmethod
    def load(cls, path: str) -> WordList:
        wl = cls(path)
        try:
            with open(path, encoding="utf-8-sig") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return wl
        for row in data if isinstance(data, list) else []:
            if isinstance(row, list) and len(row) >= 3 and all(isinstance(x, str) for x in row[:3]):
                if row[0] and row[2] and len(wl.words) < MAX_WORDS:
                    wl.words.append(Word(row[0], row[1], row[2]))
        return wl

    def __len__(self) -> int:
        return len(self.words)

    def save(self) -> bool:
        """整表按标准格式写回（改了释义后用；手写的排版不保留）。"""
        body = ",\n".join(_entry_line(w) for w in self.words)
        return _write(self.path, "[\n" + body + ("\n" if body else "") + "]\n")

    def append(self, items: list[Word]) -> bool:
        """保留文件原有内容（包括手写的格式），只往顶层数组末尾插。"""
        if not items:
            return True
        try:
            with open(self.path, encoding="utf-8-sig") as f:
                old = f.read()
        except OSError:
            old = ""
        out = "[\n"
        end = _top_level_end(old)
        if end >= 0:
            body = old[:end].rstrip(" \t\r\n").lstrip(" \t\r\n")
            body = body[1:] if body.startswith("[") else body
            body = body.lstrip("\r\n")  # 保留第一项原有的缩进
            if body.strip():
                out += body + ",\n"
        out += ",\n".join(_entry_line(w) for w in items) + "\n]\n"
        return _write(self.path, out)

    def clear(self) -> None:
        try:
            os.remove(self.path)
        except OSError:
            pass
        self.words.clear()

    def has(self, word: str) -> bool:
        lw = word.lower()
        return any(w.word.lower() == lw for w in self.words)


def _write(path: str, text: str) -> bool:
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
        return True
    except OSError:
        return False


# ---------------------------------------------------------------- 词性 / 释义拆分

# 长的在前，避免短词先命中
_POS_TOKENS = ("prep&adv", "n&adj", "adj&n", "n&v", "v&n", "adj&adv", "adj&v", "conj", "adj", "adv", "prep", "n", "v")
_POS_BARE = {"n", "v", "adj", "adv", "prep", "conj"}
_CJK_PUNCT = "，。；：、（】"


def _alnum(c: str) -> bool:
    return c.isascii() and c.isalnum()


def _pos_try(s: str, i: int, begin: int = 0) -> tuple[int, str]:
    """在 s[i:] 试配一个词性标记（左右是单词边界），返回 (消耗的字符数, 标记)；不是词性返回 (0, "")。"""
    for tok in _POS_TOKENS:
        if not s.startswith(tok, i):
            continue
        j = i + len(tok)
        if j < len(s) and _alnum(s[j]):
            continue
        if i > begin and _alnum(s[i - 1]):
            continue
        while j < len(s) and s[j] == " ":
            j += 1
        dotted = j < len(s) and s[j] == "."
        if dotted:
            j += 1
        return j - i, tok + ("." if dotted or tok in _POS_BARE else "")
    return 0, ""


def split_pos_meaning(rest: str) -> tuple[str, str]:
    """"n. 过程 v. 处理" → ("n. v.", "过程 处理")。"""
    toks: list[str] = []
    buf: list[str] = []
    i = 0
    while i < len(rest):
        used, tok = _pos_try(rest, i)
        if used:
            if tok not in toks:
                toks.append(tok)
            i += used
            continue
        buf.append(" " if rest[i] == "&" else rest[i])
        i += 1
    out: list[str] = []
    pending = False
    for c in "".join(buf):
        if c in " \t":
            pending = bool(out)
            continue
        if pending and c not in _CJK_PUNCT:
            out.append(" ")
        pending = False
        out.append(c)
    return " ".join(toks), "".join(out)


# ---------------------------------------------------------------- 单词表上的标注（「忘记」「少词义」…）

# 标注 → key：其他义项里也有 key，说明这个词本身就是这个意思，不删
_NOTES = {
    "忘": "忘", "忘记": "忘", "忘了": "忘", "又忘": "忘", "又忘了": "忘", "遗忘": "忘",
    "记不住": "记", "没记住": "记", "不记得": "记", "记不清": "记", "想不起": "想不起", "想不起来": "想不起",
    "少词义": "词义", "缺词义": "词义", "漏词义": "词义", "少了词义": "词义", "词义不全": "词义",
    "少释义": "释义", "缺释义": "释义", "漏释义": "释义",
    "少义": "少义", "漏义": "漏义", "缺义": "缺义", "少意思": "意思", "漏意思": "意思",
}

_BRACKETS = (("（", "）"), ("(", ")"), ("【", "】"), ("[", "]"))


def _note_key(token: str) -> str | None:
    for o, c in _BRACKETS:
        if len(token) > len(o) + len(c) and token.startswith(o) and token.endswith(c):
            token = token[len(o):-len(c)]
            break
    return _NOTES.get(token)


def strip_notes(meaning: str) -> str:
    """去掉「忘记 获利丰厚的」「少词义 发展 …、发展」这类标注；只删跟其他义项不沾边的。"""
    spans = []  # [start, end, key, delete]
    for m in re.finditer(r"[^ \t、，,;；/]+", meaning):
        spans.append([m.start(), m.end(), _note_key(m.group()), False])
    real = [meaning[s:e] for s, e, key, _ in spans if key is None]
    any_del = False
    for t in spans:
        if t[2] is not None and real and not any(t[2] in r for r in real):
            t[3] = any_del = True
    if not any_del:
        return meaning
    # 标注后面紧跟的是补记的义项，后面已经有了就一起删
    for i in range(len(spans) - 1):
        if not spans[i][3] or spans[i + 1][2] is not None:
            continue
        nxt = meaning[spans[i + 1][0]:spans[i + 1][1]]
        if any(meaning[s:e] == nxt for s, e, _, _ in spans[i + 2:]):
            spans[i + 1][3] = True
    gone = [False] * len(meaning)
    for i, (s, e, _, dele) in enumerate(spans):
        if not dele:
            continue
        a, b = s, spans[i + 1][0] if i + 1 < len(spans) else e
        if i + 1 >= len(spans):  # 最后一个：去掉前面的分隔符
            k = i - 1
            while k >= 0 and spans[k][3]:
                k -= 1
            a = spans[k][1] if k >= 0 else 0
        for p in range(a, b):
            gone[p] = True
    out = "".join(c for c, g in zip(meaning, gone) if not g)
    return out.lstrip(" \t、，,;；/")  # 开头可能剩下分隔符


# ---------------------------------------------------------------- 批量导入

_ENTRY_SEPS = " \t=:-|,;/"


def _is_zh(c: str) -> bool:
    """汉字及中文 / 全角标点：单行式里英文部分到此为止。"""
    return "　" <= c <= "鿿" or "＀" <= c <= "￯"


def _squash(s: str) -> str:
    return " ".join(s.split())


def split_entry(line: str) -> tuple[str, str] | None:
    """切出行首的单词或短语，返回 (单词, 词性+释义)：
    有 Tab → Tab 前整段是单词；有中文 → 第一个中文字符前是单词，再剥掉末尾的词性；都没有 → 第一个空格前。"""
    p = len(line) - len(line.lstrip(" \t"))
    if p >= len(line):
        return None
    e = line.find("\t", p)
    if e < 0:
        zh = next((i for i in range(p, len(line)) if _is_zh(line[i])), -1)
        if zh >= 0:
            e = zh
            while True:
                while e > p and line[e - 1] in _ENTRY_SEPS:
                    e -= 1
                t = e
                while t > p and line[t - 1] not in _ENTRY_SEPS:
                    t -= 1
                if t == p or _pos_try(line, t, p)[0] != e - t:
                    break
                e = t  # 末尾这个是词性，留给释义
        else:
            e = line.find(" ", p)
            e = len(line) if e < 0 else e
    rest = e
    while e > p and line[e - 1] in _ENTRY_SEPS:
        e -= 1
    word = _squash(line[p:e])
    while rest < len(line) and line[rest] in _ENTRY_SEPS:
        rest += 1
    return (word, line[rest:]) if rest < len(line) and word else None


def parse_add_text(wl: WordList, text: str) -> tuple[list[Word], AddStat]:
    """多行文本 → 新词（不写盘）。三种格式：
    ① 每行「单词 词性 释义」  ② 英文、中文一行一行交替  ③ 先全部英文，再全部中文，数量一致。"""
    st = AddStat()
    items: list[Word] = []
    if not text:
        st.err = "没有内容"
        return items, st
    lines = [ln.strip(" \t") for ln in re.split(r"\r\n|\r|\n", text)]
    lines = [ln for ln in lines if ln]
    if not lines:
        st.err = "没有可读的行"
        return items, st

    seen = {w.word.lower() for w in wl.words}

    def take(word: str, pos_meaning: str) -> None:
        pos, mn = split_pos_meaning(pos_meaning)
        mn = strip_notes(mn)
        if not mn:
            st.bad += 1
        elif word.lower() in seen:
            st.dup += 1
        else:
            seen.add(word.lower())
            items.append(Word(word, pos, mn))

    zh = [has_cjk(ln) for ln in lines]
    any_ascii = not all(zh)
    alternate = any_ascii and len(lines) % 2 == 0 and all(z == (i % 2 == 1) for i, z in enumerate(zh))
    if alternate and len(lines) > 2:
        for i in range(0, len(lines), 2):
            take(_squash(lines[i]), lines[i + 1])
    elif any_ascii:
        eng_n = zh.index(True) if True in zh else len(lines)
        if not all(zh[eng_n:]):
            st.err = "格式混杂：英文行和中文行交错，请检查后重试"
            return [], st
        if eng_n != len(lines) - eng_n:
            st.err = f"英文 {eng_n} 个、中文 {len(lines) - eng_n} 行，数量不一致"
            return [], st
        for i in range(eng_n):
            take(_squash(lines[i]), lines[eng_n + i])
    else:
        for ln in lines:
            got = split_entry(ln)
            if got is None:
                st.bad += 1
            else:
                take(*got)
    return items, st


def add_text(wl: WordList, text: str) -> AddStat:
    """解析并追加进词表（内存 + 文件）。"""
    items, st = parse_add_text(wl, text)
    if st.err or not items:
        return st
    room = MAX_WORDS - len(wl.words)
    if room <= 0:
        st.err = f"词表已满（上限 {MAX_WORDS} 词）"
        return st
    items = items[:room]
    if not wl.append(items):
        st.err = f"写入词表失败：{wl.path}"
        return st
    wl.words.extend(items)
    st.added = len(items)
    return st
