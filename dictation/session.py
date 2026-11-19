"""出题状态机：普通轮次模式 + 无尽模式。只管「出哪个词、现在处于什么阶段」，不碰界面和判分。

── 普通模式（轮次）
  第 1 轮按词表顺序出全部词；答错可再试，点「下一个」公布答案并记为错词；
  一轮结束有错词 → 错词预览 → 下一轮只默错词；全对 → 完成。

── 无尽模式
  词表按顺序每 20 个分一组，一组一组过：
  · 组内按原顺序出词；提交答错可以继续重试；只有点「不知道」才公布答案，按钮变成「重新开始」；
  · 点「重新开始」→ 本组所有见过的词（之前答对的 + 这次错的）打乱顺序排在最前，没见过的词保持原顺序跟在后面；
  · 本组一次不错地答到最后一个 → 本组通过，进入下一组（上一组的词不再出现）；
  · 一个词一次答对（没先提交答错）满 15 次 → 移出本组，从后面的词里按顺序补一个进来，本组保持 20 个；
    词用完了就不补，本组变少，全部移出也算本组过关；「不知道」不清零次数，从第 1 组重新开始才清零；
  · 也可以只练一部分词（普通模式一轮的错词）：start_endless_pool 把这些词打乱后同样每 20 个一组。
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from enum import IntEnum

GROUP_SIZE = 20
RETIRE_HITS = 15
_MASK = (1 << 64) - 1


class Kind(IntEnum):
    ROUNDS = 0
    ENDLESS = 1


class Phase(IntEnum):
    EMPTY = 0     # 词表为空
    ASKING = 1    # 等待作答
    WRONG = 2     # 答错了，可再试
    CORRECT = 3   # 答对了，等「下一个」
    REVEALED = 4  # 已公布答案，等「下一个」（无尽模式里是「重新开始」）
    PREVIEW = 5   # 普通模式：本轮结束，错词预览
    DONE = 6      # 全部完成


@dataclass
class Session:
    kind: Kind = Kind.ROUNDS
    phase: Phase = Phase.EMPTY
    nwords: int = 0
    queue: list[int] = field(default_factory=list)  # 当前出题顺序（词表下标）
    pos: int = 0
    # 普通模式
    round: int = 1
    round_words: list[int] = field(default_factory=list)  # 本轮的词（「重新开始」恢复用）
    wrong: list[int] = field(default_factory=list)  # 本轮错词（按出错顺序）
    # 无尽模式
    group_size: int = GROUP_SIZE
    group: int = 0
    gstart: int = 0  # 当前组从总顺序第 gstart 个开始；[gend, 总数) 是还没进过组的词
    gend: int = 0
    seen: int = 0  # queue 前 seen 个是本组已经见过的
    restarts: int = 0
    pool: list[int] = field(default_factory=list)  # 只练一部分词（已打乱）；空 = 整个词表
    hits: dict[int, int] = field(default_factory=dict)  # 词表下标 → 一次答对的次数
    fumbled: bool = False  # 当前这题答错过或已经计过次数
    retired: int = -1  # 刚才 next() 移出的词
    joined: int = -1  # 刚才 next() 补进来的词
    rng: int = 0x9E3779B97F4A7C15

    # ------------------------------------------------------------ 随机数：xorshift64*，无偏取范围

    def seed(self, seed: int) -> None:
        self.rng = (seed & _MASK) or 0x9E3779B97F4A7C15

    def _rand(self) -> int:
        x = self.rng
        x ^= x >> 12
        x ^= (x << 25) & _MASK
        x ^= x >> 27
        self.rng = x
        return (x * 0x2545F4914F6CDD1D) & _MASK

    def _below(self, n: int) -> int:
        limit = _MASK - _MASK % n
        while True:
            r = self._rand()
            if r < limit:
                return r % n

    def _shuffle(self, a: list[int], n: int | None = None) -> None:
        n = len(a) if n is None else n
        for i in range(n - 1, 0, -1):
            j = self._below(i + 1)
            a[i], a[j] = a[j], a[i]

    # ------------------------------------------------------------ 公共

    @property
    def qlen(self) -> int:
        return len(self.queue)

    @property
    def wlen(self) -> int:
        return len(self.wrong)

    def _begin(self) -> None:
        self.pos = 0
        self.fumbled = False
        self.phase = Phase.ASKING if self.queue else Phase.EMPTY

    def current(self) -> int:
        """当前题目的词表下标；空 / 预览 / 完成时返回 -1。"""
        if self.phase in (Phase.ASKING, Phase.WRONG, Phase.CORRECT, Phase.REVEALED):
            return self.queue[self.pos]
        return -1

    def _mark_wrong(self, w: int) -> None:
        if w not in self.wrong:
            self.wrong.append(w)

    # ------------------------------------------------------------ 普通模式

    def _rounds_load(self, words: list[int], rnd: int) -> None:
        self.kind = Kind.ROUNDS
        self.round = rnd
        self.round_words = list(words)
        self.queue = list(words)
        self.wrong = []
        self._begin()

    def start_rounds(self, nwords: int) -> None:
        self.nwords = nwords
        self._rounds_load(list(range(nwords)), 1)

    def next_round(self) -> None:
        if self.kind == Kind.ROUNDS and self.phase == Phase.PREVIEW:
            self._rounds_load(self.wrong, self.round + 1)

    def _rounds_advance(self) -> None:
        self.pos += 1
        if self.pos < self.qlen:
            self.phase = Phase.ASKING
            return
        self.pos = self.qlen - 1
        self.phase = Phase.PREVIEW if self.wrong else Phase.DONE

    # ------------------------------------------------------------ 无尽模式

    def endless_total(self) -> int:
        return len(self.pool) if self.pool else self.nwords

    def _endless_word(self, i: int) -> int:
        return self.pool[i] if self.pool else i

    def _load_group(self, g: int, start: int) -> None:
        self.group = g
        self.gstart = start
        self.gend = min(start + self.group_size, self.endless_total())
        self.seen = 0
        self.restarts = 0
        self.queue = [self._endless_word(i) for i in range(self.gstart, self.gend)]
        self._begin()

    def mastered(self) -> int:
        return sum(1 for w, h in self.hits.items() if w < self.nwords and h >= RETIRE_HITS)

    def endless_passed(self) -> int:
        if self.kind != Kind.ENDLESS:
            return 0
        return self.endless_total() if self.phase == Phase.DONE else self.gend - self.qlen

    def total_hits(self) -> int:
        return sum(h for w, h in self.hits.items() if w < self.nwords)

    def _retire(self) -> bool:
        w = self.queue[self.pos]
        if self.hits.get(w, 0) < RETIRE_HITS:
            return False
        self.seen = max(self.seen, self.pos + 1) - 1  # 它本来算见过
        del self.queue[self.pos]
        self.retired = w
        total = self.endless_total()
        while self.qlen < self.group_size and self.gend < total:
            nw = self._endless_word(self.gend)
            self.gend += 1
            self.queue.append(nw)  # 没见过，排最后
            self.joined = nw
        return True

    def ngroups(self) -> int:
        if self.kind != Kind.ENDLESS:
            return 0
        rest = self.endless_total() - self.gend
        return self.group + 1 + ((rest + self.group_size - 1) // self.group_size if rest > 0 else 0)

    def start_endless(self, nwords: int) -> None:
        self.start_endless_pool(nwords, [])

    def start_endless_pool(self, nwords: int, words: list[int]) -> None:
        self.kind = Kind.ENDLESS
        self.nwords = nwords
        self.group_size = GROUP_SIZE
        self.wrong = []
        self.hits = {}
        self.pool = []
        for w in words:
            if 0 <= w < nwords and w not in self.pool:
                self.pool.append(w)
        self._shuffle(self.pool)
        self._load_group(0, 0)

    def _reset_group(self) -> None:
        """本组错了：见过的词（含这次错的）打乱放最前，没见过的保持原序在后，从头再来。"""
        failed = self.queue[self.pos]
        self.seen = max(self.seen, self.pos + 1)
        self._shuffle(self.queue, self.seen)
        # 见过的词不止一个时，刚错的词不放第一个（刚看完答案，马上再考没有意义）
        if self.seen >= 2 and self.queue[0] == failed:
            j = 1 + self._below(self.seen - 1)
            self.queue[0], self.queue[j] = self.queue[j], failed
        self.restarts += 1
        self._begin()

    def _endless_advance(self) -> None:
        if not self._retire():
            self.pos += 1
            self.seen = max(self.seen, self.pos)
        self.fumbled = False
        if self.pos < self.qlen:
            self.phase = Phase.ASKING
            return
        if self.gend < self.endless_total():
            self._load_group(self.group + 1, self.gend)  # 上一组的词不再出现
        else:
            self.pos = max(self.qlen - 1, 0)
            self.phase = Phase.DONE

    def failed(self) -> bool:
        """无尽模式里本组已错、下一步是重开本组。"""
        return self.kind == Kind.ENDLESS and self.phase == Phase.REVEALED

    # ------------------------------------------------------------ 动作

    def answer(self, correct: bool) -> None:
        if self.phase not in (Phase.ASKING, Phase.WRONG):
            return
        if correct:
            self.phase = Phase.CORRECT
            if self.kind == Kind.ENDLESS and not self.fumbled:  # 一次答对：马上计上
                w = self.queue[self.pos]
                self.hits[w] = self.hits.get(w, 0) + 1
        else:
            self.phase = Phase.WRONG  # 提交答错只允许重试；「不知道」才公布答案并重开
        self.fumbled = True  # 换题型再答一遍不重复计

    def dont_know(self) -> None:
        if self.phase not in (Phase.ASKING, Phase.WRONG):
            return
        if self.kind == Kind.ROUNDS:
            self._mark_wrong(self.queue[self.pos])
        self.phase = Phase.REVEALED

    def next(self) -> int:
        """「下一个」/「重新开始本组」。普通模式答错后跳过时返回被跳过的词（用来公布答案），否则 -1。"""
        skipped = -1
        self.retired = self.joined = -1
        if self.phase == Phase.CORRECT:
            self._endless_advance() if self.kind == Kind.ENDLESS else self._rounds_advance()
        elif self.phase == Phase.WRONG and self.kind == Kind.ROUNDS:
            skipped = self.queue[self.pos]
            self._mark_wrong(skipped)
            self._rounds_advance()
        elif self.phase == Phase.REVEALED:
            self._reset_group() if self.kind == Kind.ENDLESS else self._rounds_advance()
        return skipped

    def restart(self) -> None:
        """普通模式重开本轮（第 1 轮 / 已完成则从头）；无尽模式从第 1 组重来（只练一部分词时还是这批词）。"""
        if self.kind == Kind.ENDLESS:
            self.wrong = []
            self.hits = {}
            self._load_group(0, 0)
        elif self.round == 1 or self.phase == Phase.DONE:
            self.start_rounds(self.nwords)
        else:
            self._rounds_load(self.round_words, self.round)

    def reask(self) -> None:
        """切换题型后重新作答当前词（无尽模式已错的不撤销）；完成页则直接重开。"""
        if self.phase == Phase.DONE:
            self.restart()
        elif self.phase in (Phase.WRONG, Phase.CORRECT):
            self.phase = Phase.ASKING
        elif self.phase == Phase.REVEALED and self.kind == Kind.ROUNDS:
            self.phase = Phase.ASKING

    def words_changed(self, nwords: int) -> None:
        """词表增删后同步词数（清空 → 回到空状态）。"""
        self.nwords = nwords
        if nwords == 0:
            self.pool = []
            self.hits = {}
            self.queue = []
            self.pos = 0
            self.wrong = []
            self.phase = Phase.EMPTY
        elif self.phase == Phase.EMPTY:
            self.start_endless(nwords) if self.kind == Kind.ENDLESS else self.start_rounds(nwords)

    # ------------------------------------------------------------ 存档

    def to_json(self) -> dict:
        d = asdict(self)
        d["kind"] = int(self.kind)
        d["phase"] = int(self.phase)
        d["hits"] = {str(k): v for k, v in self.hits.items() if v}
        return d

    @classmethod
    def from_json(cls, d: dict) -> Session:
        s = cls()
        for k, v in d.items():
            if hasattr(s, k):
                setattr(s, k, v)
        s.kind = Kind(int(s.kind))
        s.phase = Phase(int(s.phase))
        s.hits = {int(k): int(v) for k, v in dict(s.hits).items()}
        s.fumbled = bool(s.fumbled)
        return s
