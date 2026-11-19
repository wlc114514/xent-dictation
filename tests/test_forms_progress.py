"""变形默写状态机 + 进度存档（含 C 版 .bin 迁移）。"""

import json
import os
import struct
import tempfile
import unittest

from dictation import progress
from dictation.forms import Entry, Form, FormsDrill, form_ok, load_entries
from dictation.session import Kind, Phase, Session


def entries() -> list[Entry]:
    return [
        Entry("v. 施加", [Form("exert", "v.", "施加", ""), Form("exertion", "n.", "努力", "")]),
        Entry("v. 燃烧", [Form("burn", "v.", "燃烧", ""), Form("burned/burnt", "adj.", "烧伤的", "过去分词")]),
    ]


class FormsTest(unittest.TestCase):
    def test_form_ok(self):
        self.assertTrue(form_ok(" Burnt ", "burned/burnt"))
        self.assertTrue(form_ok("burned", "burned/burnt"))
        self.assertFalse(form_ok("", "burned/burnt"))
        self.assertFalse(form_ok("burn", "burned/burnt"))

    def test_bundled(self):
        es = load_entries(None)
        self.assertGreater(len(es), 50)
        self.assertTrue(all(e.forms and e.forms[0].word for e in es))

    def test_round_flow(self):
        fd = FormsDrill(entries())
        self.assertEqual((fd.phase, fd.current(), fd.cur), ("asking", (0, [0, 1]), 0))
        fd.set_text(0, "exert")
        fd.submit()
        self.assertEqual((fd.rows[0].status, fd.cur), ("correct", 1), "答对跳下一行")
        fd.set_text(0, "改不了")  # 已答完的行不能再改
        self.assertEqual(fd.rows[0].text, "exert")
        fd.set_text(1, "exersion")
        fd.submit()
        self.assertEqual((fd.rows[1].status, fd.cur), ("wrong", 1), "答错可再试")
        fd.next_action()  # 答错后「下一个」= 放弃
        self.assertEqual((fd.rows[1].status, fd.rows[1].text, fd.wrong), ("revealed", "exertion", [(0, 1)]))
        self.assertTrue(fd.entry_done())
        fd.submit()  # 词条完成后回车 = 下一个
        self.assertEqual(fd.current(), (1, [0, 1]))
        fd.give_up()
        fd.set_text(1, "burnt")
        fd.submit()
        fd.next_action()
        self.assertEqual((fd.phase, fd.wrong), ("preview", [(0, 1), (1, 0)]))

        fd.start_round()  # 错题轮：同词条合并，原词没错就只默变形
        self.assertEqual((fd.round, fd.queue, fd.current()), (2, [(0, [1]), (1, [0])], (0, [1])))
        fd.set_text(0, "exertion")
        fd.submit()
        fd.restart()  # 重开本轮
        self.assertEqual((fd.round, fd.pos, fd.wrong), (2, 1, []))
        for word in ("exertion", None, "burn"):
            if word is None:
                fd.submit()
                continue
            fd.set_text(0, word)
            fd.submit()
        fd.submit()
        self.assertEqual(fd.phase, "done")
        fd.restart()  # 全部完成后回到第 1 轮
        self.assertEqual((fd.round, fd.current()), (1, (0, [0, 1])))

    def test_save_restore(self):
        fd = FormsDrill(entries())
        fd.give_up()
        fd.give_up()
        fd.submit()
        saved = json.loads(json.dumps(fd.to_json()))
        again = FormsDrill(entries())
        self.assertTrue(again.restore(saved))
        self.assertEqual((again.current(), again.wrong, again.cur), ((1, [0, 1]), [(0, 0), (0, 1)], 0))
        self.assertFalse(FormsDrill(entries()[:1]).restore(saved), "词条换了就不读")

    def test_empty(self):
        self.assertEqual(FormsDrill([]).phase, "empty")


def c_progress(sessions: list[Session], counts: list[tuple[int, int]], nwords: int, direction: int) -> bytes:
    """按 C 版布局（2026-11-14 起）写一个 dictation_progress.bin。"""
    size = progress._SIZE
    out = bytearray(struct.pack("<IIii", progress._MAGIC, size, nwords, direction))
    o = progress._OFF
    for s, (answered, correct) in zip(sessions, counts):
        rec = bytearray(size + 8)

        def i32(name, v):
            struct.pack_into("<i", rec, o[name], v)

        def arr(name, a):
            struct.pack_into(f"<{len(a)}i", rec, o[name], *a)

        for name in ("kind", "phase", "nwords", "pos", "round", "group_size", "group", "gstart", "gend", "seen",
                     "restarts", "retired", "joined"):
            i32(name, int(getattr(s, name)))
        arr("queue", s.queue)
        i32("qlen", len(s.queue))
        arr("round_words", s.round_words)
        i32("rwlen", len(s.round_words))
        arr("wrong", s.wrong)
        i32("wlen", len(s.wrong))
        arr("pool", s.pool)
        i32("npool", len(s.pool))
        arr("hits", [s.hits.get(i, 0) for i in range(s.nwords)])
        i32("fumbled", int(s.fumbled))
        struct.pack_into("<Q", rec, o["rng"], s.rng)
        struct.pack_into("<2i", rec, size, answered, correct)
        out += rec
    return bytes(out)


class ProgressTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.json = os.path.join(self.dir.name, "dictation_progress.json")
        self.bin = os.path.join(self.dir.name, "dictation_progress.bin")

    def tearDown(self):
        self.dir.cleanup()

    def sessions(self) -> list[Session]:
        a, b = Session(), Session()
        a.seed(1)
        b.seed(2)
        a.start_rounds(30)
        for _ in range(30):
            a.dont_know() if a.current() % 4 == 0 else a.answer(True)
            a.next()
        b.start_endless(30)
        for _ in range(5):
            b.answer(True)
            b.next()
        b.dont_know()
        return [a, b]

    def test_migrate_bin(self):
        a, b = self.sessions()
        self.assertEqual(a.phase, Phase.PREVIEW)
        with open(self.bin, "wb") as f:
            f.write(c_progress([a, b], [(30, 22), (6, 5)], 30, 1))
        data = progress.load(self.json, self.bin, 30)
        self.assertIsNotNone(data)
        self.assertTrue(os.path.exists(self.json), "转换后写成 JSON")
        self.assertTrue(os.path.exists(self.bin), ".bin 留着不动")
        self.assertEqual((data["nwords"], data["dir"]), (30, 1))
        ra, rb = (Session.from_json(d["session"]) for d in data["drills"])
        self.assertEqual((ra.kind, ra.phase, ra.wrong), (Kind.ROUNDS, Phase.PREVIEW, a.wrong))
        self.assertEqual((rb.kind, rb.phase, rb.queue, rb.hits, rb.rng), (Kind.ENDLESS, b.phase, b.queue, b.hits, b.rng))
        self.assertEqual([(d["answered"], d["correct"]) for d in data["drills"]], [(30, 22), (6, 5)])
        os.remove(self.bin)  # 第二次直接读 JSON
        self.assertEqual(progress.load(self.json, self.bin, 30), data)

    def test_reject(self):
        a, b = self.sessions()
        with open(self.bin, "wb") as f:
            f.write(c_progress([a, b], [(0, 0), (0, 0)], 30, 0))
        self.assertIsNone(progress.load(self.json, self.bin, 20), "词表变短了（换了词表）不迁移")
        with open(self.bin, "wb") as f:
            f.write(b"garbage" * 10)
        self.assertIsNone(progress.load(self.json, self.bin, 30))
        self.assertFalse(os.path.exists(self.json))


if __name__ == "__main__":
    unittest.main()
