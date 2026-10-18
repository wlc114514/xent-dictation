/**
 * @file dict_session.c
 * @brief 后端 · 出题状态机实现（规则见 dict_session.h）。
 */
#include "dict_session.h"

#include <string.h>

/* ================================================================== */
/* 随机数：xorshift64*，无偏取范围                                     */
/* ================================================================== */

void sess_seed(Session *s, uint64_t seed)
{
	s->rng = seed ? seed : 0x9E3779B97F4A7C15ull;
}

static uint64_t rng_next(Session *s)
{
	uint64_t x = s->rng;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	s->rng = x;
	return x * 0x2545F4914F6CDD1Dull;
}

/* [0, n) 均匀 */
static int rng_below(Session *s, int n)
{
	uint64_t un    = (uint64_t)n;
	uint64_t limit = UINT64_MAX - UINT64_MAX % un;
	uint64_t r;
	do r = rng_next(s); while (r >= limit);
	return (int)(r % un);
}

/* Fisher–Yates */
static void shuffle(Session *s, int *a, int n)
{
	for (int i = n - 1; i > 0; i--) {
		int j = rng_below(s, i + 1);
		int t = a[i]; a[i] = a[j]; a[j] = t;
	}
}

/* ================================================================== */
/* 公共                                                                */
/* ================================================================== */

static void begin_queue(Session *s)
{
	s->pos   = 0;
	s->phase = (s->qlen > 0) ? SP_ASKING : SP_EMPTY;
}

int sess_current(const Session *s)
{
	switch (s->phase) {
	case SP_ASKING: case SP_WRONG: case SP_CORRECT: case SP_REVEALED:
		return s->queue[s->pos];
	default:
		return -1;
	}
}

int sess_progress(const Session *s)
{
	return (sess_current(s) >= 0) ? s->pos + 1 : s->qlen;
}

static bool in_list(const int *a, int n, int v)
{
	for (int i = 0; i < n; i++) if (a[i] == v) return true;
	return false;
}

static void mark_wrong(Session *s, int widx)
{
	if (!in_list(s->wrong, s->wlen, widx)) s->wrong[s->wlen++] = widx;
}

/* ================================================================== */
/* 普通模式                                                            */
/* ================================================================== */

static void rounds_load(Session *s, const int *words, int n, int round)
{
	s->kind  = SESS_ROUNDS;
	s->round = round;
	memmove(s->round_words, words, sizeof(int) * (size_t)n);
	s->rwlen = n;
	memcpy(s->queue, s->round_words, sizeof(int) * (size_t)n);
	s->qlen = n;
	s->wlen = 0;
	begin_queue(s);
}

void sess_start_rounds(Session *s, int nwords)
{
	static int all[DICT_MAX_WORDS];
	s->nwords = nwords;
	for (int i = 0; i < nwords; i++) all[i] = i;
	rounds_load(s, all, nwords, 1);
}

void sess_next_round(Session *s)
{
	if (s->kind != SESS_ROUNDS || s->phase != SP_PREVIEW) return;
	rounds_load(s, s->wrong, s->wlen, s->round + 1);
}

static void rounds_advance(Session *s)
{
	s->pos++;
	if (s->pos < s->qlen) { s->phase = SP_ASKING; return; }
	s->pos   = s->qlen - 1;
	s->phase = (s->wlen > 0) ? SP_PREVIEW : SP_DONE;
}

/* ================================================================== */
/* 无尽模式                                                            */
/* ================================================================== */

/* 第 g 组从词表下标 start 开始，取 group_size 个 */
static void endless_load_group(Session *s, int g, int start)
{
	s->group    = g;
	s->gstart   = start;
	s->gend     = start + s->group_size;
	if (s->gend > s->nwords) s->gend = s->nwords;
	s->seen     = 0;
	s->restarts = 0;
	s->qlen     = 0;
	for (int i = s->gstart; i < s->gend; i++) s->queue[s->qlen++] = i;   /* 组内按原顺序 */
	begin_queue(s);
}

int sess_ngroups(const Session *s)
{
	if (s->kind != SESS_ENDLESS) return 0;
	int rest = s->nwords - s->gend;
	return s->group + 1 + (rest > 0 ? (rest + s->group_size - 1) / s->group_size : 0);
}

void sess_start_endless(Session *s, int nwords)
{
	s->kind       = SESS_ENDLESS;
	s->nwords     = nwords;
	s->group_size = ENDLESS_GROUP_SIZE;
	s->wlen       = 0;
	endless_load_group(s, 0, 0);
}

/* 本组错了：见过的词（含这次错的）打乱放最前，没见过的保持原序在后，从头再来 */
static void endless_reset_group(Session *s)
{
	int failed = s->queue[s->pos];
	if (s->pos + 1 > s->seen) s->seen = s->pos + 1;

	shuffle(s, s->queue, s->seen);
	/* 见过的词不止一个时，刚错的词不放第一个（刚看完答案，马上再考没有意义） */
	if (s->seen >= 2 && s->queue[0] == failed) {
		int j = 1 + rng_below(s, s->seen - 1);
		s->queue[0] = s->queue[j];
		s->queue[j] = failed;
	}

	s->restarts++;
	begin_queue(s);
}

static void endless_advance(Session *s)
{
	s->pos++;
	if (s->pos > s->seen) s->seen = s->pos;
	if (s->pos < s->qlen) { s->phase = SP_ASKING; return; }

	/* 一次不错地答完整组 → 下一组 */
	if (s->gend < s->nwords) {
		endless_load_group(s, s->group + 1, s->gend);   /* 上一组的词不再出现 */
	} else {
		s->pos   = s->qlen - 1;
		s->phase = SP_DONE;
	}
}

bool sess_failed(const Session *s)
{
	return s->kind == SESS_ENDLESS && s->phase == SP_REVEALED;
}

/* ================================================================== */
/* 动作                                                                */
/* ================================================================== */

void sess_answer(Session *s, bool correct)
{
	if (s->phase != SP_ASKING && s->phase != SP_WRONG) return;
	if (correct) {
		s->phase = SP_CORRECT;
	} else {
		s->phase = SP_WRONG;               /* 提交答错只允许重试；「不知道」才公布答案并重开 */
	}
}

void sess_dont_know(Session *s)
{
	if (s->phase != SP_ASKING && s->phase != SP_WRONG) return;
	if (s->kind == SESS_ROUNDS) mark_wrong(s, s->queue[s->pos]);
	s->phase = SP_REVEALED;
}

int sess_next(Session *s)
{
	int skipped = -1;
	switch (s->phase) {
	case SP_CORRECT:
		if (s->kind == SESS_ENDLESS) endless_advance(s);
		else                         rounds_advance(s);
		break;
	case SP_WRONG:
		if (s->kind == SESS_ENDLESS) break;  /* 无尽模式只能靠「不知道」触发重开 */
		skipped = s->queue[s->pos];
		mark_wrong(s, skipped);
		rounds_advance(s);
		break;
	case SP_REVEALED:
		if (s->kind == SESS_ENDLESS) endless_reset_group(s);
		else                         rounds_advance(s);
		break;
	default:
		break;
	}
	return skipped;
}

void sess_restart(Session *s)
{
	if (s->kind == SESS_ENDLESS) {
		sess_start_endless(s, s->nwords);
	} else if (s->round == 1 || s->phase == SP_DONE) {
		sess_start_rounds(s, s->nwords);
	} else {
		rounds_load(s, s->round_words, s->rwlen, s->round);
	}
}

void sess_reask(Session *s)
{
	switch (s->phase) {
	case SP_DONE:
		sess_restart(s);
		break;
	case SP_WRONG:
	case SP_CORRECT:
		s->phase = SP_ASKING;
		break;
	case SP_REVEALED:
		if (s->kind == SESS_ROUNDS) s->phase = SP_ASKING;   /* 已记为错词，换题型再答一次 */
		break;
	default:
		break;
	}
}

void sess_words_changed(Session *s, int nwords)
{
	s->nwords = nwords;
	if (nwords == 0) {
		s->qlen  = 0;
		s->pos   = 0;
		s->wlen  = 0;
		s->phase = SP_EMPTY;
		return;
	}
	if (s->phase == SP_EMPTY) {
		if (s->kind == SESS_ENDLESS) sess_start_endless(s, nwords);
		else                         sess_start_rounds(s, nwords);
	}
}
