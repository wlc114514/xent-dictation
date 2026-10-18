/**
 * @file dictation.c
 * @brief 「单词默写」前端 — 消息处理、批量加词窗口、入口。
 *
 * 前端：
 *   · dict_app.h  —— 模型（App）与消息
 *   · dict_view.c —— 视图（NavigationView + 卡片 + InfoBar 的 Fluent 版式）
 *   · dictation.c —— update()、后台判分、加词窗口、程序入口
 * 后端：
 *   · dict_session.c —— 出题状态机（普通轮次 + 无尽模式）
 *   · dict_words.c   —— 词表 dictation_extra.json 读写、批量导入
 *   · dict_text.c    —— 判分规则、首字母提示、JSON
 *   · dict_judge.c   —— 本地 JEV 小模型判分（在后台线程跑，不卡界面）
 */
#include <fluxent/fluxent.h>
#include <fluxent/flux_app.h>
#include <fluxent/flux_host.h>

#include "flux_internal.h"          /* FluxBackendCtx / flux_xtk_runtime_config */
#include "render/flux_dl_replay.h"  /* flux_xtk_backend_create */

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dict_app.h"
#include "dict_text.h"

static App    g_app;
static XtkRt *g_rt;        /* 运行时：给后台线程 / 加词窗口回投消息用 */
static HWND   g_bridge;    /* 跨线程投递消息的隐藏窗口 */

Drill *app_drill(App *a)
{
	if (a->page == PAGE_PRACTICE) return &a->drill[0];
	if (a->page == PAGE_ENDLESS)  return &a->drill[1];
	return NULL;
}

static void fb_set(Feedback *fb, XtkInfoBarSeverity sev, const char *title, const char *fmt, ...)
{
	fb->on  = true;
	fb->sev = sev;
	snprintf(fb->title, sizeof fb->title, "%s", title);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(fb->msg, sizeof fb->msg, fmt, ap);
	va_end(ap);
}

static void fb_clear(Feedback *fb) { fb->on = false; }

static int bits(uint32_t m)
{
	int n = 0;
	for (; m; m &= m - 1) n++;
	return n;
}

int drill_senses(const App *a, Drill *d)
{
	int idx = sess_current(&d->s);
	if (idx < 0) { d->nsense = 0; d->sense_of = 0; return 0; }
	if (d->sense_of != idx + 1) {
		const char *m = a->wl.words[idx].meaning;
		d->nsense = split_senses(m, d->senses, DICT_MAX_SENSES);
		if (d->nsense == 0) { snprintf(d->senses[0], sizeof d->senses[0], "%s", m); d->nsense = 1; }
		d->got      = 0;
		d->sense_of = idx + 1;
	}
	return bits(d->got);
}

/* 换题 / 重答同一个词前调用：已答出的义项作废 */
static void senses_reset(Drill *d) { d->sense_of = 0; }

/* 没答出的义项，用「、」连起来 */
static const char *senses_missing(const Drill *d, char *buf, size_t cap)
{
	size_t j = 0;
	buf[0]   = 0;
	for (int k = 0; k < d->nsense; k++) {
		if (d->got & (1u << k)) continue;
		j += (size_t)snprintf(buf + j, cap - j, "%s%s", j ? "、" : "", d->senses[k]);
		if (j >= cap) break;
	}
	return buf;
}

/* 公布答案用：「原词 xxx」/「释义 xxx」，后面附上完整词条 */
static const char *answer_text(const App *a, int idx, char *buf, size_t cap)
{
	const Word *w = &a->wl.words[idx];
	if (a->dir == DIR_EN2CN) snprintf(buf, cap, "%s  %s  %s", w->word, w->pos, w->meaning);
	else                     snprintf(buf, cap, "%s  %s  %s", w->meaning, w->pos, w->word);
	return buf;
}

/* ================================================================== */
/* 跨线程消息桥：后台线程 PostMessage → UI 线程 xtk_rt_post            */
/* ================================================================== */

#define WM_BRIDGE_JUDGED (WM_APP + 1)
#define WM_BRIDGE_POST   (WM_APP + 2)

static LRESULT CALLBACK bridge_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (!g_rt) return DefWindowProcW(h, msg, wp, lp);
	if (msg == WM_BRIDGE_JUDGED) {
		/* wp 低 32 位 = 请求号，高 32 位 = 判为意思接近的义项位掩码（只有一个义项时是 bit 0；义项不超过 16 个，float 装得下） */
		xtk_rt_post(g_rt, (XtkMsg){.tag = MSG_JUDGED, .i = (int32_t)(uint32_t)wp, .f = (float)(uint32_t)((uint64_t)wp >> 32),
		                           .d = (double)(LONG_PTR)lp / 1e6});
		return 0;
	}
	if (msg == WM_BRIDGE_POST) {
		xtk_rt_post(g_rt, (XtkMsg){.tag = (int32_t)wp});
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

static void bridge_create(void)
{
	WNDCLASSW wc     = {0};
	wc.lpfnWndProc   = bridge_proc;
	wc.hInstance     = GetModuleHandleW(NULL);
	wc.lpszClassName = L"DictBridge";
	RegisterClassW(&wc);
	g_bridge = CreateWindowExW(0, L"DictBridge", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
}

/* ================================================================== */
/* 后台判分                                                            */
/* ================================================================== */

typedef struct JudgeJob {
	Judge *judge;
	int    seq;
	char   meaning[DICT_MAX_MEANING];
	char   ans[DICT_MAX_ANSWER];

	/* 要判的义项（只有一个义项时就是整条释义）和还没答出的位掩码 */
	char     senses[DICT_MAX_SENSES][DICT_SENSE_LEN];
	int      nsense;
	uint32_t todo;
	double   threshold;
} JudgeJob;

#define JUDGE_MAX_PIECES 4   /* 一次最多判答案里的前几项：每项对每个义项问一次模型（2B 约 0.4 秒一次） */

/* 答案按 、，; 拆成几项，逐项对还没答出的义项判：「庞大的，微小的」整串交给模型不可靠（0.8B 会判对）。
 * 一项答案只顶一个义项；返回判为接近的义项位掩码，*p = 命中的最低概率（没命中则为最接近的那次） */
static DWORD WINAPI judge_thread(LPVOID param)
{
	JudgeJob *job = (JudgeJob *)param;
	char      parts[DICT_MAX_SENSES][DICT_SENSE_LEN];
	int       np = split_senses(job->ans, parts, DICT_MAX_SENSES);
	if (np == 0) { snprintf(parts[0], sizeof parts[0], "%s", job->ans); np = 1; }
	if (np > JUDGE_MAX_PIECES) np = JUDGE_MAX_PIECES;

	uint32_t hit = 0;
	double   best = -1, worst_hit = 2;
	for (int i = 0; i < np; i++) {
		for (int k = 0; k < job->nsense; k++) {
			if (!(job->todo & (1u << k)) || (hit & (1u << k))) continue;
			double q = judge_p(job->judge, job->senses[k], parts[i]);
			if (q < 0) goto done;   /* 模型不可用 */
			if (q > best) best = q;
			if (q >= job->threshold) {
				hit |= 1u << k;
				if (q < worst_hit) worst_hit = q;
				break;
			}
		}
	}
done:;
	double   p  = hit ? worst_hit : best;
	WPARAM   wp = (WPARAM)(((uint64_t)hit << 32) | (uint32_t)job->seq);
	PostMessageW(g_bridge, WM_BRIDGE_JUDGED, wp, (LPARAM)(LONG_PTR)llround(p * 1e6));
	free(job);
	return 0;
}

static bool start_judging(App *a, Drill *d, const Word *w)
{
	if (!judge_enabled(a->judge) || !g_bridge || is_blank(a->answer)) return false;
	JudgeJob *job = (JudgeJob *)calloc(1, sizeof *job);
	if (!job) return false;
	job->judge = a->judge;
	job->seq   = ++a->judge_seq;
	snprintf(job->meaning, sizeof job->meaning, "%s", w->meaning);
	snprintf(job->ans, sizeof job->ans, "%s", a->answer);
	job->threshold = judge_threshold(a->judge);
	if (d->nsense > 1) {
		memcpy(job->senses, d->senses, sizeof job->senses);
		job->nsense = d->nsense;
		job->todo   = ~d->got;
	} else {
		snprintf(job->senses[0], sizeof job->senses[0], "%s", w->meaning);
		job->nsense = 1;
		job->todo   = 1;
	}
	HANDLE h = CreateThread(NULL, 0, judge_thread, job, 0, NULL);
	if (!h) { free(job); return false; }
	CloseHandle(h);
	a->judging = true;
	fb_set(&d->fb, XTK_INFOBAR_INFORMATIONAL, "正在判断…", "字面没判出来，正在请本地模型看意思是否接近。");
	return true;
}

/* 丢弃正在进行的判分 */
static void cancel_judging(App *a)
{
	if (a->judging) a->judge_seq++;
	a->judging = false;
}

/* ================================================================== */
/* 练习动作                                                            */
/* ================================================================== */

/* 判定结果落地：p < 0 表示没用上模型 */
static void apply_verdict(App *a, Drill *d, bool ok, double p)
{
	int  idx     = sess_current(&d->s);
	bool slipped = (d->s.phase == SP_WRONG);   /* 之前在这个词上错过 */
	char ans[800];
	sess_answer(&d->s, ok);

	if (ok) {
		d->answered++;
		if (!slipped) d->correct++;
		const Word *w = &a->wl.words[idx];
		if (p >= 0) fb_set(&d->fb, XTK_INFOBAR_SUCCESS, "答对了", "本地模型判为意思接近（%.2f）· %s  %s", p, w->word, w->meaning);
		else        fb_set(&d->fb, XTK_INFOBAR_SUCCESS, "答对了", "%s  %s  %s", w->word, w->pos, w->meaning);
		a->answer[0] = 0;
	} else if (d->s.phase == SP_REVEALED) {          /* 点「不知道」后公布答案并等待重开 */
		d->answered++;
		answer_text(a, idx, ans, sizeof ans);
		if (p >= 0) fb_set(&d->fb, XTK_INFOBAR_ERROR, "已公布答案", "接近度 %.2f · 正确答案：%s", p, ans);
		else        fb_set(&d->fb, XTK_INFOBAR_ERROR, "已公布答案", "正确答案：%s", ans);
		a->answer[0] = 0;
	} else {                                          /* 普通模式：可再试 */
		if (p >= 0) fb_set(&d->fb, XTK_INFOBAR_ERROR, "答错了", "接近度 %.2f。再试一次，或点「不知道」看答案。", p);
		else        fb_set(&d->fb, XTK_INFOBAR_ERROR, "答错了", "再试一次，或点「不知道」看答案。");
	}
}

/* 一词多义答出了 fresh 个新义项：全齐了（或开关关着、答出一个就够）算答对，否则清空输入框接着写 */
static void senses_progress(App *a, Drill *d, int fresh, double p)
{
	int left = d->nsense - bits(d->got);
	if (left == 0 || !a->all_senses) { apply_verdict(a, d, true, p); return; }
	if (p >= 0) fb_set(&d->fb, XTK_INFOBAR_SUCCESS, "答对一个", "本地模型判为意思接近（%.2f）。还差 %d 个意思，接着写。", p, left);
	else        fb_set(&d->fb, XTK_INFOBAR_SUCCESS, fresh > 1 ? "答对几个" : "答对一个", "还差 %d 个意思，接着写。", left);
	a->answer[0] = 0;
}

static void do_submit(App *a)
{
	Drill *d = app_drill(a);
	if (!d || a->judging || (d->s.phase != SP_ASKING && d->s.phase != SP_WRONG)) return;
	const Word *w = &a->wl.words[sess_current(&d->s)];

	if (a->dir == DIR_CN2EN) {
		apply_verdict(a, d, answer_eq(a->answer, w->word), -1);
		return;
	}
	drill_senses(a, d);
	if (d->nsense > 1) {
		/* 一词多义：每次答出一个（或几个），全部答出才算这个词对（开关关掉时答出一个就算） */
		int fresh = senses_mark(a->answer, d->senses, d->nsense, &d->got);
		if (fresh > 0) { senses_progress(a, d, fresh, -1); return; }
	} else if (meaning_match(a->answer, w->meaning)) {
		apply_verdict(a, d, true, -1);
		return;
	}
	if (!start_judging(a, d, w)) apply_verdict(a, d, false, -1);
}

static void on_judged(App *a, int seq, double p, uint32_t hit)
{
	Drill *d = app_drill(a);
	if (!d || !a->judging || seq != a->judge_seq) return;   /* 过期结果（期间切了页/题型） */
	a->judging = false;
	if (d->nsense > 1) {
		hit &= ~d->got & ((1u << d->nsense) - 1);
		if (hit) {
			d->got |= hit;
			senses_progress(a, d, bits(hit), p);
			return;
		}
	}
	if (p < 0) apply_verdict(a, d, false, -1);              /* 模型不可用：按字面判错 */
	else       apply_verdict(a, d, hit != 0, p);
}

static void do_dont(App *a)
{
	Drill *d = app_drill(a);
	if (!d || a->judging || (d->s.phase != SP_ASKING && d->s.phase != SP_WRONG)) return;
	int  idx = sess_current(&d->s);
	char t[800], miss[600] = "";
	sess_dont_know(&d->s);
	d->answered++;
	answer_text(a, idx, t, sizeof t);
	if (a->dir == DIR_EN2CN && d->nsense > 1 && d->got) {   /* 答出过一部分：点明还漏了哪些 */
		char m[500];
		snprintf(miss, sizeof miss, "\n没答出：%s", senses_missing(d, m, sizeof m));
	}
	if (d->s.kind == SESS_ENDLESS) fb_set(&d->fb, XTK_INFOBAR_ERROR, "本组要重新开始", "正确答案：%s%s", t, miss);
	else                           fb_set(&d->fb, XTK_INFOBAR_WARNING, "答案", "%s%s", t, miss);
	a->answer[0] = 0;
}

static void do_next(App *a)
{
	Drill *d = app_drill(a);
	if (!d || a->judging) return;
	SessPhase before = d->s.phase;
	if (before != SP_CORRECT && before != SP_WRONG && before != SP_REVEALED) return;
	int group    = d->s.group;
	int restarts = d->s.restarts;
	int skipped  = sess_next(&d->s);
	senses_reset(d);

	a->answer[0] = 0;
	fb_clear(&d->fb);
	char t[800];
	if (skipped >= 0) {
		/* 普通模式答错后跳过：顺带公布上一题答案 */
		d->answered++;
		fb_set(&d->fb, XTK_INFOBAR_WARNING, "上一题的答案", "%s", answer_text(a, skipped, t, sizeof t));
	} else if (d->s.kind == SESS_ENDLESS && d->s.phase != SP_DONE) {
		if (d->s.group != group)
			fb_set(&d->fb, XTK_INFOBAR_SUCCESS, "本组过关", "第 %d 组全部答对，进入第 %d 组。", group + 1, d->s.group + 1);
		else if (d->s.restarts != restarts)
			fb_set(&d->fb, XTK_INFOBAR_INFORMATIONAL, "本组重新开始",
			       "见过的 %d 个词已打乱顺序排在前面，没见过的词保持原顺序。", d->s.seen);
	}
}

/* 设置只有一项，存成 {"all_senses": true} */
static void settings_load(App *a)
{
	a->all_senses = true;   /* 默认要全答 */
	char *buf = read_file(a->settings_path, NULL);
	if (!buf) return;
	const char *v = json_find_key(buf, "all_senses");
	if (v && strncmp(v, "false", 5) == 0) a->all_senses = false;
	free(buf);
}

static void settings_save(const App *a)
{
	FILE *f = u8_fopen(a->settings_path, "wb");
	if (!f) return;
	fprintf(f, "{\n  \"all_senses\": %s\n}\n", a->all_senses ? "true" : "false");
	fclose(f);
}

/* 「多义词要全答」开关：关掉时，正答到一半的词已答出过意思就直接算对 */
static void do_all_senses(App *a, bool on)
{
	if (a->all_senses == on) return;
	a->all_senses = on;
	settings_save(a);
	Drill *d = app_drill(a);
	if (!on && d && !a->judging && a->dir == DIR_EN2CN && d->got &&
	    (d->s.phase == SP_ASKING || d->s.phase == SP_WRONG))
		apply_verdict(a, d, true, -1);
}

static void reset_drill(Drill *d)
{
	d->answered = d->correct = 0;
	fb_clear(&d->fb);
	senses_reset(d);
}

static void do_restart(App *a)
{
	Drill *d = app_drill(a);
	if (!d) return;
	cancel_judging(a);
	sess_restart(&d->s);
	reset_drill(d);
	a->answer[0] = 0;
}

static void do_start_round(App *a)
{
	Drill *d = app_drill(a);
	if (!d) return;
	sess_next_round(&d->s);
	senses_reset(d);
	fb_clear(&d->fb);
	a->answer[0] = 0;
}

static void do_nav(App *a, int page)
{
	if (page < 0 || page >= PAGE_COUNT || page == PAGE_SEP || page == (int)a->page) return;
	cancel_judging(a);
	Drill *d = app_drill(a);
	if (d && (d->s.phase == SP_ASKING || d->s.phase == SP_WRONG) && d->fb.sev == XTK_INFOBAR_INFORMATIONAL)
		fb_clear(&d->fb);   /* 「正在判断…」随判分一起作废 */
	a->page   = (Page)page;
	a->answer[0] = 0;
}

static void do_dir(App *a, int dir)
{
	if (dir != DIR_CN2EN && dir != DIR_EN2CN) return;
	if ((Dir)dir == a->dir) return;
	cancel_judging(a);
	a->dir = (Dir)dir;
	if (a->dir == DIR_EN2CN) judge_warmup(a->judge);   /* 预热本地 JEV */
	for (int i = 0; i < 2; i++) {
		Drill *d = &a->drill[i];
		sess_reask(&d->s);
		senses_reset(d);
		if (!sess_failed(&d->s) && d->s.phase != SP_DONE) fb_clear(&d->fb);
	}
	a->answer[0] = 0;
}

/* ================================================================== */
/* 词库动作                                                            */
/* ================================================================== */

static void words_changed(App *a)
{
	a->wl_rev++;
	for (int i = 0; i < 2; i++) sess_words_changed(&a->drill[i].s, a->wl.n);
}

static void add_text(App *a, const char *text)
{
	AddStat st;
	words_add_text(&a->wl, text, &st);
	Feedback *fb = &a->add_fb;
	if (st.err[0]) {
		fb_set(fb, XTK_INFOBAR_ERROR, "没有录入", "%s", st.err);
	} else if (st.added <= 0) {
		fb_set(fb, XTK_INFOBAR_WARNING, "没有可录入的词", "%d 个已在词库里，%d 行格式不对。", st.dup, st.bad);
	} else if (st.dup + st.bad > 0) {
		fb_set(fb, XTK_INFOBAR_SUCCESS, "已录入", "新增 %d 个词，跳过 %d 个重复、%d 行格式不对。现在共 %d 个词。",
		       st.added, st.dup, st.bad, a->wl.n);
	} else {
		fb_set(fb, XTK_INFOBAR_SUCCESS, "已录入", "新增 %d 个词，现在共 %d 个词。新词排在词表末尾。", st.added, a->wl.n);
	}
	if (st.added > 0) words_changed(a);
}

/* 剪贴板文本 → UTF-8（调用方 free） */
static char *clipboard_text(void)
{
	if (!OpenClipboard(NULL)) return NULL;
	char  *out = NULL;
	HANDLE h   = GetClipboardData(CF_UNICODETEXT);
	const wchar_t *wt = h ? (const wchar_t *)GlobalLock(h) : NULL;
	if (wt) {
		int need = WideCharToMultiByte(CP_UTF8, 0, wt, -1, NULL, 0, NULL, NULL);
		if (need > 0 && need < 4 * 1024 * 1024 && (out = (char *)malloc((size_t)need)))
			WideCharToMultiByte(CP_UTF8, 0, wt, -1, out, need, NULL, NULL);
		GlobalUnlock(h);
	}
	CloseClipboard();
	return out;
}

static void do_add_clip(App *a)
{
	char *t = clipboard_text();
	if (!t || !*t) fb_set(&a->add_fb, XTK_INFOBAR_WARNING, "剪贴板是空的", "先在别处复制好词表，再点「从剪贴板导入」。");
	else           add_text(a, t);
	free(t);
}

static void do_clear_result(App *a, int result)
{
	a->clear_open = false;
	if (result != XTK_DIALOG_PRIMARY) return;
	cancel_judging(a);
	int n = a->wl.n;
	words_clear(&a->wl);
	words_changed(a);
	for (int i = 0; i < 2; i++) reset_drill(&a->drill[i]);
	fb_set(&a->add_fb, XTK_INFOBAR_INFORMATIONAL, "词库已清空", "删除了 %d 个词。重新录入后就能开始默写。", n);
}

static void show_add_dialog(App *a);

static void update(void *model, XtkMsg msg)
{
	App   *a = (App *)model;
	Drill *d = app_drill(a);
	switch (msg.tag) {
	case MSG_NAV:         do_nav(a, msg.i);           break;
	case MSG_DIR:         do_dir(a, msg.i);           break;
	case MSG_ANSWER:
		if (msg.ptr) snprintf(a->answer, sizeof a->answer, "%s", (const char *)msg.ptr);
		break;
	case MSG_ENTER:
		/* 回车 = 主按钮：答题中提交，判定后下一个 / 重新开始本组 */
		if (!d) break;
		if (d->s.phase == SP_ASKING || d->s.phase == SP_WRONG) do_submit(a);
		else if (d->s.phase == SP_CORRECT || d->s.phase == SP_REVEALED) do_next(a);
		break;
	case MSG_SUBMIT:        do_submit(a);                    break;
	case MSG_DONT:          do_dont(a);                      break;
	case MSG_NEXT:          do_next(a);                      break;
	case MSG_RESTART:       do_restart(a);                   break;
	case MSG_START_ROUND:   do_start_round(a);               break;
	case MSG_JUDGED:        on_judged(a, msg.i, msg.d, (uint32_t)msg.f); break;
	case MSG_GO_LIBRARY:    do_nav(a, PAGE_LIBRARY);         break;
	case MSG_ADD_DIALOG:    show_add_dialog(a);              break;
	case MSG_ADD_DONE:      break;   /* 结果已写进 add_fb，这里只为触发重绘 */
	case MSG_ADD_CLIP:      do_add_clip(a);                  break;
	case MSG_ADD_MSG_CLOSE: fb_clear(&a->add_fb);            break;
	case MSG_CLEAR_ASK:     a->clear_open = true;            break;
	case MSG_CLEAR_RESULT:  do_clear_result(a, msg.i);       break;
	case MSG_ALL_SENSES:    do_all_senses(a, msg.b);         break;
	default: break;
	}
}

/* ================================================================== */
/* 批量加词窗口（Win32 多行输入框：框架的文本框不支持多行）            */
/* ================================================================== */

typedef struct AddDialog {
	App     *app;
	HWND     hwnd, label, edit, btn_ok, btn_cancel;
	HBRUSH   bg;
	HFONT    font;
	COLORREF bg_col, fg_col;
} AddDialog;

static AddDialog *g_add_dlg;

/* 按窗口大小和 DPI 摆放控件：编辑框吃掉剩余空间 */
static void add_dialog_layout(AddDialog *d)
{
	RECT rc;
	GetClientRect(d->hwnd, &rc);
	int s = (int)GetDpiForWindow(d->hwnd);
#define DP(v) MulDiv((v), s, 96)
	int W = rc.right, H = rc.bottom, pad = DP(16), bw = DP(96), bh = DP(34), lh = DP(48);
	MoveWindow(d->label, pad, pad, W - 2 * pad, lh, TRUE);
	MoveWindow(d->edit, pad, pad + lh + DP(8), W - 2 * pad, H - (pad + lh + DP(8)) - bh - 2 * pad, TRUE);
	MoveWindow(d->btn_cancel, W - pad - bw, H - pad - bh, bw, bh, TRUE);
	MoveWindow(d->btn_ok, W - pad - 2 * bw - DP(10), H - pad - bh, bw, bh, TRUE);
#undef DP
}

static HWND add_child(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id, HFONT font)
{
	HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
	                         parent, (HMENU)(INT_PTR)id, NULL, NULL);
	SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
	return h;
}

/* 编辑框文本 → UTF-8 → 写进词表 */
static void add_dialog_submit(AddDialog *d)
{
	int len = GetWindowTextLengthW(d->edit);
	if (len <= 0) {
		MessageBoxW(d->hwnd, L"输入框为空", L"提示", MB_OK | MB_ICONWARNING);
		return;
	}
	wchar_t *wbuf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
	if (!wbuf) return;
	GetWindowTextW(d->edit, wbuf, len + 1);
	int   u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
	char *u8    = (char *)malloc((size_t)u8len);
	if (u8) {
		WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8, u8len, NULL, NULL);
		add_text(d->app, u8);
		free(u8);
	}
	free(wbuf);
	PostMessageW(g_bridge, WM_BRIDGE_POST, MSG_ADD_DONE, 0);   /* 让主界面刷新提示 */
	DestroyWindow(d->hwnd);
}

static LRESULT CALLBACK add_dialog_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	AddDialog *d = (AddDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_CREATE: {
		d = (AddDialog *)((CREATESTRUCTW *)lp)->lpCreateParams;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
		d->hwnd   = hwnd;
		d->bg_col = d->app->dark ? RGB(32, 32, 32) : RGB(249, 249, 249);   /* 跟随系统深浅色 */
		d->fg_col = d->app->dark ? RGB(240, 240, 240) : RGB(26, 26, 26);
		d->bg     = CreateSolidBrush(d->bg_col);

		NONCLIENTMETRICSW ncm = {sizeof ncm};
		SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
		ncm.lfMessageFont.lfHeight = -MulDiv(16, (int)GetDpiForWindow(hwnd), 96);
		lstrcpyW(ncm.lfMessageFont.lfFaceName, L"Microsoft YaHei UI");
		d->font = CreateFontIndirectW(&ncm.lfMessageFont);

		d->label = add_child(hwnd, L"STATIC",
		    L"每行「单词或短语 词性 释义」，如 account for v. 解释、导致\r\n或者英文、中文一行一行交替；或者上半部分全英文、下半部分全中文",
		    SS_LEFT, 0, 0, d->font);
		d->edit = add_child(hwnd, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
		                    WS_EX_CLIENTEDGE, 0, d->font);
		SendMessageW(d->edit, EM_LIMITTEXT, 0, 0);   /* 不限长度 */
		d->btn_ok     = add_child(hwnd, L"BUTTON", L"提交", BS_DEFPUSHBUTTON, 0, IDOK, d->font);
		d->btn_cancel = add_child(hwnd, L"BUTTON", L"取消", BS_PUSHBUTTON, 0, IDCANCEL, d->font);

		if (d->app->dark) {
			BOOL dark = TRUE;
			DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
			SetWindowTheme(d->edit, L"DarkMode_Explorer", NULL);
			SetWindowTheme(d->btn_ok, L"DarkMode_Explorer", NULL);
			SetWindowTheme(d->btn_cancel, L"DarkMode_Explorer", NULL);
		}

		add_dialog_layout(d);
		SetFocus(d->edit);
		return 0;
	}
	case WM_ERASEBKGND: {
		if (!d) break;
		RECT rc;
		GetClientRect(hwnd, &rc);
		FillRect((HDC)wp, &rc, d->bg);
		return 1;
	}
	case WM_SIZE:
		if (d && d->edit) add_dialog_layout(d);
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lp)->ptMinTrackSize = (POINT){420, 320};
		return 0;
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSTATIC:
		SetTextColor((HDC)wp, d->fg_col);
		SetBkColor((HDC)wp, d->bg_col);
		return (LRESULT)d->bg;
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK)     { add_dialog_submit(d); return 0; }
		if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd);  return 0; }
		break;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (d) {
			if (d->font) DeleteObject(d->font);
			if (d->bg) DeleteObject(d->bg);
			free(d);
		}
		g_add_dlg = NULL;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_add_dialog(App *a)
{
	if (g_add_dlg) {
		SetForegroundWindow(g_add_dlg->hwnd);
		return;
	}
	static bool registered;
	if (!registered) {
		WNDCLASSEXW wc   = {sizeof wc};
		wc.lpfnWndProc   = add_dialog_proc;
		wc.hInstance     = GetModuleHandleW(NULL);
		wc.lpszClassName = L"DictAddDialog";
		wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = NULL;   /* 背景在 WM_ERASEBKGND 里按主题画 */
		RegisterClassExW(&wc);
		registered = true;
	}

	AddDialog *d = (AddDialog *)calloc(1, sizeof *d);
	if (!d) return;
	d->app    = a;
	g_add_dlg = d;

	HWND parent = FindWindowW(L"FluxentWindowClass", NULL);
	int  dpi    = parent ? (int)GetDpiForWindow(parent) : 96;
	int  w = MulDiv(640, dpi, 96), h = MulDiv(560, dpi, 96), x = CW_USEDEFAULT, y = CW_USEDEFAULT;
	if (parent) {
		RECT pr;
		GetWindowRect(parent, &pr);
		x = pr.left + ((pr.right - pr.left) - w) / 2;
		y = pr.top + ((pr.bottom - pr.top) - h) / 2;
	}
	HWND hw = CreateWindowExW(0, L"DictAddDialog", L"添加单词", WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX,
	                          x, y, w, h, parent, NULL, GetModuleHandleW(NULL), d);
	if (!hw) {
		free(d);
		g_add_dlg = NULL;
		return;
	}
	ShowWindow(hw, SW_SHOW);
	SetForegroundWindow(hw);
}

/* ================================================================== */
/* 入口                                                                */
/* ================================================================== */

/* 解析相对路径：先看 CWD，再看 exe 往上 5 级的「背词工具」根目录 */
static void resolve_path(const char *in, char *out, int cap)
{
	if (in[0] == '/' || in[0] == '\\' || (in[0] && in[1] == ':')) {
		snprintf(out, (size_t)cap, "%s", in);
		return;
	}
	wchar_t exe[MAX_PATH], wrel[MAX_PATH], full[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, in, -1, wrel, MAX_PATH);
	if (GetFileAttributesW(wrel) != INVALID_FILE_ATTRIBUTES) {
		snprintf(out, (size_t)cap, "%s", in);
		return;
	}
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	for (int i = 0; i < 6; i++) {   /* 去掉文件名 + 往上 5 级 */
		wchar_t *slash = wcsrchr(exe, L'\\');
		if (!slash) break;
		*slash = 0;
	}
	_snwprintf(full, MAX_PATH, L"%ls\\%ls", exe, wrel);
	if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES)
		WideCharToMultiByte(CP_UTF8, 0, full, -1, out, cap, NULL, NULL);
	else
		snprintf(out, (size_t)cap, "%s", in);   /* 都不在：原样返回 */
}

static uint64_t make_seed(void)
{
	LARGE_INTEGER qpc;
	QueryPerformanceCounter(&qpc);
	return (uint64_t)time(NULL) ^ ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)qpc.QuadPart;
}

/* 模型初始化：读词表 + 判分配置；两套练习都从头准备好 */
static void app_init(App *a, const char *words_path, const char *judge_path)
{
	memset(a, 0, sizeof *a);
	char path[MAX_PATH * 3];
	resolve_path(words_path, path, (int)sizeof path);
	words_load(&a->wl, path);   /* 读不出来就是空表，界面会提示 */
	a->judge = judge_create(judge_path, resolve_path);
	resolve_path("dictation_settings.json", a->settings_path, (int)sizeof a->settings_path);
	settings_load(a);
	a->dark  = flux_theme_systemdark();
	uint64_t seed = make_seed();
	sess_seed(&a->drill[0].s, seed);
	sess_seed(&a->drill[1].s, seed ^ 0xA5A5A5A5DEADBEEFull);
	sess_start_rounds(&a->drill[0].s, a->wl.n);
	sess_start_endless(&a->drill[1].s, a->wl.n);   /* 无尽模式随时可进，进度独立保存 */
}

#ifndef DICT_LAYOUT_TEST

/* 框架的 .integrate_window = true 在本机仍会留一条系统标题栏（顶部两组最小化/
 * 最大化/关闭）。等窗口真正显示出来再摘掉 WS_CAPTION——太早改样式会打断显示流程。 */
static DWORD WINAPI caption_fix_thread(LPVOID unused)
{
	(void)unused;
	for (int i = 0; i < 600; i++) {
		HWND h = FindWindowW(L"FluxentWindowClass", NULL);
		if (h && IsWindowVisible(h)) {
			LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
			if (st & WS_CAPTION) {
				SetWindowLongPtrW(h, GWL_STYLE, st & ~(LONG_PTR)WS_CAPTION);
				SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
			return 0;
		}
		Sleep(15);
	}
	return 1;
}

static void frame_cb(void *ctx) { xtk_rt_frame((XtkRt *)ctx); }

/* DICT_DIAG=1 时把失败的帧记进 dictation_diag.log（排查界面不显示用） */
static void on_diagnostic(void *ctx, XtkFrameResult const *r)
{
	(void)ctx;
	if (r->status == XTK_FRAME_CLEAN || r->status == XTK_FRAME_COMMITTED || r->status == XTK_FRAME_DEFERRED) return;
	FILE *f = fopen("dictation_diag.log", "a");
	if (!f) return;
	fprintf(f, "frame status=%d batch.error=%d op=%u identity.error=%d\n", (int)r->status,
	        (int)r->diagnostic.error, r->diagnostic.failed_op, (int)r->identity.error);
	fclose(f);
}

/* 与框架的 flux_run() 相同的装配，只是把运行时留在 g_rt，好让后台线程回投消息 */
static int app_run(const FluxAppCfg *cfg, App *a)
{
	FluxApp *app = flux_app_create(cfg);
	if (!app) return 1;
	FluxBackendCtx bctx = {.ctx = flux_app_ctx(app), .store = flux_app_store(app), .app = app};
	int rc = 1;

	bctx.host = flux_host_adopt(bctx.ctx, bctx.store, flux_app_root(app));
	XtkBackend *backend = bctx.host ? flux_xtk_backend_create(&bctx, bctx.host) : NULL;
	if (backend) {
		flux_app_sethost(app, bctx.host, backend);
		XtkRtCfg rt_cfg = flux_xtk_runtime_config(&bctx);
		const char *diag = getenv("DICT_DIAG");
		if (diag && *diag == '1') rt_cfg.on_diagnostic = on_diagnostic;
		bctx.runtime    = xtk_rt_create(bctx.host, &rt_cfg, flux_app_root(app), a, update, app_view);
		if (bctx.runtime) {
			g_rt = bctx.runtime;
			flux_app_onframe(app, frame_cb, bctx.runtime);
			rc = flux_app_run(app);
			flux_app_onframe(app, NULL, NULL);
			g_rt = NULL;
		}
	}
	if (bctx.runtime && !xtk_rt_destroy(bctx.runtime)) rc = 1;
	if (bctx.host) flux_host_destroy(bctx.host);
	flux_app_destroy(app);
	return rc;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int showCmd)
{
	(void)hInst; (void)hPrev; (void)cmdLine; (void)showCmd;

	/* 数据文件：命令行参数优先，否则用工作目录下的默认名 */
	char words_path[MAX_PATH * 3] = "dictation_extra.json";
	char judge_path[MAX_PATH * 3] = "dictation_judge.json";
	int     argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv) {
		if (argc >= 2) WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, words_path, sizeof words_path, NULL, NULL);
		if (argc >= 3) WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, judge_path, sizeof judge_path, NULL, NULL);
		LocalFree(argv);
	}

	App *a = &g_app;
	app_init(a, words_path, judge_path);
	bridge_create();

	/* 截图/回归用：DICT_START_PAGE=1/3/4 直接打开无尽模式/词库/说明 */
	const char *env = getenv("DICT_START_PAGE");
	if (env && *env) {
		int p = atoi(env);
		if (p > 0 && p < PAGE_COUNT && p != PAGE_SEP) a->page = (Page)p;
	}

	HANDLE th = CreateThread(NULL, 0, caption_fix_thread, NULL, 0, NULL);
	if (th) CloseHandle(th);

	FluxAppCfg cfg = {
	    .title     = L"单词默写",
	    .width     = 1080,
	    .height    = 780,
	    .dark_mode = a->dark,
	    .backdrop  = FLUX_BACKDROP_MICA,
	};
	int rc = app_run(&cfg, a);

	judge_destroy(a->judge);
	return rc;
}

#else /* DICT_LAYOUT_TEST */

/* ================================================================== */
/* 无头布局自检：xmake build test_dict_layout && xmake run test_dict_layout
 *   STAGE=5  默写页                  STAGE=8  词库页
 *   STAGE=12 错词预览页              STAGE=13 无尽模式答错后（「重新开始本组」）
 *   STAGE=14 说明页                  STAGE=15 全部完成
 * 不开窗口，打印视图里每个节点的实际矩形。                              */
/* ================================================================== */

static void dump_node(XentCtx *ctx, XtkNode const *n, const char *path, int depth)
{
	XentRect r  = {0};
	bool     ok = xent_layout_rect(ctx, xtk_node_id(n), &r);
	printf("%-14s d=%d  %s x=%7.1f y=%7.1f w=%7.1f h=%7.1f\n", path, depth, ok ? "rect" : "----", r.x, r.y, r.w, r.h);
	for (int i = 0; i < xtk_node_nchild(n); i++) {
		char sub[128];
		snprintf(sub, sizeof sub, "%s.%d", path, i);
		dump_node(ctx, xtk_node_child(n, i), sub, depth + 1);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *env   = getenv("STAGE");
	int         stage = env ? atoi(env) : 5;
	printf("STAGE=%d\n", stage);

	/* 造 3 个假词，不依赖词表文件 */
	App *a = &g_app;
	memset(a, 0, sizeof *a);
	static const char *fake[3][3] = {{"swear", "v.", "发誓"}, {"swirl", "v.", "盘旋"}, {"symmetric", "adj.", "对称的"}};
	for (int i = 0; i < 3; i++) {
		Word *w = &a->wl.words[a->wl.n++];
		snprintf(w->word, sizeof w->word, "%s", fake[i][0]);
		snprintf(w->pos, sizeof w->pos, "%s", fake[i][1]);
		snprintf(w->meaning, sizeof w->meaning, "%s", fake[i][2]);
	}
	Session *s0 = &a->drill[0].s, *s1 = &a->drill[1].s;
	sess_seed(s0, 1);
	sess_seed(s1, 2);
	sess_start_rounds(s0, a->wl.n);
	sess_start_endless(s1, a->wl.n);

	if (stage == 8) a->page = PAGE_LIBRARY;
	if (stage == 14) a->page = PAGE_HELP;
	if (stage == 12)
		for (int i = 0; i < 3; i++) { sess_dont_know(s0); sess_next(s0); }
	if (stage == 13) {
		a->page = PAGE_ENDLESS;
		do_dont(a);
	}
	if (stage == 15)
		for (int i = 0; i < 3; i++) { sess_answer(s0, true); sess_next(s0); }

	XentCtx  *ctx   = xent_ctx_create(&(XentCfg){0});
	FluxText *txt   = flux_text_create();
	flux_text_register(txt, ctx);   /* 不注册文本测量，文字高度会退回默认字号 */
	FluxStore *store = flux_store_create(64);
	flux_store_bind(store, ctx);
	XentNodeId host = xent_node_create(ctx);
	xent_setproto(ctx, host, XENT_PROTOCOL_FLEX);
	xent_setflexdir(ctx, host, XENT_FLEX_COLUMN);
	xent_setitems(ctx, host, XENT_FLEX_ALIGN_STRETCH);

	FluxBackendCtx bctx = {.ctx = ctx, .store = store};
	bctx.host           = flux_host_adopt(ctx, store, host);
	XtkRtCfg tcfg       = flux_xtk_runtime_config(&bctx);
	XtkRt   *rt         = xtk_rt_create(bctx.host, &tcfg, host, a, update, app_page);   /* 导航外壳要真实后端，这里只量页面 */
	bctx.runtime        = rt;
	XtkFrameResult fr = xtk_rt_frame(rt);
	printf("frame status=%d identity.error=%d batch.error=%d op=%u\n", (int)fr.status, (int)fr.identity.error,
	       (int)fr.diagnostic.error, fr.diagnostic.failed_op);
	xent_layout(ctx, host, 760.0f, 732.0f);   /* 窗口减去展开的导航栏 / 顶栏 */
	dump_node(ctx, xtk_rt_root(rt), "root", 0);

	xtk_rt_destroy(rt);
	flux_host_destroy(bctx.host);
	flux_store_destroy(store);
	flux_text_destroy(txt);
	xent_ctx_destroy(ctx);
	return 0;
}

#endif /* DICT_LAYOUT_TEST */
