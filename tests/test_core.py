"""后端无头测试：判分规则、词表读写、出题状态机（普通模式 + 无尽模式）。从 C 版 test_dict_core.c 移植。

    python -m unittest discover -s tests

无尽模式的每条用例都对应 2026-10-06 ~ 10-08 会话里提出的一条需求。
"""

import os
import tempfile
import unittest

from dictation.session import GROUP_SIZE, RETIRE_HITS, Kind, Phase, Session
from dictation.text import answer_eq, is_blank, masked_display, meaning_match, senses_mark, senses_merge, split_senses
from dictation.words import WordList, add_text, split_pos_meaning, strip_notes


def ok_next(s: Session) -> None:
    """答对当前词并「下一个」。"""
    s.answer(True)
    s.next()


def is_perm_of_range(a: list[int], lo: int, hi: int) -> bool:
    return sorted(a) == list(range(lo, hi))


def reset_shape_ok(s: Session, seen_words: list[int]) -> bool:
    """重开后的队列：前 seen 个是组内见过词的一个排列，后面是没见过的词按原顺序。"""
    n = len(seen_words)
    if set(s.queue[:n]) != set(seen_words) or not is_perm_of_range(s.queue, s.gstart, s.gend):
        return False
    rest = s.queue[n:]
    return not set(rest) & set(seen_words) and rest == sorted(rest)


class TextTest(unittest.TestCase):
    def test_meaning_match(self):
        self.assertTrue(meaning_match("公正", "公正"))
        self.assertTrue(meaning_match("分类", "把...分类"), "子串")
        self.assertTrue(meaning_match("屈服", "屈服；让步"), "义项之一")
        self.assertTrue(meaning_match("让步", "屈服；让步"), "第二个义项")
        self.assertFalse(meaning_match("不合理的", "合理的"), "多了否定字不算包含")
        self.assertFalse(meaning_match("合理", "不合理的"), "少了否定字不算包含")
        self.assertFalse(meaning_match("不可能", "可能的、或许"), "义项也查否定字")
        self.assertFalse(meaning_match("重要", "不重要的"), "字的子集也查否定字")
        self.assertTrue(meaning_match("无用", "无用的、废物"), "两边都有否定字照常")
        self.assertTrue(meaning_match("废物", "无用的、废物"), "别的义项有否定字不影响")
        self.assertTrue(meaning_match("不合理的", "不合理的"))
        self.assertTrue(meaning_match("非常重要的", "重要的"), "「非常」不是否定")
        self.assertTrue(meaning_match("差不多", "几乎、差不多"), "「差不多」不是否定")
        self.assertFalse(meaning_match("悲观", "乐观"))
        self.assertFalse(meaning_match("", "公正"))
        self.assertFalse(meaning_match("abc", "公正"))

    def test_masked_display(self):
        self.assertEqual(masked_display("swear"), "s w _ _ _")
        self.assertEqual(masked_display("a"), "_")
        self.assertEqual(masked_display("account for"), "a c c _ _ _ _   f _ _")
        self.assertEqual(masked_display("well-known"), "w e l l - _ _ _ _ _")

    def test_answer_eq(self):
        self.assertTrue(answer_eq("Take sth  into account ", "take sth. into account"))
        self.assertFalse(answer_eq("take into account", "take sth. into account"))
        self.assertFalse(answer_eq("", ""))

    def test_split_senses(self):
        self.assertEqual(split_senses("行为、传导; 实施、传导"), ["行为", "传导", "实施"])
        self.assertEqual(split_senses("使沉淀、(水汽等)凝结(成雨、露、雪等)")[1:], ["(水汽等)凝结(成雨、露、雪等)"])
        self.assertEqual(split_senses("逆转、使 (决定、政策、趋势) 转向")[1:], ["使 (决定、政策、趋势) 转向"])
        sn = split_senses("媒介、手段 中等的")
        self.assertEqual((len(sn), sn[2]), (3, "中等的"), "汉字间空格拆")
        self.assertEqual(len(split_senses("（气候或地区)温和的")), 1, "括号不配对也不拆")

    def test_senses_mark(self):
        sn = split_senses("易得到的(8)、浅显易懂的(1)")
        got, fresh = senses_mark("易得到的", sn, 0)
        self.assertEqual((got, fresh), (1, 1), "括号注释不用写")
        self.assertEqual(senses_mark("易得到的", sn, got)[1], 0, "答过的不重复计")
        self.assertEqual(senses_mark("没关系", sn, got), (1, 0), "答错不置位")
        sn = split_senses("精心设计的、复杂的、详尽的; 详细解释、发展")
        self.assertEqual(senses_mark("发展，复杂的 详尽的", sn, 0)[1], 3, "一次写多个")

    def test_senses_merge(self):
        sn = split_senses("标志、目标、标记、记号")
        self.assertIsNone(senses_merge(sn, set()), "没有同义的不改")
        merged = senses_merge(sn, {(0, 2), (2, 3)})  # 标志≈标记、标记≈记号 → 连通，三条合一
        self.assertEqual(merged, "标志（标记、记号）、目标")
        sn = split_senses(merged)
        self.assertEqual(len(sn), 2)
        got, fresh = senses_mark("记号", sn, 0)
        self.assertEqual((got, fresh), (1, 1), "写组里任何一条都算")
        self.assertEqual(senses_mark("标志", sn, got)[1], 0, "同一组不重复计")
        self.assertEqual(senses_mark("目标", sn, got), (3, 1))
        sn = split_senses("缓解、救济、减轻")
        self.assertEqual(senses_merge(sn, {(2, 0)}), "缓解（减轻）、救济", "只在后一条上标也算，保持先后顺序")

    def test_blank(self):
        self.assertTrue(is_blank(" ，。 "))
        self.assertFalse(is_blank("x"))


class WordsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "测试词表_test.json")  # 中文文件名
        with open(self.path, "w", encoding="utf-8") as f:
            f.write('[\n  ["zeal", "n.", "热情"],\n  ["equity", "n.", "公正 [法]"]\n]\n')

    def tearDown(self):
        self.dir.cleanup()

    def test_all(self):
        wl = WordList.load(self.path)
        self.assertEqual(len(wl), 2)
        self.assertEqual(wl.words[1].meaning, "公正 [法]", "释义里的 ] 不能截断")

        st = add_text(wl, "tangible adj. 有形的\ntedious adj. 乏味的\nzeal n. 热情\n")
        self.assertEqual((st.added, st.dup, st.err), (2, 1, ""))

        st = add_text(wl, "account for v. 解释、导致\ntake sth. into account 考虑到\nset off: 出发\nlook up\tv.\t查阅\n")
        self.assertEqual((st.added, st.err), (4, ""))
        w = wl.words
        self.assertEqual((w[-4].word, w[-4].pos, w[-4].meaning), ("account for", "v.", "解释、导致"))
        self.assertEqual(w[-3].word, "take sth. into account")
        self.assertEqual(w[-2].word, "set off", "分隔符去掉")
        self.assertEqual((w[-1].word, w[-1].pos), ("look up", "v."), "Tab 分列")
        st = add_text(wl, "conduct n. v. 行为; 实施\n")
        self.assertEqual((st.added, wl.words[-1].pos), (1, "n. v."), "多个词性")

        st = add_text(wl, "withstand\nyield\n承受\n屈服；让步\n")
        self.assertEqual((st.added, st.err), (2, ""), "两段式")

        st = add_text(wl, "rigid\nswirl\n僵硬的\n")
        self.assertEqual(st.added, 0)
        self.assertIn("数量不一致", st.err)
        st = add_text(wl, "rigid\n僵硬的\nswirl\n")
        self.assertEqual(st.added, 0)
        self.assertIn("格式混杂", st.err)

        st = add_text(wl, "give up\n放弃\nturn  down\nv. 拒绝、调低\n")
        self.assertEqual((st.added, wl.words[-1].word, wl.words[-1].pos), (2, "turn down", "v."), "英中交替")

        self.assertEqual(split_pos_meaning("n. 过程 v. 处理"), ("n. v.", "过程 处理"))
        self.assertEqual(split_pos_meaning("存货 & 库存"), ("", "存货 库存"))

        again = WordList.load(self.path)  # 追加后文件可重新读回、原有内容保留
        self.assertEqual(len(again), 13)
        self.assertEqual((again.words[0].word, again.words[12].word), ("zeal", "turn down"))
        self.assertEqual(again.words[10].meaning, "屈服；让步")

        wl.words[0].meaning = "热情（热心）"  # 改释义后整表写回
        self.assertTrue(wl.save())
        again = WordList.load(self.path)
        self.assertEqual((len(again), again.words[0].meaning, again.words[1].meaning), (13, "热情（热心）", "公正 [法]"))

        for src, want in [
            ("忘记 获利丰厚的", "获利丰厚的"),
            ("少词义 发展 精心设计的、复杂的、详尽的; 详细解释、发展", "精心设计的、复杂的、详尽的; 详细解释、发展"),
            ("获利丰厚的 （忘了）", "获利丰厚的"),
            ("遗忘、记不住 毁灭", "毁灭"),
            ("少词义 解决、分辨", "解决、分辨"),
            ("忘记", "忘记"),  # forget：只有这一个意思，不动
            ("忘记、遗忘", "忘记、遗忘"),  # 都是标注词、没有别的义项
            ("遗忘、忘却", "遗忘、忘却"),  # oblivion：其他义项也跟「忘」沾边
            ("健忘的 忘记", "健忘的 忘记"),
            ("复杂难懂得", "复杂难懂得"),
            ("错觉", "错觉"),
        ]:
            self.assertEqual(strip_notes(src), want, src)
        st = add_text(wl, "lucrative adj. 忘记 获利丰厚的\n")
        self.assertEqual((st.added, wl.words[-1].meaning), (1, "获利丰厚的"), "录入时去标注")

        wl.clear()
        self.assertEqual(len(wl), 0)
        self.assertFalse(os.path.exists(self.path), "清空后文件应被删除")


class RoundsTest(unittest.TestCase):
    def test_rounds(self):
        s = Session()
        s.seed(42)
        s.start_rounds(4)
        self.assertEqual((s.current(), s.phase), (0, Phase.ASKING), "按词表顺序出题")

        s.answer(False)
        self.assertEqual(s.phase, Phase.WRONG)
        s.answer(True)
        self.assertEqual((s.phase, s.wlen), (Phase.CORRECT, 0), "答错可再试，再答对算对")
        self.assertEqual(s.next(), -1)
        self.assertEqual(s.current(), 1)

        s.answer(False)  # 答错后直接「下一个」= 记错词并公布上一题
        self.assertEqual((s.next(), s.wlen, s.current()), (1, 1, 2))

        s.dont_know()
        self.assertEqual((s.phase, s.wlen), (Phase.REVEALED, 2))
        s.next()
        self.assertEqual(s.current(), 3)

        ok_next(s)  # 一轮结束有错词 → 预览 → 下一轮只默错词
        self.assertEqual((s.phase, s.wlen), (Phase.PREVIEW, 2))
        s.next_round()
        self.assertEqual((s.round, s.queue), (2, [1, 2]))

        ok_next(s)  # 重新开始（第 2 轮内）= 重开本轮
        s.restart()
        self.assertEqual((s.round, s.qlen, s.current()), (2, 2, 1))

        ok_next(s)
        ok_next(s)
        self.assertEqual(s.phase, Phase.DONE)
        s.restart()
        self.assertEqual((s.round, s.qlen, s.current()), (1, 4, 0), "全对 → 完成 → 再来一轮从头开始")

        s.start_rounds(0)
        self.assertEqual((s.phase, s.current()), (Phase.EMPTY, -1))
        s.dont_know()
        s.next()
        self.assertEqual(s.phase, Phase.EMPTY, "空表下按钮不能改状态")


class EndlessTest(unittest.TestCase):
    def test_endless(self):
        s = Session()
        s.seed(20261010)

        # 独立按钮、随时进入 —— 普通模式中途进入，从第 1 组第 1 个词开始
        s.start_rounds(45)
        ok_next(s)
        ok_next(s)
        s.dont_know()
        s.start_endless(45)
        self.assertEqual((s.kind, s.group, s.current(), s.phase), (Kind.ENDLESS, 0, 0, Phase.ASKING))

        # 每 20 个一组 —— 45 个词分成 20/20/5 三组，组内按原顺序
        self.assertEqual((s.ngroups(), s.qlen), (3, 20))
        self.assertEqual(s.queue, list(range(20)))

        # 第一个词就错 → 显示答案 + 重新开始 → 还是从第一个词开始
        s.dont_know()
        self.assertTrue(s.phase == Phase.REVEALED and s.failed())
        self.assertEqual(s.current(), 0)
        s.next()
        self.assertEqual((s.current(), s.queue[1], s.restarts), (0, 1, 1))

        # 提交答错只允许重试，不触发重开
        s.answer(False)
        self.assertTrue(s.phase == Phase.WRONG and not s.failed())
        before = s.pos
        s.next()
        self.assertEqual((s.pos, s.restarts), (before, 1), "提交答错后不能跳题或重开")
        s.answer(True)
        s.next()

        # 错了以后点「重新开始」必定重置
        s.start_endless(45)
        for _ in range(3):
            ok_next(s)
        s.dont_know()
        s.next()
        self.assertEqual((s.pos, s.phase, s.restarts), (0, Phase.ASKING, 1))

        # 重开范围 = 之前答对的 + 这次错的，打乱放前面；没见过的词保持原顺序放后面
        self.assertTrue(reset_shape_ok(s, [0, 1, 2, 3]), s.queue)
        self.assertEqual((s.queue[4], s.queue[5], s.queue[19]), (4, 5, 19))
        self.assertNotEqual(s.queue[0], 3, "见过的词不止一个时，刚错的词不放第一个")

        # 重开后没答到的「见过的词」也算见过
        s.dont_know()
        s.next()
        self.assertTrue(reset_shape_ok(s, [0, 1, 2, 3]) and s.seen == 4)

        # 打乱是真随机 —— 2000 次重开
        pos_hits, first_hits = [0] * 8, [0] * 8
        distinct, last = 0, None
        for _ in range(2000):
            s.start_endless(45)
            for _ in range(5):
                ok_next(s)
            s.dont_know()
            s.next()
            for i in range(6):
                if s.queue[i] == 5:
                    pos_hits[i] += 1
            first_hits[s.queue[0]] += 1
            if s.queue[:6] != last:
                distinct += 1
            last = s.queue[:6]
        self.assertEqual(pos_hits[0], 0)
        self.assertTrue(all(300 < pos_hits[i] < 500 for i in range(1, 6)), pos_hits)
        self.assertGreater(distinct, 1900)
        self.assertTrue(all(first_hits[i] > 0 for i in range(5)))

        # 本组一次不错地答完 → 进入下一组
        s.start_endless(45)
        s.dont_know()
        s.next()
        for _ in range(20):
            ok_next(s)
        self.assertEqual((s.group, s.gstart, s.gend, s.restarts, s.seen), (1, 20, 40, 0, 0))
        self.assertEqual(s.queue, list(range(20, 40)))
        for _ in range(7):
            ok_next(s)
        s.dont_know()
        s.next()
        self.assertTrue(is_perm_of_range(s.queue, 20, 40), "第二组重开不能混进第一组的词")

        # 全部小组都过了才结束（20/20/5）
        s.start_endless(45)
        for _ in range(40):
            ok_next(s)
        self.assertEqual((s.group, s.qlen, s.phase), (2, 5, Phase.ASKING))
        for _ in range(4):
            ok_next(s)
        self.assertEqual(s.phase, Phase.ASKING)
        ok_next(s)
        self.assertEqual(s.phase, Phase.DONE)

        s.restart()
        self.assertEqual((s.kind, s.group, s.current()), (Kind.ENDLESS, 0, 0))
        s.start_rounds(45)
        self.assertEqual((s.kind, s.qlen), (Kind.ROUNDS, 45))

        # 无尽模式中途加词：新词进后面的组
        s.start_endless(33)
        for _ in range(20):
            ok_next(s)
        self.assertEqual((s.gstart, s.gend), (20, 33))
        s.words_changed(50)
        self.assertEqual(s.ngroups(), 3)
        for _ in range(13):
            ok_next(s)
        self.assertEqual((s.gstart, s.gend), (33, 50))

        # 切换题型：已点不知道的不撤销，提交答错可重新作答
        s.start_endless(45)
        s.answer(True)
        s.reask()
        self.assertEqual(s.phase, Phase.ASKING)
        s.answer(False)
        self.assertEqual(s.phase, Phase.WRONG)
        s.reask()
        self.assertEqual(s.phase, Phase.ASKING)
        s.dont_know()
        s.reask()
        self.assertEqual(s.phase, Phase.REVEALED)

    def test_pool(self):
        s = Session()
        s.seed(20261107)
        wrong = [100 + i * 3 for i in range(25)]
        s.start_endless_pool(200, wrong)
        self.assertEqual((s.kind, len(s.pool), s.endless_total()), (Kind.ENDLESS, 25, 25))
        self.assertEqual((s.ngroups(), s.qlen), (2, 20))
        self.assertTrue(all(q >= 100 and (q - 100) % 3 == 0 for q in s.queue), "只能出错词")
        self.assertNotEqual(s.queue, wrong[:20], "要打乱")

        s.words_changed(230)  # 中途加的新词不进来
        seen, steps = [], 0
        while s.phase != Phase.DONE and steps < 200:
            steps += 1
            c = s.current()
            if c >= 0 and c not in seen:
                seen.append(c)
            ok_next(s)
        self.assertEqual((s.phase, len(seen)), (Phase.DONE, 25))
        self.assertTrue(all(c < 200 for c in seen))

        s.restart()
        self.assertEqual((len(s.pool), s.group, s.qlen, s.phase), (25, 0, 20, Phase.ASKING))
        s.start_endless(230)
        self.assertEqual((len(s.pool), s.endless_total(), s.queue[0]), (0, 230, 0))

        s.start_endless_pool(200, [5, 5, 7, 7, 9])
        self.assertEqual(len(s.pool), 3, "重复的下标只算一次")
        s.words_changed(0)
        self.assertEqual((len(s.pool), s.phase), (0, Phase.EMPTY))

    def test_retire(self):
        s = Session()
        s.seed(20261114)
        s.start_endless(45)
        for _ in range(14):  # 答对词 0 → 词 1 点不知道 → 重开（刚错的不放第一个，所以又是词 0）
            ok_next(s)
            s.dont_know()
            s.next()
        self.assertEqual((s.current(), s.hits.get(0, 0)), (0, 14))
        ok_next(s)
        self.assertEqual((s.retired, s.joined), (0, 20))
        self.assertTrue(s.qlen == 20 and 0 not in s.queue and s.queue[19] == 20, s.queue)
        self.assertEqual((s.pos, s.current(), s.seen, s.phase), (0, 1, 1, Phase.ASKING))
        self.assertEqual((s.gend, s.ngroups(), s.mastered(), s.endless_passed()), (21, 3, 1, 1))

        c = s.current()  # 答对当场就计上；换题型再答一遍不重复计
        h0, t0 = s.hits.get(c, 0), s.total_hits()
        s.answer(True)
        self.assertEqual((s.hits.get(c, 0), s.total_hits()), (h0 + 1, t0 + 1))
        s.reask()
        s.answer(True)
        self.assertEqual(s.hits.get(c, 0), h0 + 1)
        s.next()
        self.assertEqual(s.hits.get(c, 0), h0 + 1)
        s.restart()
        for _ in range(14):
            ok_next(s)
            s.dont_know()
            s.next()
        ok_next(s)

        cur = s.current()  # 提交答错再答对不算一次答对；「不知道」不清零
        h = s.hits.get(cur, 0)
        s.answer(False)
        s.answer(True)
        s.next()
        self.assertEqual((s.hits.get(cur, 0), h, s.retired), (h, 0, -1))
        s.restart()
        ok_next(s)
        s.dont_know()
        s.next()
        self.assertEqual(s.hits.get(0, 0), 1)

        s.restart()  # 整个无尽模式重新开始才清零
        self.assertEqual((s.hits.get(0, 0), s.hits.get(1, 0), s.mastered(), s.gend, s.queue[0]), (0, 0, 0, 20, 0))

        s.start_endless(21)  # 后面没有新词就不补，本组变少；全部移出算本组过关 → 完成
        s.hits[0] = 14
        ok_next(s)
        self.assertEqual((s.joined, s.qlen, s.gend), (20, 20, 21))
        s.hits[s.current()] = 14
        ok_next(s)
        self.assertTrue(s.retired >= 0 and s.joined == -1 and s.qlen == 19)
        for i in range(21):
            if s.hits.get(i, 0) < RETIRE_HITS:
                s.hits[i] = 14
        steps = 0
        while s.phase != Phase.DONE and steps < 100:
            steps += 1
            ok_next(s)
        self.assertEqual((s.phase, s.qlen, s.mastered(), s.endless_passed()), (Phase.DONE, 0, 21, 21))

        wrong = [100 + i * 3 for i in range(25)]  # 错词池里也一样，从词池后面补
        s.start_endless_pool(200, wrong)
        s.hits[s.current()] = 14
        ok_next(s)
        self.assertEqual((s.joined, s.queue[19], s.qlen), (s.pool[20], s.pool[20], 20))

    def test_fuzz(self):
        """随机「学生」跑完整个无尽模式 300 局，每一步检查不变量。"""
        s = Session()
        for game in range(300):
            n = 1 + game % 67
            s.seed(1000 + game)
            s.start_endless(n)
            r = (0x12345678 + game) & ((1 << 64) - 1)
            steps = 0
            while s.phase != Phase.DONE and steps < 200000:
                steps += 1
                retired = sum(1 for i in range(s.gstart, s.gend) if s.hits.get(i, 0) >= RETIRE_HITS)
                self.assertEqual(s.qlen + retired, s.gend - s.gstart, (game, s.queue))
                self.assertLessEqual(s.qlen, GROUP_SIZE)
                self.assertEqual(len(set(s.queue)), s.qlen)
                self.assertTrue(all(s.gstart <= q < s.gend and s.hits.get(q, 0) < RETIRE_HITS for q in s.queue))
                cur = s.current()
                self.assertTrue(s.gstart <= cur < s.gend)
                r = (r * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
                if (r >> 33) % 100 < 93:
                    ok_next(s)
                    continue
                seen_before = max(s.seen, s.pos + 1)
                if (r >> 40) & 1:
                    s.dont_know()
                    self.assertTrue(s.failed())
                    s.next()
                    self.assertEqual((s.pos, s.seen), (0, seen_before))
                else:
                    s.answer(False)
                    self.assertTrue(s.phase == Phase.WRONG and not s.failed())
                    s.answer(True)
                    s.next()
                rest = s.queue[s.seen:]
                self.assertEqual(rest, sorted(rest), "没见过的词保持原顺序")
            self.assertEqual((s.phase, s.gend), (Phase.DONE, n), game)

    def test_json_roundtrip(self):
        s = Session()
        s.seed(7)
        s.start_endless(45)
        for _ in range(5):
            ok_next(s)
        s.dont_know()
        t = Session.from_json(s.to_json())
        self.assertEqual(t.to_json(), s.to_json())
        s.next()
        t.next()
        self.assertEqual(t.queue, s.queue, "随机数状态也要存下来")


if __name__ == "__main__":
    unittest.main()
