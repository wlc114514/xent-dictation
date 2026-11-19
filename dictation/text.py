"""文本工具：判分规则、义项拆分 / 合并、首字母提示。纯函数，不碰界面。"""

from __future__ import annotations

MAX_SENSES = 16  # 已答出的义项用位掩码记

_SEPS = set(",;/、，；")
_OPEN = set("([（【")
_CLOSE = set(")]）】")
_NEG = "不非无未没反否莫勿"
# 含否定字但不表否定的常用词（「几乎」=「差不多」、「重要的」≈「非常重要的」）
_NOT_NEG = ("差不多", "不得不", "不错", "不断", "不久", "非常", "无论", "未来", "反而", "反应", "反映", "莫非")


def is_cjk(ch: str) -> bool:
    return "一" <= ch <= "鿿"


def cjk(s: str) -> str:
    """只留汉字（U+4E00–U+9FFF）。"""
    return "".join(ch for ch in s if is_cjk(ch))


def has_cjk(s: str) -> bool:
    return any(is_cjk(ch) for ch in s)


def is_blank(s: str) -> bool:
    """没有 0-9 / a-z / 汉字 就算空白（纯标点也算）。"""
    return not any(ch.isascii() and ch.isalnum() or is_cjk(ch) for ch in s)


def _neg_mask(s: str) -> int:
    """出现过哪些否定字（位掩码）。两边否定字不一样时不按包含判：「不合理」包含「合理」，但意思相反。"""
    mask, i = 0, 0
    while i < len(s):
        skip = next((len(w) for w in _NOT_NEG if s.startswith(w, i)), 0)
        if skip:
            i += skip
            continue
        k = _NEG.find(s[i])
        if k >= 0:
            mask |= 1 << k
        i += 1
    return mask


def _split_cjk_senses(meaning: str) -> list[str]:
    """按 , ; / 、 ， ； 拆，每段只留汉字，空段丢掉（判分用，不管括号）。"""
    out, cur = [], []
    for ch in meaning:
        if ch in _SEPS:
            if cur:
                out.append("".join(cur))
                cur = []
        elif is_cjk(ch):
            cur.append(ch)
    if cur:
        out.append("".join(cur))
    return out


def meaning_match(ans: str, meaning: str) -> bool:
    """「单词 → 释义」宽松判分。"""
    a, m = cjk(ans), cjk(meaning)
    if not a:
        return False
    if not m:
        la, lm = ans.lower(), meaning.lower()
        return bool(la) and la in lm
    if a == m:
        return True
    neg = _neg_mask(a)
    if len(a) >= 2 and neg == _neg_mask(m) and a in m:
        return True
    senses = _split_cjk_senses(meaning)
    for s in senses:
        if a == s:
            return True
        if neg != _neg_mask(s):
            continue
        if len(s) >= 2 and s in a:
            return True
        if len(a) >= 2 and a in s:
            return True
    # 字的子集：答案里的字都在释义里出现过，且不短于最短义项的一半
    if len(a) >= 2 and neg == _neg_mask(m) and set(a) <= set(m):
        shortest = min([len(m)] + [len(s) for s in senses])
        if len(a) * 2 >= shortest:
            return True
    return False


def split_senses(meaning: str, limit: int = MAX_SENSES) -> list[str]:
    """拆义项：、，,；;/ 分隔，两个汉字之间的空格也算；括号里的不拆，重复的只留一个。"""
    out: list[str] = []

    def put(piece: str) -> None:
        piece = piece.strip(" \t")
        if piece and not is_blank(piece) and piece not in out and len(out) < limit:
            out.append(piece)

    depth, prev, start = 0, "", 0
    for i, ch in enumerate(meaning):
        if ch in _OPEN:
            depth += 1
        elif ch in _CLOSE and depth > 0:
            depth -= 1
        split = depth == 0 and ch in _SEPS
        if depth == 0 and ch in " \t" and is_cjk(prev):
            # 空格只在两个汉字之间才算分隔：「复杂的 复合体」拆，「使 (决定) 转向」不拆
            rest = meaning[i:].lstrip(" \t")
            split = bool(rest) and is_cjk(rest[0])
        if split:
            put(meaning[start:i])
            start = i + 1
        if ch not in " \t":
            prev = "" if split else ch
    put(meaning[start:])
    return out


def senses_mark(ans: str, senses: list[str], got: int) -> tuple[int, int]:
    """答案（可一次写多个）逐条对还没答出的义项；返回 (新的位掩码, 新答出的个数)。"""
    fresh = 0
    for part in split_senses(ans):
        for k, sense in enumerate(senses):
            if got & (1 << k) or not meaning_match(part, sense):
                continue
            got |= 1 << k
            fresh += 1
            break
    return got, fresh


def senses_merge(senses: list[str], links: set[tuple[int, int]]) -> str | None:
    """合并意思相同的义项：links 里的 (i, j) 表示同义，按连通关系合并（a≈b、b≈c → 三条合一）。
    同一组写成「第一条（其余、…）」，组之间用「、」，保持原来的先后顺序。没有可合并的返回 None。"""
    root = list(range(len(senses)))

    def find(x: int) -> int:
        while root[x] != x:
            x = root[x]
        return x

    merged = False
    for i, j in sorted(links):
        a, b = find(i), find(j)
        if a != b:
            root[max(a, b)] = min(a, b)  # 组以最靠前的一条为首
            merged = True
    if not merged:
        return None
    groups = []
    for i, sense in enumerate(senses):
        if find(i) != i:
            continue
        rest = [senses[k] for k in range(i + 1, len(senses)) if find(k) == i]
        groups.append(sense + ("（" + "、".join(rest) + "）" if rest else ""))
    return "、".join(groups)


def _answer_norm(s: str) -> str:
    return " ".join(s.replace(".", "").lower().split())


def answer_eq(ans: str, word: str) -> bool:
    """「释义 → 单词」判对：不分大小写、忽略 '.'、多余空格不计。"""
    a = _answer_norm(ans)
    return bool(a) and a == _answer_norm(word)


def masked_display(word: str) -> str:
    """首字母提示：每个词露出前半段，其余挖空，如 "s w _ _ _"；短语词与词之间留宽空。"""
    parts = []
    for w in word.split(" "):
        if not w:
            continue
        hint = max((len(w) + 1) // 2 - 1, 0)
        parts.append(" ".join(ch if i < hint or not (ch.isalnum() or not ch.isascii()) else "_"
                              for i, ch in enumerate(w)))
    return "   ".join(parts)
