/**
 * @file dict_words.c
 * @brief 后端 · 词表读写与批量导入（规则与 Python 版 AddWordsDialog 一致）。
 */
#include "dict_words.h"
#include "dict_text.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_word(Word *it, const char *w, const char *pos, const char *mn)
{
	snprintf(it->word, sizeof it->word, "%s", w ? w : "");
	snprintf(it->pos, sizeof it->pos, "%s", pos ? pos : "");
	snprintf(it->meaning, sizeof it->meaning, "%s", mn ? mn : "");
}

/* ================================================================== */
/* 读                                                                  */
/* ================================================================== */

/* 解析一个 "..." 再吃掉后面的逗号；失败返回 NULL */
static const char *field(const char *p, char *out, size_t cap)
{
	p = js_str(p, out, cap);
	if (!p) return NULL;
	p = skip_ws(p);
	if (*p == ',') p = skip_ws(p + 1);
	return p;
}

bool words_load(WordList *wl, const char *path)
{
	wl->n = 0;
	snprintf(wl->path, sizeof wl->path, "%s", path);
	char *buf = read_file(path, NULL);
	if (!buf) return false;

	const char *p = skip_ws(buf);
	if (*p != '[') { free(buf); return false; }
	p = skip_ws(p + 1);
	while (*p && *p != ']') {
		if (*p == ',') { p = skip_ws(p + 1); continue; }
		if (*p != '[') break;
		p = skip_ws(p + 1);

		char w[DICT_MAX_WORD] = {0}, ps[DICT_MAX_POS] = {0}, mn[DICT_MAX_MEANING] = {0};
		if (!(p = field(p, w, sizeof w)))   break;
		if (!(p = field(p, ps, sizeof ps))) break;
		if (!(p = field(p, mn, sizeof mn))) break;

		/* 跳过本项多余字段，直到 ']' */
		while (*p && *p != ']') {
			if (*p == '"') {
				char tmp[16];
				if (!(p = js_str(p, tmp, sizeof tmp))) break;
			} else {
				p++;
			}
			p = skip_ws(p);
		}
		if (!p) break;
		if (*p == ']') p = skip_ws(p + 1);

		if (w[0] && mn[0] && wl->n < DICT_MAX_WORDS) put_word(&wl->words[wl->n++], w, ps, mn);
	}
	free(buf);
	return true;
}

/* ================================================================== */
/* 写                                                                  */
/* ================================================================== */

/* 顶层数组的收尾 ']'（带字符串语义，释义里出现 ']' 也不会认错） */
static char *top_level_end(char *buf)
{
	int  depth = 0;
	bool in_str = false, esc = false;
	for (char *p = buf; *p; p++) {
		char c = *p;
		if (in_str) {
			if (esc) esc = false;
			else if (c == '\\') esc = true;
			else if (c == '"') in_str = false;
			continue;
		}
		if (c == '"') in_str = true;
		else if (c == '[') depth++;
		else if (c == ']' && --depth <= 0) return p;
	}
	return NULL;
}

/* 保留文件原有内容（包括手写的格式），只往顶层数组末尾插 */
static bool append_to_file(const char *path, const Word *items, int n)
{
	if (n <= 0) return true;
	char *buf = read_file(path, NULL);
	char *end = buf ? top_level_end(buf) : NULL;
	FILE *f = u8_fopen(path, "wb");
	if (!f) { free(buf); return false; }

	fputs("[\n", f);
	if (buf && end) {
		*end = 0;
		size_t len = strlen(buf);
		while (len > 0 && strchr(" \t\r\n", buf[len - 1])) buf[--len] = 0;
		const char *body = skip_ws(buf);
		if (*body == '[') body++;
		while (*body == '\r' || *body == '\n') body++;   /* 保留第一项原有的缩进 */
		if (*skip_ws(body)) {
			fputs(body, f);
			fputs(",\n", f);
		}
	}
	for (int i = 0; i < n; i++) {
		char w[DICT_MAX_WORD * 7], ps[DICT_MAX_POS * 7], mn[DICT_MAX_MEANING * 7];
		json_escape(items[i].word, w, sizeof w);
		json_escape(items[i].pos, ps, sizeof ps);
		json_escape(items[i].meaning, mn, sizeof mn);
		fprintf(f, "  [\"%s\", \"%s\", \"%s\"]%s\n", w, ps, mn, (i + 1 < n) ? "," : "");
	}
	fputs("]\n", f);
	bool ok = fclose(f) == 0;
	free(buf);
	return ok;
}

void words_clear(WordList *wl)
{
	if (wl->path[0]) u8_remove(wl->path);
	wl->n = 0;
}

/* ================================================================== */
/* 词性 / 释义拆分                                                     */
/* ================================================================== */

static bool is_cjk_punct(const char *s)
{
	static const char *const P[] = {"，", "。", "；", "：", "、", "（", "】", NULL};
	for (int i = 0; P[i]; i++)
		if (strncmp(s, P[i], strlen(P[i])) == 0) return true;
	return false;
}

/* 与 Python 版 _POS_RE 同集合；长的在前，避免短词先命中 */
static const char *const POS_TOKENS[] = {
	"prep&adv", "n&adj", "adj&n", "n&v", "v&n", "adj&adv", "adj&v",
	"conj", "adj", "adv", "prep", "n", "v", NULL
};
static const char *const POS_BARE[] = {"n", "v", "adj", "adv", "prep", "conj", NULL};

/* 在 s（整串起点 begin）处试配一个词性标记；返回消耗字节数，标记写入 tok */
static int pos_try(const char *s, const char *begin, char *tok, size_t cap)
{
	for (int i = 0; POS_TOKENS[i]; i++) {
		size_t L = strlen(POS_TOKENS[i]);
		if (strncmp(s, POS_TOKENS[i], L) != 0) continue;
		if (isalnum((unsigned char)s[L])) continue;                /* \b 右侧 */
		if (s > begin && isalnum((unsigned char)s[-1])) continue;   /* \b 左侧 */

		const char *p = s + L;
		while (*p == ' ') p++;
		bool dotted = (*p == '.');
		if (dotted) p++;
		bool bare = false;
		for (int k = 0; POS_BARE[k]; k++) if (strcmp(POS_TOKENS[i], POS_BARE[k]) == 0) bare = true;
		/* 写了点的保留点；裸写的单一词性补点（与 Python 一致） */
		snprintf(tok, cap, "%s%s", POS_TOKENS[i], (dotted || bare) ? "." : "");
		return (int)(p - s);
	}
	return 0;
}

void split_pos_meaning(const char *rest, char *pos, size_t pc, char *meaning, size_t mc)
{
	char toks[8][40];
	int  nt = 0;
	pos[0] = meaning[0] = 0;
	if (!rest) return;

	/* 1) 抽掉词性标记，& → 空格 */
	char   tmp[4096];
	size_t tj = 0;
	for (const char *p = rest; *p && tj + 8 < sizeof tmp;) {
		char tok[40];
		int  used = pos_try(p, rest, tok, sizeof tok);
		if (used > 0) {
			bool dup = false;
			for (int i = 0; i < nt; i++) if (strcmp(toks[i], tok) == 0) dup = true;
			if (!dup && nt < 8) snprintf(toks[nt++], sizeof toks[0], "%s", tok);
			p += used;
			continue;
		}
		if (*p == '&') { tmp[tj++] = ' '; p++; continue; }
		uint32_t cp;
		int n = utf8_decode((const unsigned char *)p, &cp);
		memcpy(tmp + tj, p, (size_t)n);
		tj += (size_t)n;
		p += n;
	}
	tmp[tj] = 0;

	/* 2) 压空白、去首尾、去掉中文标点前的空格 */
	size_t oj = 0;
	bool   pending_sp = false;
	for (const char *q = tmp; *q;) {
		if (*q == ' ' || *q == '\t') { if (oj > 0) pending_sp = true; q++; continue; }
		uint32_t cp;
		int n = utf8_decode((const unsigned char *)q, &cp);
		if (pending_sp && !is_cjk_punct(q) && oj + (size_t)n + 2 < mc) meaning[oj++] = ' ';
		pending_sp = false;
		if (oj + (size_t)n + 1 < mc) { memcpy(meaning + oj, q, (size_t)n); oj += (size_t)n; }
		q += n;
	}
	meaning[oj] = 0;

	/* 3) 词性去重后以空格连接 */
	for (int i = 0; i < nt; i++) {
		size_t have = strlen(pos);
		if (have + strlen(toks[i]) + 2 >= pc) break;
		if (i) strcat(pos, " ");
		strcat(pos, toks[i]);
	}
}

/* ================================================================== */
/* 批量导入                                                            */
/* ================================================================== */

/* 取行首的「单词」，返回其余部分（没有词/没有其余部分 → NULL） */
static const char *split_first_word(const char *line, char *word, size_t wc)
{
	const char *p = line;
	while (*p == ' ' || *p == '\t') p++;
	if (!*p) return NULL;
	const char *e = p;
	while (*e && *e != ' ' && *e != '\t') e++;
	size_t len = (size_t)(e - p);
	if (len + 1 > wc) len = wc - 1;
	memcpy(word, p, len);
	word[len] = 0;
	while (*e == ' ' || *e == '\t') e++;
	return (*e && word[0]) ? e : NULL;
}

static bool exists_in(const Word *items, int n, const char *w)
{
	for (int i = 0; i < n; i++) if (str_ci_eq(items[i].word, w)) return true;
	return false;
}

/* 把 buf 就地切成去掉首尾空白的非空行 */
static int split_lines(char *buf, char **lines, int max)
{
	int   n = 0;
	char *p = buf;
	while (*p && n < max) {
		char *e = p;
		while (*e && *e != '\n' && *e != '\r') e++;
		char save = *e;
		*e = 0;
		while (*p == ' ' || *p == '\t') p++;
		char *q = p + strlen(p);
		while (q > p && (q[-1] == ' ' || q[-1] == '\t')) *--q = 0;
		if (*p) lines[n++] = p;
		if (!save) break;
		e++;
		if (save == '\r' && *e == '\n') e++;
		p = e;
	}
	return n;
}

/* 候选词：解析出释义、去重后放进 items */
static void take(const WordList *wl, Word *items, int *ni, AddStat *st,
                 const char *word, const char *pos_meaning)
{
	char pos[DICT_MAX_POS] = {0}, mn[DICT_MAX_MEANING] = {0};
	split_pos_meaning(pos_meaning, pos, sizeof pos, mn, sizeof mn);
	if (!mn[0]) { st->bad++; return; }
	if (exists_in(wl->words, wl->n, word) || exists_in(items, *ni, word)) { st->dup++; return; }
	put_word(&items[(*ni)++], word, pos, mn);
}

void words_add_text(WordList *wl, const char *text, AddStat *st)
{
	memset(st, 0, sizeof *st);
	size_t len = strlen(text);
	if (len == 0) { snprintf(st->err, sizeof st->err, "没有内容"); return; }

	int    maxl  = (int)(len / 2 + 4);
	char  *buf   = (char *)malloc(len + 1);
	char **lines = (char **)malloc(sizeof(char *) * (size_t)maxl);
	Word  *items = (Word *)calloc((size_t)maxl, sizeof(Word));
	int    ni    = 0;
	if (!buf || !lines || !items) {
		snprintf(st->err, sizeof st->err, "内存不足");
		goto done;
	}
	memcpy(buf, text, len + 1);

	int nl = split_lines(buf, lines, maxl);
	if (nl == 0) { snprintf(st->err, sizeof st->err, "没有可读的行"); goto done; }

	int  first_cjk = -1;
	bool any_ascii = false;
	for (int i = 0; i < nl; i++) {
		bool c = has_cjk(lines[i]);
		if (c && first_cjk < 0) first_cjk = i;
		if (!c) any_ascii = true;
	}

	if (any_ascii) {
		/* 两段式：前 eng_n 行英文、后 zh_n 行中文 */
		int eng_n = (first_cjk < 0) ? nl : first_cjk;
		int zh_n  = nl - eng_n;
		for (int i = eng_n; i < nl; i++) {
			if (!has_cjk(lines[i])) {
				snprintf(st->err, sizeof st->err, "格式混杂：英文行和中文行交错，请检查后重试");
				goto done;
			}
		}
		if (eng_n != zh_n) {
			snprintf(st->err, sizeof st->err, "英文 %d 个、中文 %d 行，数量不一致", eng_n, zh_n);
			goto done;
		}
		for (int i = 0; i < eng_n; i++) take(wl, items, &ni, st, lines[i], lines[eng_n + i]);
	} else {
		/* 单行式：每行「单词 词性 释义」 */
		for (int i = 0; i < nl; i++) {
			char w[DICT_MAX_WORD] = {0};
			const char *rest = split_first_word(lines[i], w, sizeof w);
			if (!rest) { st->bad++; continue; }
			take(wl, items, &ni, st, w, rest);
		}
	}

	if (ni == 0) goto done;
	if (wl->n + ni > DICT_MAX_WORDS) ni = DICT_MAX_WORDS - wl->n;
	if (ni <= 0) { snprintf(st->err, sizeof st->err, "词表已满（上限 %d 词）", DICT_MAX_WORDS); goto done; }
	if (!append_to_file(wl->path, items, ni)) {
		snprintf(st->err, sizeof st->err, "写入词表失败：%s", wl->path);
		goto done;
	}
	memcpy(&wl->words[wl->n], items, sizeof(Word) * (size_t)ni);
	wl->n += ni;
	st->added = ni;

done:
	free(buf);
	free(lines);
	free(items);
}
