"""「单词默写」：消息处理（update）+ 界面（view）+ 入口。

后端：session.py 出题状态机 / words.py 词表 / text.py 判分规则 / judge.py 本地模型判分 / forms.py 变形默写。
后台线程（模型判分、一词多义合并检查）用 xenthon.post 把结果送回界面线程。
"""

from __future__ import annotations

import ctypes
import json
import os
import sys
import threading
import time
import winreg
from dataclasses import dataclass, field
from enum import IntEnum

import xenthon as xtk

from . import progress
from .forms import FormsDrill, load_entries
from .judge import Judge
from .session import RETIRE_HITS, Kind, Phase, Session
from .text import answer_eq, is_blank, masked_display, meaning_match, senses_mark, senses_merge, split_senses
from .words import WordList, add_text

INFO, SUCCESS, WARNING, ERROR = (xtk.InfoBarSeverity.INFORMATIONAL, xtk.InfoBarSeverity.SUCCESS,
                                 xtk.InfoBarSeverity.WARNING, xtk.InfoBarSeverity.ERROR)
COLUMN, ROW = xtk.StackDirection.COLUMN, xtk.StackDirection.ROW
SEMI = xtk.FontWeight.SEMI_BOLD
STRETCH, CENTER = xtk.FlexAlign.STRETCH, xtk.FlexAlign.CENTER
SUBTLE, ACCENT = xtk.ButtonStyle.SUBTLE, xtk.ButtonStyle.ACCENT
JUDGE_MAX_PIECES = 4  # 一次最多判答案里的前几项：每项对每个义项问一次模型


class Page(IntEnum):  # 导航栏条目下标（分隔线也占一个）
    PRACTICE = 0
    ENDLESS = 1
    FORMS = 2
    SEP = 3
    LIBRARY = 4
    HELP = 5


K = xtk.NavKind
NAV_ITEMS = [(K.ITEM, "Edit", "默写"), (K.ITEM, "Sync", "无尽模式"), (K.ITEM, "Rename", "变形默写"), K.SEPARATOR,
             (K.ITEM, "List", "词库"), (K.FOOTER, "Help", "说明")]
CN2EN, EN2CN = 0, 1  # 看释义写单词 / 看单词写释义

_quit = threading.Event()  # 程序退出：合并线程别再发请求


def bits(m: int) -> int:
    return bin(m).count("1")


@dataclass
class Feedback:
    sev: int
    title: str
    msg: str


@dataclass
class Drill:
    """一套练习：普通模式、无尽模式各一套，切页互不影响。"""
    s: Session = field(default_factory=Session)
    fb: Feedback | None = None
    answered: int = 0
    correct: int = 0
    # 一词多义（单词 → 释义）：当前词拆出的义项、已答出的位掩码、义项属于哪个词（下标 + 1；0 = 需要重算）
    senses: list[str] = field(default_factory=list)
    got: int = 0
    sense_of: int = 0

    def reset(self) -> None:
        self.answered = self.correct = 0
        self.fb = None
        self.sense_of = 0


def system_dark() -> bool:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
            return winreg.QueryValueEx(k, "AppsUseLightTheme")[0] == 0
    except OSError:
        return False


def clipboard_text() -> str:
    u32, k32 = ctypes.windll.user32, ctypes.windll.kernel32
    u32.GetClipboardData.restype = ctypes.c_void_p
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    if not u32.OpenClipboard(None):
        return ""
    try:
        h = u32.GetClipboardData(13)  # CF_UNICODETEXT
        p = k32.GlobalLock(h) if h else None
        if not p:
            return ""
        try:
            return ctypes.wstring_at(p)
        finally:
            k32.GlobalUnlock(h)
    finally:
        u32.CloseClipboard()


# ==================================================================== 模型


class App:
    def __init__(self, data_dir: str, words_path: str, judge_path: str) -> None:
        self.data_dir = data_dir
        self.wl = WordList.load(words_path)
        self.wl_rev = 0
        self.judge = Judge(judge_path, data_dir)
        self.page = Page.PRACTICE
        self.dir = CN2EN
        self.drill = [Drill(), Drill()]  # [0] 普通模式，[1] 无尽模式
        self.answer = ""
        self.judge_seq = 0
        self.judging = False
        self.add_fb: Feedback | None = None
        self.merging = False
        self.clear_open = False
        self.add_open = False
        self.add_text = ""
        self.focus_seq = 0  # 每个动作 +1：焦点回到输入框
        self.dark = system_dark()
        self.settings_path = os.path.join(data_dir, "dictation_settings.json")
        self.progress_path = os.path.join(data_dir, "dictation_progress.json")
        self.legacy_progress = os.path.join(data_dir, "dictation_progress.bin")
        self.all_senses = True
        self.settings_load()
        forms_path = os.path.join(data_dir, "dictation_forms.json")
        self.forms = FormsDrill(load_entries(forms_path))
        self.forms_fb: Feedback | None = None
        seed = int(time.time_ns()) ^ (os.getpid() << 32)
        self.drill[0].s.seed(seed)
        self.drill[1].s.seed(seed ^ 0xA5A5A5A5DEADBEEF)
        self.drill[0].s.start_rounds(len(self.wl))
        self.drill[1].s.start_endless(len(self.wl))  # 无尽模式随时可进，进度独立保存
        self.progress_load()

    # ------------------------------------------------------------ 存档

    def settings_load(self) -> None:
        try:
            with open(self.settings_path, encoding="utf-8-sig") as f:
                self.all_senses = json.load(f).get("all_senses", True) is not False
        except (OSError, ValueError, AttributeError):
            pass

    def settings_save(self) -> None:
        try:
            with open(self.settings_path, "w", encoding="utf-8") as f:
                f.write('{\n  "all_senses": %s\n}\n' % ("true" if self.all_senses else "false"))
        except OSError:
            pass

    def progress_save(self) -> None:
        progress.save(self.progress_path, {
            "version": progress.VERSION, "nwords": len(self.wl), "dir": self.dir,
            "drills": [{"session": d.s.to_json(), "answered": d.answered, "correct": d.correct} for d in self.drill],
            "forms": self.forms.to_json()})

    def progress_load(self) -> None:
        data = progress.load(self.progress_path, self.legacy_progress, len(self.wl))
        if not data:
            return
        n = len(self.wl)
        try:
            if 0 < data["nwords"] <= n and data["dir"] in (CN2EN, EN2CN):
                self.dir = data["dir"]
                for d, rec in zip(self.drill, data["drills"]):
                    d.s = Session.from_json(rec["session"])
                    d.answered, d.correct = int(rec["answered"]), int(rec["correct"])
                    if d.s.phase in (Phase.CORRECT, Phase.REVEALED):
                        d.s.next()  # 上次答完没点「下一个」：直接到下一题
                    if data["nwords"] != n:
                        d.s.words_changed(n)
        except (KeyError, TypeError, ValueError):
            for i, d in enumerate(self.drill):
                d.s = Session()
                d.s.start_rounds(n) if i == 0 else d.s.start_endless(n)
        if isinstance(data.get("forms"), dict):
            self.forms.restore(data["forms"])

    # ------------------------------------------------------------ 公共

    def cur_drill(self) -> Drill | None:
        return {Page.PRACTICE: self.drill[0], Page.ENDLESS: self.drill[1]}.get(self.page)

    def drill_senses(self, d: Drill) -> int:
        """让 d.senses 对上当前题目（换词时重拆、清空已答出）；返回已答出的个数。"""
        idx = d.s.current()
        if idx < 0:
            d.senses, d.got, d.sense_of = [], 0, 0
            return 0
        if d.sense_of != idx + 1:
            m = self.wl.words[idx].meaning
            d.senses = split_senses(m) or [m]
            d.got = 0
            d.sense_of = idx + 1
        return bits(d.got)

    def answer_text(self, idx: int) -> str:
        w = self.wl.words[idx]
        return f"{w.word}  {w.pos}  {w.meaning}" if self.dir == EN2CN else f"{w.meaning}  {w.pos}  {w.word}"

    @staticmethod
    def missing(d: Drill) -> str:
        return "、".join(s for k, s in enumerate(d.senses) if not d.got & (1 << k))

    # ------------------------------------------------------------ 判分

    def start_judging(self, d: Drill, idx: int) -> bool:
        if not self.judge.enabled or is_blank(self.answer):
            return False
        self.judge_seq += 1
        w = self.wl.words[idx]
        if len(d.senses) > 1:
            senses, todo = list(d.senses), ~d.got
        else:
            senses, todo = [w.meaning], 1
        threading.Thread(target=judge_job, args=(self.judge, self.judge_seq, self.answer, senses, todo,
                                                 self.judge.threshold), daemon=True).start()
        self.judging = True
        d.fb = Feedback(INFO, "正在判断…", "字面没判出来，正在请本地模型看意思是否接近。")
        return True

    def cancel_judging(self) -> None:
        if self.judging:
            self.judge_seq += 1
        self.judging = False

    def apply_verdict(self, d: Drill, ok: bool, p: float) -> None:
        """判定结果落地：p < 0 表示没用上模型。"""
        idx = d.s.current()
        slipped = d.s.phase == Phase.WRONG  # 之前在这个词上错过
        d.s.answer(ok)
        if ok:
            d.answered += 1
            if not slipped:
                d.correct += 1
            w = self.wl.words[idx]
            d.fb = Feedback(SUCCESS, "答对了", f"本地模型判为意思接近（{p:.2f}）· {w.word}  {w.meaning}" if p >= 0
                            else f"{w.word}  {w.pos}  {w.meaning}")
            self.answer = ""
        elif d.s.phase == Phase.REVEALED:
            d.answered += 1
            ans = self.answer_text(idx)
            d.fb = Feedback(ERROR, "已公布答案", f"接近度 {p:.2f} · 正确答案：{ans}" if p >= 0 else f"正确答案：{ans}")
            self.answer = ""
        else:
            d.fb = Feedback(ERROR, "答错了", (f"接近度 {p:.2f}。" if p >= 0 else "") + "再试一次，或点「不知道」看答案。")

    def senses_progress(self, d: Drill, fresh: int, p: float) -> None:
        """一词多义答出了 fresh 个新义项：全齐了（或开关关着）算答对，否则清空输入框接着写。"""
        left = len(d.senses) - bits(d.got)
        if left == 0 or not self.all_senses:
            self.apply_verdict(d, True, p)
            return
        if p >= 0:
            d.fb = Feedback(SUCCESS, "答对一个", f"本地模型判为意思接近（{p:.2f}）。还差 {left} 个意思，接着写。")
        else:
            d.fb = Feedback(SUCCESS, "答对几个" if fresh > 1 else "答对一个", f"还差 {left} 个意思，接着写。")
        self.answer = ""

    def do_submit(self) -> None:
        d = self.cur_drill()
        if not d or self.judging or d.s.phase not in (Phase.ASKING, Phase.WRONG):
            return
        idx = d.s.current()
        w = self.wl.words[idx]
        if self.dir == CN2EN:
            self.apply_verdict(d, answer_eq(self.answer, w.word), -1)
            return
        self.drill_senses(d)
        if len(d.senses) > 1:  # 每次答出一个（或几个），全部答出才算这个词对（开关关掉时答出一个就算）
            d.got, fresh = senses_mark(self.answer, d.senses, d.got)
            if fresh:
                self.senses_progress(d, fresh, -1)
                return
        elif meaning_match(self.answer, w.meaning):
            self.apply_verdict(d, True, -1)
            return
        if not self.start_judging(d, idx):
            self.apply_verdict(d, False, -1)

    def on_judged(self, seq: int, hit: int, p: float) -> None:
        d = self.cur_drill()
        if not d or not self.judging or seq != self.judge_seq:
            return  # 过期结果（期间切了页 / 题型）
        self.judging = False
        if len(d.senses) > 1:
            hit &= ~d.got & ((1 << len(d.senses)) - 1)
            if hit:
                d.got |= hit
                self.senses_progress(d, bits(hit), p)
                return
        self.apply_verdict(d, p >= 0 and hit != 0, p if p >= 0 else -1)

    # ------------------------------------------------------------ 练习动作

    def do_dont(self) -> None:
        d = self.cur_drill()
        if not d or self.judging or d.s.phase not in (Phase.ASKING, Phase.WRONG):
            return
        idx = d.s.current()
        d.s.dont_know()
        d.answered += 1
        miss = ""
        if self.dir == EN2CN and len(d.senses) > 1 and d.got:  # 答出过一部分：点明还漏了哪些
            miss = "\n没答出：" + self.missing(d)
        if d.s.kind == Kind.ENDLESS:
            d.fb = Feedback(ERROR, "本组要重新开始", f"正确答案：{self.answer_text(idx)}{miss}")
        else:
            d.fb = Feedback(WARNING, "答案", self.answer_text(idx) + miss)
        self.answer = ""

    def do_next(self) -> None:
        d = self.cur_drill()
        if not d or self.judging or d.s.phase not in (Phase.CORRECT, Phase.WRONG, Phase.REVEALED):
            return
        s = d.s
        group, restarts = s.group, s.restarts
        skipped = s.next()
        d.sense_of = 0
        self.answer = ""
        d.fb = None
        if skipped >= 0:  # 普通模式答错后跳过：顺带公布上一题答案
            d.answered += 1
            d.fb = Feedback(WARNING, "上一题的答案", self.answer_text(skipped))
        elif s.kind == Kind.ENDLESS and s.phase != Phase.DONE:
            words = self.wl.words
            if s.group != group:
                d.fb = Feedback(SUCCESS, "本组过关", f"第 {group + 1} 组全部答对，进入第 {s.group + 1} 组。")
            elif s.restarts != restarts:
                d.fb = Feedback(INFO, "本组重新开始", f"见过的 {s.seen} 个词已打乱顺序排在前面，没见过的词保持原顺序。")
            elif s.retired >= 0 and s.joined >= 0:
                d.fb = Feedback(SUCCESS, "掌握一个词", f"{words[s.retired].word} 已一次答对 {RETIRE_HITS} 次，移出本组，"
                                                  f"换上 {words[s.joined].word}。")
            elif s.retired >= 0:
                d.fb = Feedback(SUCCESS, "掌握一个词", f"{words[s.retired].word} 已一次答对 {RETIRE_HITS} 次，移出本组。"
                                                  f"后面没有新词了，本组还剩 {s.qlen} 个。")

    def do_enter(self, text: str) -> None:
        """回车 = 主按钮：答题中提交，判定后下一个 / 重新开始本组。"""
        d = self.cur_drill()
        if not d:
            return
        if d.s.phase in (Phase.ASKING, Phase.WRONG):
            self.answer = text
            self.do_submit()
        elif d.s.phase in (Phase.CORRECT, Phase.REVEALED):
            self.do_next()

    def do_all_senses(self, on: bool) -> None:
        """「多义词全答」开关：关掉时，正答到一半的词已答出过意思就直接算对。"""
        if self.all_senses == on:
            return
        self.all_senses = on
        self.settings_save()
        d = self.cur_drill()
        if not on and d and not self.judging and self.dir == EN2CN and d.got and d.s.phase in (Phase.ASKING, Phase.WRONG):
            self.apply_verdict(d, True, -1)

    def do_endless_wrong(self) -> None:
        """普通模式结果页：这一轮的错词打乱后替换无尽模式的进度，切到无尽模式。"""
        src = self.drill[0].s
        if src.kind != Kind.ROUNDS or src.phase != Phase.PREVIEW or not src.wrong:
            return
        self.cancel_judging()
        d = self.drill[1]
        d.s.start_endless_pool(len(self.wl), src.wrong)
        d.reset()
        d.fb = Feedback(INFO, "错词无尽模式", f"第 {src.round} 轮的 {len(d.s.pool)} 个错词已打乱，每 20 个一组。"
                                         f"练完回「默写」还能接着开始第 {src.round + 1} 轮。")
        self.page = Page.ENDLESS
        self.answer = ""

    def do_endless_all(self) -> None:
        d = self.drill[1]
        if not d.s.pool:
            return
        self.cancel_judging()
        d.s.start_endless(len(self.wl))
        d.reset()
        self.answer = ""

    def do_restart(self) -> None:
        d = self.cur_drill()
        if not d:
            return
        self.cancel_judging()
        d.s.restart()
        d.reset()
        self.answer = ""

    def do_start_round(self) -> None:
        d = self.cur_drill()
        if not d:
            return
        d.s.next_round()
        d.sense_of = 0
        d.fb = None
        self.answer = ""

    def do_nav(self, page: int) -> None:
        if page not in (Page.PRACTICE, Page.ENDLESS, Page.FORMS, Page.LIBRARY, Page.HELP) or page == self.page:
            return
        self.cancel_judging()
        d = self.cur_drill()
        if d and d.s.phase in (Phase.ASKING, Phase.WRONG) and d.fb and d.fb.sev == INFO:
            d.fb = None  # 「正在判断…」随判分一起作废
        self.page = Page(page)
        self.answer = ""

    def do_dir(self, direction: int) -> None:
        if direction not in (CN2EN, EN2CN) or direction == self.dir:
            return
        self.cancel_judging()
        self.dir = direction
        if direction == EN2CN:
            self.judge.warmup()  # 预热本地模型
        for d in self.drill:
            d.s.reask()
            d.sense_of = 0
            if not d.s.failed() and d.s.phase != Phase.DONE:
                d.fb = None
        self.answer = ""

    # ------------------------------------------------------------ 词库

    def words_changed(self) -> None:
        self.wl_rev += 1
        for d in self.drill:
            d.s.words_changed(len(self.wl))

    def do_add(self, text: str) -> None:
        before = len(self.wl)
        st = add_text(self.wl, text)
        if st.err:
            self.add_fb = Feedback(ERROR, "没有录入", st.err)
        elif st.added <= 0:
            self.add_fb = Feedback(WARNING, "没有可录入的词", f"{st.dup} 个已在词库里，{st.bad} 行格式不对。")
        elif st.dup + st.bad > 0:
            self.add_fb = Feedback(SUCCESS, "已录入", f"新增 {st.added} 个词，跳过 {st.dup} 个重复、{st.bad} 行格式不对。"
                                                   f"现在共 {len(self.wl)} 个词。")
        else:
            self.add_fb = Feedback(SUCCESS, "已录入", f"新增 {st.added} 个词，现在共 {len(self.wl)} 个词。新词排在词表末尾。")
        if st.added > 0:
            self.words_changed()
            self.start_merge(before, len(self.wl), self.add_fb.msg)  # 新词里的一词多义：意思相同的释义合并

    def do_add_clip(self) -> None:
        text = clipboard_text()
        if not text.strip():
            self.add_fb = Feedback(WARNING, "剪贴板是空的", "先在别处复制好词表，再点「从剪贴板导入」。")
        else:
            self.do_add(text)

    def do_add_result(self, result: int) -> None:
        self.add_open = False
        if result == 1:
            if not self.add_text.strip():
                self.add_fb = Feedback(WARNING, "没有录入", "输入框是空的。")
            else:
                self.do_add(self.add_text)
                self.add_text = ""

    def do_clear_result(self, result: int) -> None:
        self.clear_open = False
        if result != 1:
            return
        self.cancel_judging()
        n = len(self.wl)
        self.wl.clear()
        self.words_changed()
        for d in self.drill:
            d.reset()
        self.add_fb = Feedback(INFO, "词库已清空", f"删除了 {n} 个词。重新录入后就能开始默写。")

    # ------------------------------------------------------------ 一词多义：合并意思相同的释义

    def start_merge(self, start: int, end: int, intro: str = "") -> bool:
        """检查词表 [start, end) 里的多义词。没有多义词 / 没配合并 / 正在查 → False。"""
        thr = self.judge.merge_threshold_or_zero()
        if thr <= 0 or self.merging:
            return False
        items = [{"word": w.word, "meaning": w.meaning, "merged": ""} for w in self.wl.words[start:end]
                 if len(split_senses(w.meaning)) > 1]
        if not items:
            return False
        job = {"items": items, "intro": intro, "failed": False}
        threading.Thread(target=merge_job, args=(self.judge, thr, job), daemon=True).start()
        self.merging = True
        self.add_fb = Feedback(INFO, "正在检查一词多义…", (intro + "\n" if intro else "") +
                               f"用智能判分检查 {len(items)} 个多义词里有没有意思相同的释义。")
        return True

    def on_merge_progress(self, done: int, total: int) -> None:
        if self.merging and self.add_fb:
            self.add_fb.title = f"正在检查一词多义：{done} / {total}"

    def on_merge_done(self, job: dict) -> None:
        self.merging = False
        changed, lines = set(), []
        for it in job["items"]:
            if not it["merged"]:
                continue
            for i, w in enumerate(self.wl.words):
                if w.word == it["word"] and w.meaning == it["meaning"]:  # 期间被删 / 改过就不动
                    w.meaning = it["merged"]
                    changed.add(i)
                    lines.append(f"\n{w.word}：{w.meaning}")
                    break
        if changed:
            self.wl_rev += 1
            for d in self.drill:  # 正在考的词释义变了：义项重拆
                if d.sense_of > 0 and d.sense_of - 1 in changed:
                    d.sense_of = 0
        saved = not changed or self.wl.save()
        intro = job["intro"]
        sep = "\n" if intro else ""
        if not saved:
            self.add_fb = Feedback(ERROR, "合并结果没存上", f"{intro}{sep}写入词表失败：{self.wl.path}")
        elif changed:
            cut = "（智能判分中途断开，后面的没查完）" if job["failed"] else ""
            self.add_fb = Feedback(SUCCESS, "已合并意思相同的释义",
                                   f"{intro}{sep}{len(changed)} 个词的释义有合并{cut}，写其中任何一个都算答对这一项："
                                   + "".join(lines[:6]) + ("\n…" if len(lines) > 6 else ""))
        elif job["failed"]:
            self.add_fb = Feedback(WARNING, "一词多义没有检查", f"{intro}{sep}智能判分没启动或中途断开，释义照原样保存。")
        elif intro:
            self.add_fb = Feedback(SUCCESS, "已录入", f"{intro}\n一词多义里没有意思相同的释义。")
        else:
            self.add_fb = Feedback(INFO, "没有要合并的释义", f"检查了 {len(job['items'])} 个多义词，没有意思相同的释义。")

    def do_merge_all(self) -> None:
        if not self.merging and not self.start_merge(0, len(self.wl)):
            self.add_fb = Feedback(INFO, "没有要检查的词", "词库里没有一词多义的词。")

    # ------------------------------------------------------------ 变形默写

    def forms_action(self, name: str) -> None:
        fd = self.forms
        getattr(fd, name)()


# ==================================================================== 后台线程


def judge_job(judge: Judge, seq: int, ans: str, senses: list[str], todo: int, threshold: float) -> None:
    """答案按 、，; 拆成几项，逐项对还没答出的义项判（整串交给模型不可靠）。一项答案只顶一个义项。"""
    parts = (split_senses(ans) or [ans])[:JUDGE_MAX_PIECES]
    hit, best, worst_hit = 0, -1.0, 2.0
    for part in parts:
        for k, sense in enumerate(senses):
            if not todo & (1 << k) or hit & (1 << k):
                continue
            q = judge.p(sense, part)
            if q < 0:  # 模型不可用
                xtk.post(("judged", seq, hit, worst_hit if hit else -1.0))
                return
            best = max(best, q)
            if q >= threshold:
                hit |= 1 << k
                worst_hit = min(worst_hit, q)
                break
    xtk.post(("judged", seq, hit, worst_hit if hit else best))


def merge_job(judge: Judge, thr: float, job: dict) -> None:
    """「缓解、减轻」这种：两条释义双向都过阈值才合并，宁可不合并也不错合并。"""
    split = [split_senses(it["meaning"]) for it in job["items"]]
    total = sum(len(sn) * (len(sn) - 1) // 2 for sn in split)
    done = 0
    xtk.post(("merge_progress", 0, total))
    ready = False
    for _ in range(240):  # 服务还没起：拉起来等它就绪（最多约 60 秒）
        if _quit.is_set():
            break
        if judge.p("取代", "代替") >= 0:
            ready = True
            break
        time.sleep(0.25)
    if not ready:
        job["failed"] = True
    for it, sn in zip(job["items"], split):
        if job["failed"]:
            break
        links = set()
        for i in range(len(sn)):
            for k in range(i + 1, len(sn)):
                done += 1
                xtk.post(("merge_progress", done, total))
                p = -1.0 if _quit.is_set() else judge.p(sn[i], sn[k])
                q = judge.p(sn[k], sn[i]) if p > thr else p  # 单向就不够的，不用再问反向
                if p < 0 or q < 0:
                    job["failed"] = True
                    break
                if p > thr and q > thr:
                    links.add((i, k))
            if job["failed"]:
                break
        if not job["failed"]:
            it["merged"] = senses_merge(sn, links) or ""
    xtk.post(("merge_done", job))


# ==================================================================== update


def update(a: App, msg) -> App:
    tag = msg[0]
    if tag not in ("answer", "add_text", "merge_progress", "form_text"):
        a.focus_seq += 1
    match msg:
        case ("nav", i):
            a.do_nav(i)
        case ("dir", i):
            a.do_dir(i)
        case ("answer", text):
            a.answer = text
        case ("enter", text):
            a.do_enter(text)
        case ("submit",):
            a.do_submit()
        case ("dont",):
            a.do_dont()
        case ("next",):
            a.do_next()
        case ("restart",):
            a.do_restart()
        case ("start_round",):
            a.do_start_round()
        case ("judged", seq, hit, p):
            a.on_judged(seq, hit, p)
        case ("go_library",):
            a.do_nav(Page.LIBRARY)
        case ("add_open",):
            a.add_open = True
        case ("add_text", text):
            a.add_text = text
        case ("add_result", r):
            a.do_add_result(r)
        case ("add_clip",):
            a.do_add_clip()
        case ("add_fb_close",):
            a.add_fb = None
        case ("clear_ask",):
            a.clear_open = True
        case ("clear_result", r):
            a.do_clear_result(r)
        case ("all_senses", on):
            a.do_all_senses(bool(on))
        case ("merge_all",):
            a.do_merge_all()
        case ("endless_wrong",):
            a.do_endless_wrong()
        case ("endless_all",):
            a.do_endless_all()
        case ("merge_progress", done, total):
            a.on_merge_progress(done, total)
        case ("merge_done", job):
            a.on_merge_done(job)
        case ("form_text", i, text):
            a.forms.set_text(i, text)
        case ("form_enter", i, text):
            a.forms.set_text(i, text)
            a.forms.submit()
        case ("forms", name) if name in ("submit", "give_up", "next_action", "start_round", "restart"):
            a.forms_action(name)
    if tag not in ("answer", "add_text", "merge_progress", "form_text"):
        a.progress_save()  # 打字不存，其余每步都存：崩了也不丢
    return a


# ==================================================================== 视图

SECONDARY = 0x8A8A8AFF


def accent(a: App) -> int:
    return 0x60CDFFFF if a.dark else 0x005FB8FF


def good(a: App) -> int:
    return 0x6CCB5FFF if a.dark else 0x0F7B0FFF


def bad(a: App) -> int:
    return 0xFF99A4FF if a.dark else 0xC42B1CFF


def caption(t: str) -> xtk.El:
    return xtk.text(text=t, fontsize=12, fg=SECONDARY)


def body(t: str) -> xtk.El:
    return xtk.text(text=t, fontsize=13, fg=SECONDARY, textwrap=True)


def subtitle(t: str) -> xtk.El:
    return xtk.text(text=t, fontsize=16, fontweight=SEMI)


def para(t: str) -> xtk.El:
    return xtk.text(text=t, fontsize=14, textwrap=True)


def row(*kids, **kw) -> xtk.El:
    return xtk.container(*kids, dir=ROW, **kw)


def column(*kids, **kw) -> xtk.El:
    return xtk.container(*kids, dir=COLUMN, **kw)


def page_header(title: str, blurb: str | None = None, trailing: xtk.El | None = None) -> xtk.El:
    """页标题：第一行大标题 + 右边的页级操作，说明另起一行占满宽度（工具多时标题不会被挤断）。"""
    return column(
        row(row(xtk.text(text=title, fontsize=28, fontweight=SEMI), shrink=0),
            row(trailing, shrink=1) if trailing else None,
            gap=16, items=CENTER, justify=xtk.FlexJustify.SPACE_BETWEEN, fill=True),
        body(blurb) if blurb else None,
        gap=6, items=STRETCH)


def section(title: str, desc: str | None, content: xtk.El) -> xtk.El:
    return xtk.card(subtitle(title), body(desc) if desc else None, content,
                    dir=COLUMN, gap=12, pl=20, pt=18, pr=20, pb=18, items=STRETCH)


def stat_tile(label: str, value: str) -> xtk.El:
    return xtk.card(caption(label), xtk.text(text=value, fontsize=24, fontweight=SEMI),
                    dir=COLUMN, gap=4, pl=18, pt=14, pr=18, pb=14, grow=1)


def feedback_bar(fb: Feedback | None, close=None) -> xtk.El | None:
    if not fb:
        return None
    return xtk.infobar(text=fb.title, message=fb.msg, severity=fb.sev, closable=close is not None, on_close=close)


def fill_page(key: str, *kids) -> xtk.El:
    """练习页：不滚动，卡片撑满剩余高度。"""
    return column(*kids, key=key, gap=20, pl=36, pt=28, pr=36, pb=28, items=STRETCH, fill=True, grow=1)


def word_row(no: int, word: str, pos: str, meaning: str) -> xtk.El:
    """词表一行：序号 · 单词 · 词性 · 释义。"""
    return row(
        xtk.text(text=str(no), fontsize=12, fg=SECONDARY, w=36),
        xtk.text(text=word, fontsize=15, fontweight=SEMI, w=190),
        xtk.text(text=pos, fontsize=12, fg=SECONDARY, w=72),
        xtk.text(text=meaning, fontsize=14, grow=1, maxlines=1, trim=xtk.TextTrim.CHAR_ELLIPSIS),
        gap=16, items=CENTER, pl=8, pt=4, pr=8, pb=4)


def word_list(a: App, idx: list[int] | None, key: str) -> xtk.El:
    """虚拟化词表：只构建视口里看得见的几行，词再多也不卡。"""
    words = a.wl.words

    def item(i: int) -> xtk.El:
        w = words[idx[i] if idx is not None else i]
        return word_row(i + 1, w.word, w.pos, w.meaning)

    n = len(idx) if idx is not None else len(words)
    return xtk.list(key=f"{key}-{a.wl_rev}", grow=1, itemh=44, mode=xtk.ListSelMode.NONE, sel=-1, items=(n, item))


# ------------------------------------------------------------ 练习页（普通模式 / 无尽模式共用）


def drill_tools(a: App, endless: bool) -> xtk.El:
    wrong_only = endless and bool(a.drill[1].s.pool)  # 无尽模式正在练上一轮的错词
    return row(
        xtk.button(text="换回全部词库", icon="List", style=SUBTLE, on_invoke=("endless_all",)) if wrong_only else None,
        xtk.selector(items=[("Edit", "释义 → 单词"), ("Font", "单词 → 释义")], sel=a.dir, on_select=lambda i: ("dir", i)),
        xtk.checkbox(text="多义词全答", checked=a.all_senses, on_change=lambda b: ("all_senses", b),
                     tip="单词 → 释义：有几个意思要全部写出来" if a.all_senses else "单词 → 释义：写出任意一个意思就算对"),
        xtk.button(icon="Refresh", style=SUBTLE, on_invoke=("restart",),
                   tip="这批错词从第 1 组重新开始" if wrong_only else "从第 1 组重新开始" if endless else "从第 1 轮重新开始"),
        gap=8, items=CENTER)


def drill_blurb(a: App, s: Session, endless: bool) -> str:
    if endless and s.pool:
        return f"错词 {len(s.pool)} 个 · 已打乱 · 每 20 词一组 · 对满 15 次换新词 · 整组全对才进下一组"
    if endless:
        return "每 20 词一组 · 错一次本组打乱重来 · 对满 15 次换新词 · 整组全对才进下一组"
    if s.round == 1:
        return f"按词表顺序默写全部 {len(a.wl)} 个词，错的词进下一轮。"
    return f"第 {s.round} 轮 · 只默上一轮错的 {s.qlen} 个词。"


def view_empty(title: str) -> xtk.El:
    return fill_page("empty",
                     page_header(title),
                     xtk.infobar(text="词库还是空的", message="先去词库录入单词，再回来默写。"),
                     row(xtk.button(text="去词库添加", icon="Add", style=ACCENT, on_invoke=("go_library",))))


def view_preview(a: App, d: Drill) -> xtk.El:
    """普通模式一轮结束：错词预览。"""
    s = d.s
    return fill_page(
        "preview",
        page_header(f"第 {s.round} 轮结束", "把错的词过一遍，再开始下一轮。"),
        xtk.infobar(text=f"这一轮错了 {s.wlen} 个词", message=f"共 {s.qlen} 个词，答对 {s.qlen - s.wlen} 个。", severity=WARNING),
        xtk.card(word_list(a, s.wrong, "wrong"), dir=COLUMN, pl=8, pt=8, pr=8, pb=8, items=STRETCH, fill=True, grow=1),
        row(xtk.button(text="从头开始", icon="Refresh", on_invoke=("restart",)),
            xtk.button(text="错词进无尽模式", icon="Sync", on_invoke=("endless_wrong",),
                       tip="这一轮的错词打乱后替换无尽模式的进度；这里的结果页不变"),
            xtk.button(text=f"开始第 {s.round + 1} 轮", icon="Play", style=ACCENT, on_invoke=("start_round",),
                       focus=("preview", a.focus_seq)),
            gap=8, justify=xtk.FlexJustify.END, fill=True))


def done_card(icon: str, big: str, sub: str, button: xtk.El) -> xtk.El:
    return xtk.card(
        xtk.button(icon=icon, style=SUBTLE, enabled=False),
        xtk.text(text=big, fontsize=34, fontweight=SEMI, textalign=xtk.TextAlign.CENTER),
        body(sub),
        button,
        dir=COLUMN, gap=16, pl=40, pt=32, pr=40, pb=32, items=CENTER, justify=xtk.FlexJustify.CENTER, fill=True, grow=1)


def view_done(a: App, d: Drill, endless: bool) -> xtk.El:
    s = d.s
    big = f"{s.group + 1} 组全部过关" if endless else f"{s.nwords} 个词全部默对"
    sub = f"无尽模式完成 · 共 {s.endless_total()} 个{'错词' if s.pool else '词'}" if endless else f"用了 {s.round} 轮"
    return fill_page(
        "done-endless" if endless else "done",
        page_header("无尽模式" if endless else "默写", None, drill_tools(a, endless)),
        done_card("Flag", big, sub, xtk.button(text="再来一遍" if endless else "再来一轮", icon="Refresh", style=ACCENT,
                                               on_invoke=("restart",), focus=("done", a.focus_seq))))


def view_senses(a: App, d: Drill, got: int, revealed: bool) -> xtk.El:
    """一词多义的进度：答出的打勾，没答出的留空（公布答案后标出漏掉的）。"""
    parts = []
    for k, sense in enumerate(d.senses):
        if d.got & (1 << k):
            parts.append("✓ " + sense)
        elif revealed:
            parts.append("✗ " + sense)
        else:
            parts.append("＿＿＿")
    return column(caption(f"共 {len(d.senses)} 个意思 · 已答出 {got} 个"),
                  xtk.text(text="　　".join(parts), fontsize=18, fontweight=SEMI, textalign=xtk.TextAlign.CENTER,
                           fg=accent(a), textwrap=True),
                  gap=6, items=CENTER)


def view_drill(a: App, d: Drill, endless: bool) -> xtk.El:
    s = d.s
    title = "无尽模式" if endless else "默写"
    if s.phase == Phase.EMPTY:
        return view_empty(title)
    if s.phase == Phase.PREVIEW:
        return view_preview(a, d)
    if s.phase == Phase.DONE:
        return view_done(a, d, endless)

    idx = s.current()
    w = a.wl.words[idx]
    asking = s.phase in (Phase.ASKING, Phase.WRONG)
    busy = a.judging

    if endless:
        where = (f"第 {s.group + 1} / {s.ngroups()} 组 · 这个词已对 {s.hits.get(idx, 0)}/{RETIRE_HITS}"
                 + (f" · 本组已重开 {s.restarts} 次" if s.restarts else ""))
    else:
        where = f"第 {s.round} 轮 · 错词复习" if s.round > 1 else "第 1 轮"
    prog = column(
        row(xtk.text(text=f"{s.pos + 1} / {s.qlen}", fontsize=13, fontweight=SEMI), caption(where),
            justify=xtk.FlexJustify.SPACE_BETWEEN, fill=True),
        xtk.progress(value=float(s.pos), max=float(max(s.qlen, 1)), indeterminate=busy),
        gap=8, items=STRETCH)

    hint = None
    if a.dir == EN2CN:
        prompt = xtk.text(text=w.word, fontsize=52, fontweight=SEMI, textalign=xtk.TextAlign.CENTER)
        got = a.drill_senses(d)
        if len(d.senses) > 1 and a.all_senses:
            hint = view_senses(a, d, got, s.phase == Phase.REVEALED)
    else:
        prompt = xtk.text(text=w.meaning, fontsize=38, fontweight=SEMI, textalign=xtk.TextAlign.CENTER, textwrap=True)
        hint = xtk.text(text=masked_display(w.word), fontsize=24, fontweight=SEMI, textalign=xtk.TextAlign.CENTER,
                        fg=accent(a))

    placeholder = ("写出中文释义，回车提交" if a.dir == EN2CN else "写出英文单词，回车提交") if asking else "回车 = 下一个"
    box = xtk.suggest(key="answer", text=a.answer, placeholder=placeholder, w=380, enabled=not busy,
                      focus=(a.page, a.focus_seq), on_change=lambda t: ("answer", t),
                      on_query=lambda t, i: ("enter", t))
    entry = row(box,
                xtk.button(text="提交", icon="Accept", style=ACCENT, enabled=not busy, on_invoke=("submit",)) if asking else None,
                gap=8, items=STRETCH, justify=xtk.FlexJustify.CENTER)

    act = []
    if s.phase in (Phase.ASKING, Phase.WRONG):
        act.append(xtk.button(text="不知道", icon="Help", style=SUBTLE, enabled=not busy, on_invoke=("dont",)))
        if s.phase == Phase.WRONG and not endless:
            act.append(xtk.button(text="跳过", icon="Next", enabled=not busy, on_invoke=("next",)))
    elif s.phase == Phase.CORRECT or not endless:
        act.append(xtk.button(text="下一个", icon="Next", style=ACCENT, on_invoke=("next",)))
    else:  # 无尽模式错了：原「下一个」的位置换成「重新开始」，答案留在屏幕上
        act.append(xtk.button(text="重新开始本组", icon="Refresh", style=ACCENT, on_invoke=("next",)))

    card = xtk.card(
        xtk.text(text=w.pos, fontsize=13, textalign=xtk.TextAlign.CENTER, fg=SECONDARY),
        # 题面和提示包成一组：多义词有提示、单意词没有，输入框的位置也不变
        column(prompt, hint, gap=18, items=STRETCH),
        row(entry, mt=8, justify=xtk.FlexJustify.CENTER),
        feedback_bar(d.fb),
        row(*act, gap=8, justify=xtk.FlexJustify.CENTER),
        dir=COLUMN, gap=18, pl=40, pt=28, pr=40, pb=28, items=STRETCH, justify=xtk.FlexJustify.CENTER, fill=True, grow=1)

    if endless:
        stats = row(stat_tile("当前组", f"{s.group + 1} / {s.ngroups()}"),
                    stat_tile("本组已见过", f"{max(s.seen, s.pos + 1)} / {s.qlen}"),
                    stat_tile("已过关的词", str(s.endless_passed())),
                    stat_tile("累计一次答对", str(s.total_hits())), gap=12, items=STRETCH, fill=True)
    else:
        stats = row(stat_tile("已作答", str(d.answered)), stat_tile("一次答对", str(d.correct)),
                    stat_tile("本轮错词", str(s.wlen)), gap=12, items=STRETCH, fill=True)

    return fill_page("endless" if endless else "practice",
                     page_header(title, drill_blurb(a, s, endless), drill_tools(a, endless)), prog, card, stats)


# ------------------------------------------------------------ 变形默写


def view_forms(a: App) -> xtk.El:
    fd = a.forms
    restart = xtk.button(icon="Refresh", style=SUBTLE, on_invoke=("forms", "restart"),
                         tip="全部完成后从第 1 轮开始，否则重开本轮")
    if fd.phase == "empty":
        return fill_page("forms-empty", page_header("变形默写"),
                         xtk.infobar(text="没有词条", message="数据目录下的 dictation_forms.json 读不出来。", severity=WARNING))
    if fd.phase == "preview":
        rows = []
        for wi, fi in fd.wrong:
            f = fd.entries[wi].forms[fi]
            rows.append((f.word, f.pos, f.meaning))
        return fill_page(
            "forms-preview",
            page_header(f"第 {fd.round} 轮结束", "本轮错题过一遍，再开始默写。", restart),
            xtk.infobar(text=f"本轮错题 {len(rows)} 个", message="下一轮只默错的形式，同一个词条的错题合在一屏。", severity=WARNING),
            xtk.card(xtk.list(key=f"forms-wrong-{fd.round}", grow=1, itemh=44, mode=xtk.ListSelMode.NONE, sel=-1,
                              items=(len(rows), lambda i: word_row(i + 1, *rows[i]))),
                     dir=COLUMN, pl=8, pt=8, pr=8, pb=8, items=STRETCH, fill=True, grow=1),
            row(xtk.button(text="重新开始", icon="Refresh", on_invoke=("forms", "restart")),
                xtk.button(text="开始默写", icon="Play", style=ACCENT, on_invoke=("forms", "start_round"),
                           focus=("forms-preview", a.focus_seq)),
                gap=8, justify=xtk.FlexJustify.END, fill=True))
    if fd.phase == "done":
        return fill_page(
            "forms-done", page_header("变形默写", None, restart),
            done_card("Flag", "全部完成！", f"共 {len(fd.entries)} 个词、{fd.total_forms} 个形式，默写 {fd.round} 轮",
                      xtk.button(text="再来一轮", icon="Refresh", style=ACCENT, on_invoke=("forms", "restart"),
                                 focus=("forms-done", a.focus_seq))))

    wi, fis = fd.current()
    entry = fd.entries[wi]
    done = fd.entry_done()
    lines = []
    if fd.round > 1 and 0 not in fis:  # 错题轮且原词没错：顶部显示原词作提示（不参与默写）
        lines.append(xtk.text(text=f"原词：{entry.forms[0].word}　{entry.meaning}", fontsize=14, fg=SECONDARY))
    for i, r in enumerate(fd.rows):
        f = entry.forms[r.fi]
        if r.fi == 0:
            hint = entry.meaning
        else:
            hint = f.pos + (f"（{f.note}）" if f.note else "")
            if fd.round > 1:
                hint = entry.meaning + " → " + hint  # 错题轮补上原释义作上下文
        if r.status == "correct":
            status, color = "✓ " + f.meaning, good(a)
        elif r.status == "wrong":
            status, color = "答错了，再试一次", bad(a)
        elif r.status == "revealed":
            status, color = f"原词：{f.word}　{f.meaning}", bad(a)
        else:
            status, color = "", SECONDARY
        live = i == fd.cur and r.status not in ("correct", "revealed")
        lines.append(row(
            xtk.text(text=hint, fontsize=16, w=200, textwrap=True),
            xtk.suggest(key=f"form-{fd.serial}-{i}", text=r.text, w=240, enabled=live,
                        placeholder="回车提交" if live else "",
                        focus=(fd.serial, fd.cur, a.focus_seq) if live else None,
                        on_change=lambda t, i=i: ("form_text", i, t), on_query=lambda t, k, i=i: ("form_enter", i, t)),
            xtk.text(text=status, fontsize=14, fontweight=SEMI, fg=color, grow=1, textwrap=True),
            gap=16, items=CENTER))

    cur_wrong = not done and fd.rows[fd.cur].status == "wrong"
    act = []
    if not done:
        act.append(xtk.button(text="提交", icon="Accept", style=ACCENT, on_invoke=("forms", "submit")))
        if not cur_wrong:
            act.append(xtk.button(text="不知道", icon="Help", style=SUBTLE, on_invoke=("forms", "give_up")))
    if cur_wrong or done:
        act.append(xtk.button(text="下一个", icon="Next", style=ACCENT if done else xtk.ButtonStyle.STANDARD,
                              on_invoke=("forms", "next_action"), focus=(fd.serial, "next", a.focus_seq) if done else None))

    where = f"第 {fd.pos} / {len(fd.queue)} 词" + (f" · 第 {fd.round} 轮（错词）" if fd.round > 1 else "")
    return fill_page(
        "forms",
        page_header("变形默写", "原词和各个变形各占一行：左边是提示，右边默写；答对自动跳到下一行，点「不知道」公布答案并记入错题。",
                    restart),
        column(row(xtk.text(text=where, fontsize=13, fontweight=SEMI), caption(f"共 {len(fd.entries)} 个词条"),
                   justify=xtk.FlexJustify.SPACE_BETWEEN, fill=True),
               xtk.progress(value=float(fd.pos - (0 if done else 1)), max=float(max(len(fd.queue), 1))),
               gap=8, items=STRETCH),
        xtk.card(column(*lines, key=f"forms-rows-{fd.serial}", gap=14, items=STRETCH),
                 xtk.infobar(text="本词完成", message="按回车或点「下一个」继续。", severity=SUCCESS) if done else None,
                 row(*act, gap=8, justify=xtk.FlexJustify.CENTER),
                 dir=COLUMN, gap=20, pl=32, pt=28, pr=32, pb=28, items=STRETCH, justify=xtk.FlexJustify.CENTER,
                 fill=True, grow=1))


# ------------------------------------------------------------ 词库 / 说明


def view_library(a: App) -> xtk.El:
    thr = a.judge.merge_threshold_or_zero()
    return fill_page(
        "library",
        page_header("词库", f"共 {len(a.wl)} 个词 · {a.wl.path}",
                    xtk.button(text="清空词库", icon="Delete", style=SUBTLE, enabled=len(a.wl) > 0, on_invoke=("clear_ask",))),
        feedback_bar(a.add_fb, ("add_fb_close",)),
        section("添加单词",
                "每行一条「单词 词性 释义」，单词可以是短语（如 account for v. 解释、导致；也可以用 Tab 或 = 隔开）；"
                "或者英文、中文一行一行交替；或者先写全部英文、再写对应的中文，两段数量一致。已在词库里的词自动跳过。",
                row(xtk.button(text="打开输入窗口", icon="Add", style=ACCENT, on_invoke=("add_open",)),
                    xtk.button(text="从剪贴板导入", icon="Paste", on_invoke=("add_clip",)),
                    xtk.button(text="正在检查…" if a.merging else "合并同义释义", icon="Link",
                               enabled=not a.merging and len(a.wl) > 0, on_invoke=("merge_all",),
                               tip="用智能判分检查整个词库：一词多义里意思相同的释义合并成一项") if thr > 0 else None,
                    gap=8)),
        xtk.card(subtitle("全部单词"), word_list(a, None, "all") if len(a.wl) else body("还没有词。"),
                 dir=COLUMN, gap=8, pl=20, pt=18, pr=20, pb=12, items=STRETCH, fill=True, grow=1),
        xtk.dialog(xtk.text(text=f"会删掉全部 {len(a.wl)} 个词，并删除词库文件。这一步不能撤销。", fontsize=14, textwrap=True),
                   key="clear-dlg", text="清空词库？", primary="清空", close="取消", open=a.clear_open, contenth=64,
                   on_close=lambda r: ("clear_result", r)),
        xtk.dialog(column(body("每行「单词或短语 词性 释义」，如 account for v. 解释、导致；"
                               "或者英文、中文一行一行交替；或者上半部分全英文、下半部分全中文。"),
                          xtk.textbox(key="add-box", text=a.add_text, multiline=True, h=300,
                                      focus=("add", a.add_open) if a.add_open else None,
                                      on_change=lambda t: ("add_text", t)),
                          gap=10, items=STRETCH),
                   key="add-dlg", text="添加单词", primary="提交", close="取消", open=a.add_open, contenth=360,
                   on_close=lambda r: ("add_result", r)))


HELP = [
    ("默写", "第 1 轮按词表顺序出全部词。答错可以再试；点「不知道」或「跳过」会公布答案，这个词记为错词。\n"
           "一轮结束后先看错词清单，下一轮只默错的词，直到全部答对。"),
    ("无尽模式", "词表按顺序每 20 个分一组，组内按原顺序出题。\n"
             "提交答错可以继续重试；点「不知道」才公布答案并触发本组重开：本组见过的词（之前答对的和这次错的）打乱顺序排在前面，"
             "没见过的词保持原顺序跟在后面，从头再答。\n"
             "一个词一次答对（没先提交答错）满 15 次就移出本组，从后面的词里按顺序补一个进来，本组保持 20 个；"
             "「不知道」不清零次数，从第 1 组重新开始才清零。\n"
             "整组一次不错地答完才进入下一组，上一组的词不再出现；所有组都过关才算完成。\n"
             "默写一轮结束的结果页可以点「错词进无尽模式」：这一轮的错词打乱后替换无尽模式的进度，点「换回全部词库」回到整个词库。"),
    ("变形默写", "一屏一个词条：原词和它的各个变形（名词、形容词、副词……）各占一行。原词行的提示是释义，变形行的提示是词性和语义标注。\n"
             "答对自动跳到下一行并显示这个形式的释义；答错可以再试，点「不知道」（答错后是「下一个」）公布答案并记入错题。"
             "一轮结束先看错题，错题轮重练直到全对。双拼写（如 fueled/fuelled）写哪个都算对。\n"
             "词条在数据目录的 dictation_forms.json 里，没有就用自带的一套。"),
    ("题型与判分", "释义 → 单词：看中文写英文，下方蓝色字母是开头提示，不分大小写。\n"
              "单词 → 释义：看英文写中文。有多个意思的词（释义里用 、 ; 或空格隔开）要把每个意思都写出来："
              "一次写一个，也可以一次写几个用 、 隔开；答出的会打勾，全部答出才算对，括号里的注释不用写。"
              "右上角取消勾选「多义词全答」后，写出任意一个意思就算对（开关会记住）。"
              "录入新词时，一词多义里意思相同的释义（如「缓解、减轻」）会用智能判分自动合并成「缓解（减轻）」，写其中任何一个都算答对这一项；"
              "已有的词库可以在词库页点「合并同义释义」。字面判不出时，会请本地小模型判断意思是否接近。\n"
              "短语按整条比对：不分大小写，多余的空格和 sth. 的点号不计较。"),
    ("快捷键", "输入框里按回车 = 提交；答完后按回车 = 下一个（无尽模式里是重新开始本组）。"),
]


def view_help() -> xtk.El:
    return xtk.scroll(
        column(page_header("说明", "几种练习各自保留进度，在左侧随时切换；关掉程序再打开会接着上次的进度。"),
               *(section(t, None, para(p)) for t, p in HELP),
               gap=16, pl=36, pt=28, pr=36, pb=28, items=STRETCH, shrink=0),
        key="help", dir=COLUMN, items=STRETCH, grow=1)


def view_page(a: App) -> xtk.El:
    if a.page == Page.ENDLESS:
        return view_drill(a, a.drill[1], True)
    if a.page == Page.FORMS:
        return view_forms(a)
    if a.page == Page.LIBRARY:
        return view_library(a)
    if a.page == Page.HELP:
        return view_help()
    return view_drill(a, a.drill[0], False)


def view(a: App) -> xtk.El:
    return xtk.nav(view_page(a), items=NAV_ITEMS, titlebar=True, title="单词默写", sel=int(a.page), grow=1,
                   on_select=lambda i: ("nav", i))


# ==================================================================== 入口


def main(argv: list[str] | None = None) -> int:
    """python -m dictation [词表.json [判分配置.json]]；数据文件默认在当前目录。"""
    argv = sys.argv[1:] if argv is None else argv
    words_path = os.path.abspath(argv[0] if argv else "dictation_extra.json")
    judge_path = os.path.abspath(argv[1] if len(argv) > 1 else "dictation_judge.json")
    data_dir = os.path.dirname(words_path)
    app = App(data_dir, words_path, judge_path)
    start = os.environ.get("DICT_START_PAGE", "")  # 截图 / 回归用：直接打开某一页
    if start.isdigit() and int(start) in (1, 2, 4, 5):
        app.page = Page(int(start))
    try:
        return xtk.run(app, update, view, title="单词默写", width=1080, height=780)
    finally:
        app.progress_save()
        _quit.set()
        app.judge.close()
