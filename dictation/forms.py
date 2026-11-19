"""变形默写：一屏一个词条，原词 + 各变形各占一行。
原词行的提示是原释义，变形行是「词性（语义标注）」；答对自动跳下一行；答错可再试，
点「不知道」/「下一个」放弃则公布答案记入错题；一轮结束先看错题，错题轮重练至全对。

词条数据：数据目录下的 dictation_forms.json，没有就用自带的 forms_words.json。
格式 [{"meaning": "v. 施加；发挥", "forms": [["exert", "v.", "施加；发挥", ""], ...]}, ...]，
forms 的四项是 单词（双拼写用 / 隔开）、词性、该形式的释义、语义标注。
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field

BUNDLED = os.path.join(os.path.dirname(__file__), "forms_words.json")


@dataclass
class Form:
    word: str
    pos: str
    meaning: str
    note: str


@dataclass
class Entry:
    meaning: str
    forms: list[Form]


def load_entries(path: str | None) -> list[Entry]:
    for p in (path, BUNDLED):
        if not p or not os.path.exists(p):
            continue
        try:
            with open(p, encoding="utf-8-sig") as f:
                data = json.load(f)
            out = [Entry(str(e["meaning"]), [Form(*(str(x) for x in (list(f) + ["", "", ""])[:4])) for f in e["forms"]])
                   for e in data]
            return [e for e in out if e.forms]
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return []


def form_ok(ans: str, word: str) -> bool:
    """双拼写（burned/burnt）写任意一个都算对，不分大小写。"""
    a = ans.strip().lower()
    return bool(a) and any(a == w.strip().lower() for w in word.split("/"))


@dataclass
class Row:
    fi: int
    status: str = "normal"  # normal / wrong / correct / revealed
    text: str = ""


@dataclass
class FormsDrill:
    entries: list[Entry]
    round: int = 1
    queue: list[tuple[int, list[int]]] = field(default_factory=list)  # [(词条号, 形式号列表)]
    round_queue: list[tuple[int, list[int]]] = field(default_factory=list)
    pos: int = 0  # 下一个要出的词条在 queue 里的位置（当前词条 = pos - 1）
    wrong: list[tuple[int, int]] = field(default_factory=list)  # 本轮错题 (词条号, 形式号)，按出错顺序
    rows: list[Row] = field(default_factory=list)
    cur: int = 0  # 当前激活行
    phase: str = "asking"  # asking / preview / done / empty
    serial: int = 0  # 每换一个词条 +1（界面据此重建输入框、移焦点）

    def __post_init__(self) -> None:
        if not self.queue:
            self.reset()

    @property
    def total_forms(self) -> int:
        return sum(len(e.forms) for e in self.entries)

    def _first_queue(self) -> list[tuple[int, list[int]]]:
        return [(wi, list(range(len(e.forms)))) for wi, e in enumerate(self.entries)]

    def reset(self) -> None:
        self.round = 1
        self.queue = self._first_queue()
        self.round_queue = list(self.queue)
        self.wrong = []
        self.pos = 0
        self.rows = []
        self.next_word()

    def current(self) -> tuple[int, list[int]] | None:
        return self.queue[self.pos - 1] if self.phase == "asking" and self.pos > 0 else None

    def entry_done(self) -> bool:
        return self.phase == "asking" and self.cur >= len(self.rows)

    # ------------------------------------------------------------ 动作

    def next_word(self) -> None:
        if not self.entries:
            self.phase = "empty"
            return
        if self.pos >= len(self.queue):  # 本轮结束：有错题先预览，全对则完成
            self.phase = "preview" if self.wrong else "done"
            if self.phase == "done":
                self.rows = []
                self.cur = 0
            return
        _, fis = self.queue[self.pos]
        self.pos += 1
        self.phase = "asking"
        self.rows = [Row(fi) for fi in fis]
        self.cur = 0
        self.serial += 1

    def set_text(self, i: int, text: str) -> None:
        if 0 <= i < len(self.rows) and i == self.cur:
            self.rows[i].text = text

    def submit(self) -> None:
        """提交当前行：答对跳下一行，答错可再试；本词条全部完成后 = 下一个。"""
        if self.phase != "asking":
            return
        if self.entry_done():
            self.next_word()
            return
        r = self.rows[self.cur]
        wi, _ = self.current()
        if form_ok(r.text, self.entries[wi].forms[r.fi].word):
            r.status = "correct"
            self.cur += 1
        else:
            r.status = "wrong"

    def give_up(self) -> None:
        """不知道：公布答案、记入错题、跳下一行。"""
        if self.phase != "asking" or self.entry_done():
            return
        r = self.rows[self.cur]
        wi, _ = self.current()
        if (wi, r.fi) not in self.wrong:  # 读档后重答同一词条时不重复记
            self.wrong.append((wi, r.fi))
        r.text = self.entries[wi].forms[r.fi].word
        r.status = "revealed"
        self.cur += 1

    def next_action(self) -> None:
        """答错时 = 放弃当前行；词条完成时 = 下一个词条。"""
        if self.phase == "asking" and not self.entry_done() and self.rows[self.cur].status == "wrong":
            self.give_up()
        elif self.entry_done():
            self.next_word()

    def start_round(self) -> None:
        """看完错题，开始错题轮（同词条的错题合并成一屏）。"""
        if self.phase != "preview":
            return
        groups: dict[int, list[int]] = {}
        for wi, fi in self.wrong:
            if fi not in groups.setdefault(wi, []):
                groups[wi].append(fi)
        self.round += 1
        self.queue = [(wi, fis) for wi, fis in groups.items()]
        self.round_queue = list(self.queue)
        self.wrong = []
        self.pos = 0
        self.next_word()

    def restart(self) -> None:
        """重开当前轮；全部完成后则回到第 1 轮。"""
        if self.phase == "done":
            self.reset()
            return
        self.queue = list(self.round_queue)
        self.wrong = []
        self.pos = 0
        self.next_word()

    # ------------------------------------------------------------ 存档（当前词条从头再答）

    def to_json(self) -> dict:
        return {"n": len(self.entries), "forms": self.total_forms, "round": self.round,
                "queue": [[wi, fis] for wi, fis in self.queue], "round_queue": [[wi, fis] for wi, fis in self.round_queue],
                "pos": self.pos, "wrong": [list(x) for x in self.wrong], "phase": self.phase}

    def restore(self, d: dict) -> bool:
        try:
            if d["n"] != len(self.entries) or d["forms"] != self.total_forms:
                return False  # 词条换了
            self.round = int(d["round"])
            self.queue = [(int(wi), [int(f) for f in fis]) for wi, fis in d["queue"]]
            self.round_queue = [(int(wi), [int(f) for f in fis]) for wi, fis in d["round_queue"]]
            self.wrong = [(int(a), int(b)) for a, b in d["wrong"]]
            phase = d["phase"]
            if phase == "asking":
                self.pos = max(int(d["pos"]) - 1, 0)
                self.next_word()
            else:
                self.pos = int(d["pos"])
                self.rows = []
                self.cur = 0
                self.phase = phase
            return True
        except (KeyError, TypeError, ValueError):
            self.reset()
            return False
