/**
 * @file dict_session.h
 * @brief 后端 · 出题状态机：普通轮次模式 + 无尽模式。
 *
 * 只处理「出哪个词、现在处于什么阶段」，不碰界面、不碰判分：
 * 前端判完对错后调 sess_answer()，按钮各调对应函数即可。
 *
 * ── 普通模式（轮次）──────────────────────────────────────────────
 *   第 1 轮按词表顺序出全部词；答错可再试，点「下一个」公布答案并记为错词；
 *   一轮结束有错词 → 错词预览 → 下一轮只默错词；全对 → 完成。
 *
 * ── 无尽模式 ─────────────────────────────────────────────────────
 *   词表按顺序每 20 个分一组，一组一组过：
 *   · 组内按原顺序出词；
 *   · 提交答错可以继续重试；只有点「不知道」才公布答案，按钮变成「重新开始」；
 *   · 点「重新开始」→ 本组所有见过的词（之前答对的 + 这次错的）打乱顺序排在最前，
 *     没见过的词保持原顺序跟在后面，从头再答；
 *   · 本组一次不错地答到最后一个 → 本组通过，进入下一组（上一组的词不再出现）；
 *   · 一个词一次答对（没先提交答错）满 15 次 → 移出本组，从后面的词里按顺序补一个进来，本组保持 20 个；
 *     补进来的词算没见过，排在本组最后。词用完了就不补，本组变少，全部移出也算本组过关；
 *     「不知道」不清零次数，从第 1 组重新开始才清零；
 *   · 所有组都通过 → 无尽模式完成。
 *   · 也可以只练一部分词（普通模式一轮的错词）：sess_start_endless_pool 把这些词打乱后同样每 20 个一组；
 *     这时之后新加进词库的词不进来，「重新开始」还是这批词。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dict_words.h"   /* DICT_MAX_WORDS */

#define ENDLESS_GROUP_SIZE 20
#define ENDLESS_RETIRE_HITS 15   /* 一次答对满这么多次就移出本组 */

typedef enum SessKind {
	SESS_ROUNDS = 0,   /* 普通模式 */
	SESS_ENDLESS,      /* 无尽模式 */
} SessKind;

typedef enum SessPhase {
	SP_EMPTY = 0,   /* 词表为空 */
	SP_ASKING,      /* 等待作答 */
	SP_WRONG,       /* 普通模式：答错了，可再试 */
	SP_CORRECT,     /* 答对了，等「下一个」 */
	SP_REVEALED,    /* 已公布答案，等「下一个」（无尽模式里是「重新开始」） */
	SP_PREVIEW,     /* 普通模式：本轮结束，错词预览 */
	SP_DONE,        /* 全部完成 */
} SessPhase;

typedef struct Session {
	SessKind  kind;
	SessPhase phase;
	int       nwords;

	int queue[DICT_MAX_WORDS];   /* 当前出题顺序（词表下标） */
	int qlen;
	int pos;                     /* 当前词在 queue 里的位置（0 起） */

	/* 普通模式 */
	int round;                         /* 第几轮（1 起） */
	int round_words[DICT_MAX_WORDS];   /* 本轮的词（「重新开始」恢复用） */
	int rwlen;
	int wrong[DICT_MAX_WORDS];         /* 本轮错词（按出错顺序） */
	int wlen;

	/* 无尽模式 */
	int group_size;
	int group;       /* 当前组（0 起） */
	int gstart;      /* 当前组从第 gstart 个词开始；[gend, 总数) 是还没进过组的词 */
	int gend;        /* 本组移出词后从 gend 往里补，所以本组的词 = queue，不一定是 [gstart, gend) 全部 */
	int seen;        /* queue 前 seen 个词是本组已经见过的 */
	int restarts;    /* 本组已重开次数 */

	uint64_t rng;

	/* 无尽模式只练一部分词：pool[0..npool) 是词表下标（已打乱）；npool = 0 表示整个词表。
	 * 放在结构体末尾：旧版进度存档没有这两项，读进来是 0，正好是「整个词表」 */
	int pool[DICT_MAX_WORDS];
	int npool;

	/* 无尽模式「一次答对满 15 次就移出」。同样放在末尾：2026-11-14 前的存档读进来全是 0 */
	int hits[DICT_MAX_WORDS];   /* 每个词（词表下标）一次答对的次数 */
	int fumbled;                /* 当前这题答错过或已经计过次数（再答对不计） */
	int retired, joined;        /* 刚才 sess_next 移出的词 / 补进来的词（词表下标，没有是 -1） */
} Session;

void sess_seed(Session *s, uint64_t seed);

void sess_start_rounds(Session *s, int nwords);
void sess_start_endless(Session *s, int nwords);
/* 无尽模式只练 words[0..n)（打乱后分组）；n = 0 等于 sess_start_endless */
void sess_start_endless_pool(Session *s, int nwords, const int *words, int n);

/* 当前题目的词表下标；没有题目（空/预览/完成）返回 -1 */
int  sess_current(const Session *s);

/* 前端判完对错后调用（仅 SP_ASKING / SP_WRONG 有效） */
void sess_answer(Session *s, bool correct);

/* 「不知道」（仅 SP_ASKING / SP_WRONG 有效）→ SP_REVEALED */
void sess_dont_know(Session *s);

/* 「下一个」/「重新开始本组」（SP_CORRECT / SP_WRONG / SP_REVEALED 有效）。
 * 普通模式在 SP_WRONG 下跳过时，返回被跳过的词下标（前端用来公布答案），否则返回 -1。 */
int  sess_next(Session *s);

/* 无尽模式里本组已错、下一步是重开本组（前端据此把「下一个」显示成「重新开始」） */
bool sess_failed(const Session *s);

/* 底部「重新开始」：普通模式重开本轮（第 1 轮/已完成则从头）；无尽模式从第 1 组重来 */
void sess_restart(Session *s);

/* 错词预览页「开始默写」：下一轮只默本轮错词 */
void sess_next_round(Session *s);

/* 切换题型后重新作答当前词（无尽模式已错的不撤销）；完成页则直接重开 */
void sess_reask(Session *s);

/* 词表增删后同步词数（清空 → 回到空状态） */
void sess_words_changed(Session *s, int nwords);

/* 进度：当前是第几题（1 起）/ 共几题 */
int  sess_progress(const Session *s);

/* 无尽模式总组数（按当前词数算） */
int  sess_ngroups(const Session *s);

/* 无尽模式里一次答对满 ENDLESS_RETIRE_HITS 次、已经移出的词数 */
int  sess_mastered(const Session *s);

/* 无尽模式本次累计一次答对的次数（所有词 hits 之和，答对马上加 1） */
int  sess_total_hits(const Session *s);

/* 无尽模式已经过关的词数（之前的组 + 本组移出的） */
int  sess_endless_passed(const Session *s);

/* 无尽模式要练的词数（只练一部分词时是 npool，否则是整个词表） */
int  sess_endless_total(const Session *s);
