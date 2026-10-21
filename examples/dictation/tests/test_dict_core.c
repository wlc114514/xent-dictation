/**
 * @file test_dict_core.c
 * @brief 后端无头测试：判分规则、词表读写、出题状态机（普通模式 + 无尽模式）。
 *
 *   xmake build test_dict_core && xmake run test_dict_core
 *
 * 无尽模式的每条用例都对应 2026-10-06 ~ 10-08 会话里提出的一条需求。
 */
#include "dict_session.h"
#include "dict_text.h"
#include "dict_words.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;

#define CHECK(cond, ...)                                              \
	do {                                                              \
		if (cond) g_pass++;                                           \
		else {                                                        \
			g_fail++;                                                 \
			printf("  FAIL %s:%d  %s\n      ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                                      \
			printf("\n");                                             \
		}                                                             \
	} while (0)

/* 失败时顺带打印当前队列 */
#define CHECKQ(cond, msg)                                                   \
	do {                                                                    \
		CHECK(cond, "%s", msg);                                             \
		if (!(cond)) { printf("      "); dump_queue(&S); printf("\n"); }    \
	} while (0)

#define CASE(name) printf("[%s]\n", name)

static Session S;   /* 体积大，放静态区 */

/* 答对当前词并「下一个」 */
static void ok_next(Session *s)
{
	sess_answer(s, true);
	sess_next(s);
}

static bool is_perm_of_range(const int *a, int n, int lo, int hi)
{
	if (n != hi - lo) return false;
	static char seen[DICT_MAX_WORDS];
	memset(seen, 0, sizeof seen);
	for (int i = 0; i < n; i++) {
		if (a[i] < lo || a[i] >= hi || seen[a[i]]) return false;
		seen[a[i]] = 1;
	}
	return true;
}

static void dump_queue(const Session *s)
{
	printf("queue=[");
	for (int i = 0; i < s->qlen; i++) printf("%d%s", s->queue[i], i + 1 < s->qlen ? "," : "");
	printf("] pos=%d seen=%d", s->pos, s->seen);
}

/* ================================================================== */
/* 文本 / 判分                                                         */
/* ================================================================== */

static void test_text(void)
{
	CASE("判分规则 meaning_match");
	CHECK(meaning_match("公正", "公正"), "完全相同");
	CHECK(meaning_match("分类", "把...分类"), "子串");
	CHECK(meaning_match("屈服", "屈服；让步"), "义项之一");
	CHECK(meaning_match("让步", "屈服；让步"), "第二个义项");
	CHECK(!meaning_match("悲观", "乐观"), "意思相反");
	CHECK(!meaning_match("", "公正"), "空答案");
	CHECK(!meaning_match("abc", "公正"), "没有汉字");

	CASE("首字母提示 masked_display");
	char m[64];
	masked_display("swear", m, sizeof m);
	CHECK(strcmp(m, "s w _ _ _") == 0, "得到 [%s]", m);
	masked_display("a", m, sizeof m);
	CHECK(strcmp(m, "_") == 0, "得到 [%s]", m);
	masked_display("account for", m, sizeof m);
	CHECK(strcmp(m, "a c c _ _ _ _   f _ _") == 0, "短语逐词提示，得到 [%s]", m);
	masked_display("well-known", m, sizeof m);
	CHECK(strcmp(m, "w e l l - _ _ _ _ _") == 0, "连字符照常显示，得到 [%s]", m);

	CASE("短语判对 answer_eq");
	CHECK(answer_eq("Take sth  into account ", "take sth. into account"), "大小写/多余空格/点号不计");
	CHECK(!answer_eq("take into account", "take sth. into account"), "少词不算对");
	CHECK(!answer_eq("", ""), "空答案不算对");

	CASE("一词多义 · 拆义项");
	char sn[DICT_MAX_SENSES][DICT_SENSE_LEN];
	int  ns = split_senses("行为、传导; 实施、传导", sn, DICT_MAX_SENSES);
	CHECK(ns == 3 && !strcmp(sn[0], "行为") && !strcmp(sn[1], "传导") && !strcmp(sn[2], "实施"), "ns=%d", ns);
	ns = split_senses("使沉淀、(水汽等)凝结(成雨、露、雪等)", sn, DICT_MAX_SENSES);
	CHECK(ns == 2 && !strcmp(sn[1], "(水汽等)凝结(成雨、露、雪等)"), "括号里的顿号不拆：ns=%d [%s]", ns, sn[1]);
	ns = split_senses("逆转、使 (决定、政策、趋势) 转向", sn, DICT_MAX_SENSES);
	CHECK(ns == 2 && !strcmp(sn[1], "使 (决定、政策、趋势) 转向"), "ns=%d [%s]", ns, sn[1]);
	ns = split_senses("媒介、手段 中等的", sn, DICT_MAX_SENSES);
	CHECK(ns == 3 && !strcmp(sn[2], "中等的"), "汉字间空格拆：ns=%d", ns);
	ns = split_senses("（气候或地区)温和的", sn, DICT_MAX_SENSES);
	CHECK(ns == 1, "括号不配对也不拆：ns=%d", ns);

	CASE("一词多义 · 逐条判");
	ns = split_senses("易得到的(8)、浅显易懂的(1)", sn, DICT_MAX_SENSES);
	uint32_t got = 0;
	CHECK(senses_mark("易得到的", sn, ns, &got) == 1 && got == 1u, "括号注释不用写：got=%u", got);
	CHECK(senses_mark("易得到的", sn, ns, &got) == 0, "答过的不重复计");
	CHECK(senses_mark("没关系", sn, ns, &got) == 0 && got == 1u, "答错不置位");
	ns  = split_senses("精心设计的、复杂的、详尽的; 详细解释、发展", sn, DICT_MAX_SENSES);
	got = 0;
	CHECK(senses_mark("发展，复杂的 详尽的", sn, ns, &got) == 3, "一次写多个：got=%u", got);

	CASE("一词多义 · 合并同义义项");
	char     merged[DICT_MAX_MEANING];
	uint32_t link[DICT_MAX_SENSES] = {0};
	ns = split_senses("标志、目标、标记、记号", sn, DICT_MAX_SENSES);
	CHECK(!senses_merge(sn, ns, link, merged, sizeof merged), "没有同义的不改");
	link[0] = 1u << 2;   /* 标志≈标记 */
	link[2] = 1u << 3;   /* 标记≈记号 → 连通，三条合一 */
	CHECK(senses_merge(sn, ns, link, merged, sizeof merged) && !strcmp(merged, "标志（标记、记号）、目标"), "[%s]", merged);
	ns  = split_senses(merged, sn, DICT_MAX_SENSES);
	got = 0;
	CHECK(ns == 2, "合并后只剩 2 项：ns=%d", ns);
	CHECK(senses_mark("记号", sn, ns, &got) == 1 && got == 1u, "写组里任何一条都算：got=%u", got);
	CHECK(senses_mark("标志", sn, ns, &got) == 0, "同一组不重复计");
	CHECK(senses_mark("目标", sn, ns, &got) == 1 && got == 3u, "got=%u", got);
	memset(link, 0, sizeof link);
	ns = split_senses("缓解、救济、减轻", sn, DICT_MAX_SENSES);
	link[2] = 1u << 0;   /* 只在后一条上标也算 */
	CHECK(senses_merge(sn, ns, link, merged, sizeof merged) && !strcmp(merged, "缓解（减轻）、救济"), "保持先后顺序：[%s]", merged);

	CASE("其它工具");
	CHECK(str_ci_eq("Equity", "equity"), "大小写不敏感");
	CHECK(!str_ci_eq("equit", "equity"), "长度不同");
	CHECK(is_blank(" ，。 "), "纯标点算空白");
	CHECK(!is_blank("x"), "字母不算空白");
	char out[64];
	js_str("\"\\u516c\\u6b63 \\\"ok\\\"\"", out, sizeof out);
	CHECK(strcmp(out, "公正 \"ok\"") == 0, "得到 [%s]", out);
}

/* ================================================================== */
/* 词表                                                                */
/* ================================================================== */

static WordList WL;

static void write_file(const char *path, const char *text)
{
	FILE *f = u8_fopen(path, "wb");
	fputs(text, f);
	fclose(f);
}

static void test_words(void)
{
	const char *path = "测试词表_test_dict_core.json";   /* 中文文件名：顺带测 UTF-8 路径 */
	write_file(path, "[\n  [\"zeal\", \"n.\", \"热情\"],\n  [\"equity\", \"n.\", \"公正 [法]\"]\n]\n");

	CASE("读词表");
	CHECK(words_load(&WL, path), "读取失败");
	CHECK(WL.n == 2, "n=%d", WL.n);
	CHECK(strcmp(WL.words[1].meaning, "公正 [法]") == 0, "释义里的 ] 不能截断：[%s]", WL.words[1].meaning);

	AddStat st;
	CASE("批量导入 · 单行式");
	words_add_text(&WL, "tangible adj. 有形的\ntedious adj. 乏味的\nzeal n. 热情\n", &st);
	CHECK(st.added == 2 && st.dup == 1 && !st.err[0], "added=%d dup=%d err=%s", st.added, st.dup, st.err);

	CASE("批量导入 · 短语");
	words_add_text(&WL, "account for v. 解释、导致\ntake sth. into account 考虑到\nset off: 出发\nlook up\tv.\t查阅\n", &st);
	CHECK(st.added == 4 && !st.err[0], "added=%d bad=%d err=%s", st.added, st.bad, st.err);
	CHECK(!strcmp(WL.words[WL.n - 4].word, "account for") && !strcmp(WL.words[WL.n - 4].pos, "v.")
	      && !strcmp(WL.words[WL.n - 4].meaning, "解释、导致"), "[%s][%s][%s]",
	      WL.words[WL.n - 4].word, WL.words[WL.n - 4].pos, WL.words[WL.n - 4].meaning);
	CHECK(!strcmp(WL.words[WL.n - 3].word, "take sth. into account"), "[%s]", WL.words[WL.n - 3].word);
	CHECK(!strcmp(WL.words[WL.n - 2].word, "set off"), "分隔符去掉：[%s]", WL.words[WL.n - 2].word);
	CHECK(!strcmp(WL.words[WL.n - 1].word, "look up") && !strcmp(WL.words[WL.n - 1].pos, "v."), "Tab 分列：[%s][%s]",
	      WL.words[WL.n - 1].word, WL.words[WL.n - 1].pos);
	words_add_text(&WL, "conduct n. v. 行为; 实施\n", &st);
	CHECK(st.added == 1 && !strcmp(WL.words[WL.n - 1].pos, "n. v."), "多个词性：[%s]", WL.words[WL.n - 1].pos);

	CASE("批量导入 · 两段式");
	words_add_text(&WL, "withstand\nyield\n承受\n屈服；让步\n", &st);
	CHECK(st.added == 2 && !st.err[0], "added=%d err=%s", st.added, st.err);

	CASE("批量导入 · 数量不一致 / 交错混杂 被整批拒绝");
	words_add_text(&WL, "rigid\nswirl\n僵硬的\n", &st);
	CHECK(st.added == 0 && strstr(st.err, "数量不一致"), "err=%s", st.err);
	words_add_text(&WL, "rigid\n僵硬的\nswirl\n", &st);
	CHECK(st.added == 0 && strstr(st.err, "格式混杂"), "err=%s", st.err);

	CASE("批量导入 · 英中交替");
	words_add_text(&WL, "give up\n放弃\nturn  down\nv. 拒绝、调低\n", &st);
	CHECK(st.added == 2 && !strcmp(WL.words[WL.n - 1].word, "turn down") && !strcmp(WL.words[WL.n - 1].pos, "v."),
	      "added=%d err=%s [%s]", st.added, st.err, WL.n ? WL.words[WL.n - 1].word : "");

	CASE("词性拆分");
	char pos[DICT_MAX_POS], mn[DICT_MAX_MEANING];
	split_pos_meaning("n. 过程 v. 处理", pos, sizeof pos, mn, sizeof mn);
	CHECK(strcmp(pos, "n. v.") == 0 && strcmp(mn, "过程 处理") == 0, "pos=[%s] mn=[%s]", pos, mn);
	split_pos_meaning("存货 & 库存", pos, sizeof pos, mn, sizeof mn);
	CHECK(pos[0] == 0 && strcmp(mn, "存货 库存") == 0, "pos=[%s] mn=[%s]", pos, mn);

	CASE("追加后文件可重新读回、原有内容保留");
	static WordList again;
	CHECK(words_load(&again, path) && again.n == 13, "重新读到 %d 个", again.n);
	CHECK(strcmp(again.words[0].word, "zeal") == 0 && strcmp(again.words[12].word, "turn down") == 0,
	      "首=%s 尾=%s", again.words[0].word, again.words[12].word);
	CHECK(strcmp(again.words[10].meaning, "屈服；让步") == 0, "释义=%s", again.words[10].meaning);

	CASE("改释义后整表写回");
	snprintf(WL.words[0].meaning, sizeof WL.words[0].meaning, "热情（热心）");
	CHECK(words_save(&WL), "写回失败");
	CHECK(words_load(&again, path) && again.n == 13 && !strcmp(again.words[0].meaning, "热情（热心）")
	      && !strcmp(again.words[1].meaning, "公正 [法]"), "n=%d [%s]", again.n, again.words[0].meaning);

	CASE("清空");
	words_clear(&WL);
	CHECK(WL.n == 0 && u8_fopen(path, "rb") == NULL, "清空后文件应被删除");
}

/* ================================================================== */
/* 普通模式                                                            */
/* ================================================================== */

static void test_rounds(void)
{
	sess_seed(&S, 42);

	CASE("普通模式 · 按词表顺序出题");
	sess_start_rounds(&S, 4);
	CHECK(sess_current(&S) == 0 && S.phase == SP_ASKING, "cur=%d", sess_current(&S));

	CASE("普通模式 · 答错可再试，再答对算对");
	sess_answer(&S, false);
	CHECK(S.phase == SP_WRONG, "phase=%d", S.phase);
	sess_answer(&S, true);
	CHECK(S.phase == SP_CORRECT && S.wlen == 0, "phase=%d wlen=%d", S.phase, S.wlen);
	CHECK(sess_next(&S) == -1 && sess_current(&S) == 1, "cur=%d", sess_current(&S));

	CASE("普通模式 · 答错后直接「下一个」= 记错词并公布上一题");
	sess_answer(&S, false);
	CHECK(sess_next(&S) == 1 && S.wlen == 1 && sess_current(&S) == 2, "wlen=%d cur=%d", S.wlen, sess_current(&S));

	CASE("普通模式 · 不知道 → 公布答案 → 下一个");
	sess_dont_know(&S);
	CHECK(S.phase == SP_REVEALED && S.wlen == 2, "phase=%d wlen=%d", S.phase, S.wlen);
	sess_next(&S);
	CHECK(sess_current(&S) == 3, "cur=%d", sess_current(&S));

	CASE("普通模式 · 一轮结束有错词 → 预览 → 下一轮只默错词");
	ok_next(&S);
	CHECK(S.phase == SP_PREVIEW && S.wlen == 2, "phase=%d", S.phase);
	sess_next_round(&S);
	CHECK(S.round == 2 && S.qlen == 2 && S.queue[0] == 1 && S.queue[1] == 2, "round=%d qlen=%d", S.round, S.qlen);

	CASE("普通模式 · 重新开始（第 2 轮内）= 重开本轮");
	ok_next(&S);
	sess_restart(&S);
	CHECK(S.round == 2 && S.qlen == 2 && sess_current(&S) == 1, "round=%d cur=%d", S.round, sess_current(&S));

	CASE("普通模式 · 全对 → 完成 → 再来一轮从头开始");
	ok_next(&S);
	ok_next(&S);
	CHECK(S.phase == SP_DONE, "phase=%d", S.phase);
	sess_restart(&S);
	CHECK(S.round == 1 && S.qlen == 4 && sess_current(&S) == 0, "round=%d", S.round);

	CASE("空词表");
	sess_start_rounds(&S, 0);
	CHECK(S.phase == SP_EMPTY && sess_current(&S) == -1, "phase=%d", S.phase);
	sess_dont_know(&S);
	sess_next(&S);
	CHECK(S.phase == SP_EMPTY, "空表下按钮不能改状态");
}

/* ================================================================== */
/* 无尽模式                                                            */
/* ================================================================== */

/* 校验重开后的队列：前 seen 个是 0..seen-1 组内见过词的一个排列，后面是没见过的词按原顺序 */
static bool reset_shape_ok(const Session *s, const int *seen_words, int seen_n)
{
	static char want[DICT_MAX_WORDS];
	memset(want, 0, sizeof want);
	for (int i = 0; i < seen_n; i++) want[seen_words[i]] = 1;
	for (int i = 0; i < seen_n; i++) if (!want[s->queue[i]]) return false;
	if (!is_perm_of_range(s->queue, s->qlen, s->gstart, s->gend)) return false;
	int prev = -1;
	for (int i = seen_n; i < s->qlen; i++) {
		if (want[s->queue[i]] || s->queue[i] < prev) return false;
		prev = s->queue[i];
	}
	return true;
}

static void test_endless(void)
{
	sess_seed(&S, 20261010);

	CASE("需求：独立按钮、随时进入 —— 普通模式中途进入，从第 1 组第 1 个词开始");
	sess_start_rounds(&S, 45);
	ok_next(&S); ok_next(&S); sess_dont_know(&S);
	sess_start_endless(&S, 45);
	CHECK(S.kind == SESS_ENDLESS && S.group == 0 && sess_current(&S) == 0 && S.phase == SP_ASKING,
	      "kind=%d group=%d cur=%d", S.kind, S.group, sess_current(&S));

	CASE("需求：每 20 个一组 —— 45 个词分成 20/20/5 三组，组内按原顺序");
	CHECK(sess_ngroups(&S) == 3 && S.qlen == 20, "ngroups=%d qlen=%d", sess_ngroups(&S), S.qlen);
	bool in_order = true;
	for (int i = 0; i < 20; i++) in_order &= (S.queue[i] == i);
	CHECK(in_order, "第一组应为 0..19 原顺序");

	CASE("需求：第一个词就错 → 显示答案 + 重新开始 → 还是从第一个词开始");
	sess_dont_know(&S);
	CHECK(S.phase == SP_REVEALED && sess_failed(&S), "错了以后要停在公布答案，等「重新开始」");
	CHECK(sess_current(&S) == 0, "答案对应的还是第一个词");
	sess_next(&S);
	CHECKQ(sess_current(&S) == 0 && S.queue[1] == 1, "重开后应先考词0、再考词1");
	CHECK(S.restarts == 1, "restarts=%d", S.restarts);

	CASE("需求：提交答错只允许重试，不触发重开");
	sess_answer(&S, false);
	CHECK(S.phase == SP_WRONG && !sess_failed(&S), "phase=%d", S.phase);
	int before_pos = S.pos;
	sess_next(&S);
	CHECK(S.pos == before_pos && S.restarts == 1, "提交答错后不能跳题或重开");
	sess_answer(&S, true);
	sess_next(&S);

	CASE("需求：错了以后点「重新开始」必定重置，不会正常进入下一个词");
	sess_start_endless(&S, 45);
	ok_next(&S); ok_next(&S); ok_next(&S);   /* 答对 0,1,2 */
	sess_dont_know(&S);                      /* 词 3 点「不知道」 */
	sess_next(&S);
	CHECK(S.pos == 0 && S.phase == SP_ASKING && S.restarts == 1, "pos=%d", S.pos);

	CASE("需求：重开范围 = 之前答对的 + 这次错的，打乱放前面；没见过的词保持原顺序放后面");
	{
		int seen_words[] = {0, 1, 2, 3};
		CHECKQ(reset_shape_ok(&S, seen_words, 4), "队列形状不对");
		CHECK(S.queue[4] == 4 && S.queue[5] == 5 && S.queue[19] == 19, "没见过的词原顺序跟在后面");
		CHECK(S.queue[0] != 3, "见过的词不止一个时，刚错的词不放第一个");
	}

	CASE("需求：重开后没答到的「见过的词」也算见过 —— 重开后第一个就错，4 个词全部重新打乱");
	sess_dont_know(&S);
	sess_next(&S);
	{
		int seen_words[] = {0, 1, 2, 3};
		CHECK(reset_shape_ok(&S, seen_words, 4) && S.seen == 4, "seen=%d", S.seen);
	}

	CASE("需求：打乱是真随机 —— 2000 次重开，错词出现在每个非首位置，且顺序各不相同");
	{
		int pos_hits[8] = {0}, first_hits[8] = {0};
		int distinct = 0;
		static int last[8];
		for (int t = 0; t < 2000; t++) {
			sess_start_endless(&S, 45);
			for (int k = 0; k < 5; k++) ok_next(&S);   /* 答对 0..4 */
			sess_dont_know(&S);                        /* 词 5 错 */
			sess_next(&S);
			for (int i = 0; i < 6; i++) if (S.queue[i] == 5) pos_hits[i]++;
			first_hits[S.queue[0]]++;
			if (memcmp(last, S.queue, sizeof(int) * 6) != 0) distinct++;
			memcpy(last, S.queue, sizeof(int) * 6);
		}
		CHECK(pos_hits[0] == 0, "错词出现在首位 %d 次", pos_hits[0]);
		bool spread = true;
		for (int i = 1; i < 6; i++) spread &= (pos_hits[i] > 300 && pos_hits[i] < 500);   /* 期望 400 */
		CHECK(spread, "错词位置分布 1..5 = %d %d %d %d %d", pos_hits[1], pos_hits[2], pos_hits[3], pos_hits[4], pos_hits[5]);
		CHECK(distinct > 1900, "相邻两次重开顺序相同太多：distinct=%d", distinct);
		bool first_spread = true;
		for (int i = 0; i < 5; i++) first_spread &= first_hits[i] > 0;
		CHECK(first_spread, "之前答对的词都可能排第一");
	}

	CASE("需求：本组一次不错地答完 → 进入下一组，上一组的词不再出现，下一组按原顺序");
	sess_start_endless(&S, 45);
	sess_dont_know(&S); sess_next(&S);              /* 先错一次，带着打乱的队列过关 */
	for (int i = 0; i < 20; i++) ok_next(&S);
	CHECK(S.group == 1 && S.gstart == 20 && S.gend == 40 && S.restarts == 0 && S.seen == 0,
	      "group=%d range=[%d,%d)", S.group, S.gstart, S.gend);
	in_order = true;
	for (int i = 0; i < 20; i++) in_order &= (S.queue[i] == 20 + i);
	CHECK(in_order, "第二组应为 20..39 原顺序");
	for (int k = 0; k < 7; k++) ok_next(&S);
	sess_dont_know(&S); sess_next(&S);
	CHECKQ(is_perm_of_range(S.queue, S.qlen, 20, 40), "第二组重开不能混进第一组的词");

	CASE("需求：全部小组都过了才结束（20/20/5）");
	sess_start_endless(&S, 45);
	for (int i = 0; i < 40; i++) ok_next(&S);
	CHECK(S.group == 2 && S.qlen == 5 && S.phase == SP_ASKING, "group=%d qlen=%d phase=%d", S.group, S.qlen, S.phase);
	for (int i = 0; i < 4; i++) ok_next(&S);
	CHECK(S.phase == SP_ASKING, "还差最后一个词，不能提前结束");
	ok_next(&S);
	CHECK(S.phase == SP_DONE, "phase=%d", S.phase);

	CASE("完成后「再来一遍」从第 1 组重来；无尽模式按钮再点一次退出到普通模式");
	sess_restart(&S);
	CHECK(S.kind == SESS_ENDLESS && S.group == 0 && sess_current(&S) == 0, "group=%d", S.group);
	sess_start_rounds(&S, 45);
	CHECK(S.kind == SESS_ROUNDS && S.qlen == 45, "kind=%d", S.kind);

	CASE("无尽模式中途加词：新词进后面的组，一个不漏");
	sess_start_endless(&S, 33);                     /* 20 + 13 */
	for (int i = 0; i < 20; i++) ok_next(&S);
	CHECK(S.gstart == 20 && S.gend == 33, "range=[%d,%d)", S.gstart, S.gend);
	sess_words_changed(&S, 50);                     /* 加了 17 个 */
	CHECK(sess_ngroups(&S) == 3, "[0,20)[20,33)[33,50) 应为 3 组：ngroups=%d", sess_ngroups(&S));
	for (int i = 0; i < 13; i++) ok_next(&S);
	CHECK(S.gstart == 33 && S.gend == 50, "加的词应从 33 接着分组：[%d,%d)", S.gstart, S.gend);

	CASE("无尽模式切换题型：已点不知道的不撤销，提交答错可重新作答");
	sess_start_endless(&S, 45);
	sess_answer(&S, true);
	sess_reask(&S);
	CHECK(S.phase == SP_ASKING, "答对后切题型应回到作答");
	sess_answer(&S, false);
	CHECK(S.phase == SP_WRONG, "提交答错不应公布答案");
	sess_reask(&S);
	CHECK(S.phase == SP_ASKING, "提交答错切题型应可重新作答");
	sess_dont_know(&S);
	sess_reask(&S);
	CHECK(S.phase == SP_REVEALED, "点不知道后切题型仍应保留重开状态");
}

/* 随机「学生」跑完整个无尽模式，每一步检查不变量 */
static void test_endless_fuzz(void)
{
	CASE("无尽模式压力测试：随机作答 300 局，每步检查不变量");
	int bad = 0;
	for (int game = 0; game < 300 && !bad; game++) {
		int n = 1 + game % 67;
		sess_seed(&S, 1000 + (uint64_t)game);
		sess_start_endless(&S, n);
		uint64_t r = 0x12345678u + (uint64_t)game;
		int steps = 0;
		while (S.phase != SP_DONE && steps++ < 200000) {
			if (!is_perm_of_range(S.queue, S.qlen, S.gstart, S.gend)) { bad = 1; break; }
			int cur = sess_current(&S);
			if (cur < S.gstart || cur >= S.gend) { bad = 2; break; }
			r = r * 6364136223846793005ull + 1442695040888963407ull;
			bool correct = (r >> 33) % 100 < 93;
			if (correct) {
				ok_next(&S);
			} else {
				int seen_before = S.seen > S.pos + 1 ? S.seen : S.pos + 1;
				if ((r >> 40) & 1) {
					sess_dont_know(&S);
					if (!sess_failed(&S)) { bad = 3; break; }
					sess_next(&S);
					if (S.pos != 0 || S.seen != seen_before) { bad = 4; break; }
				} else {
					sess_answer(&S, false);
					if (S.phase != SP_WRONG || sess_failed(&S)) { bad = 5; break; }
					sess_answer(&S, true);
					sess_next(&S);
				}
				int prev = -1;
				for (int i = S.seen; i < S.qlen; i++) {   /* 没见过的词保持原顺序 */
					if (S.queue[i] < prev) { bad = 5; break; }
					prev = S.queue[i];
				}
				if (bad) break;
			}
		}
		if (!bad && S.phase != SP_DONE) bad = 6;
		if (!bad && S.gend != n) bad = 7;
		if (bad) printf("  game=%d n=%d bad=%d ", game, n, bad), dump_queue(&S), printf("\n");
	}
	CHECK(bad == 0, "不变量被破坏（bad=%d）", bad);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_text();
	test_words();
	test_rounds();
	test_endless();
	test_endless_fuzz();
	printf("\n%d 项通过，%d 项失败\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
