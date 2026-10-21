/**
 * @file dict_words.h
 * @brief 后端 · 词表：读 dictation_extra.json、追加写回、批量导入解析、清空。
 *
 * 文件格式：[["word","pos","meaning"], ...]（与 Python 版共用）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DICT_MAX_WORDS   2000
#define DICT_MAX_WORD    96
#define DICT_MAX_POS     96
#define DICT_MAX_MEANING 640

typedef struct Word {
	char word[DICT_MAX_WORD];
	char pos[DICT_MAX_POS];
	char meaning[DICT_MAX_MEANING];
} Word;

typedef struct WordList {
	Word words[DICT_MAX_WORDS];
	int  n;
	char path[520];   /* 词表文件（追加/清空写到这里） */
} WordList;

/* 批量导入结果 */
typedef struct AddStat {
	int  added, dup, bad;
	char err[200];    /* 非空 = 整批被拒（格式混杂/数量不一致/写盘失败…） */
} AddStat;

/* 读 path 进 wl（并记住 path）；文件不存在/解析失败返回 false，wl->n = 0 */
bool words_load(WordList *wl, const char *path);

/* 多行文本 → 追加进词表（内存 + 文件）。两种格式：
 *   ① 两段式：先全部英文单词（一行一个），再全部中文释义（一行一个），数量要一致
 *   ② 单行式：每行「单词 词性 释义」；单词可以是短语，到第一个中文字符为止
 *     （末尾的词性归释义），也可以用 Tab / = / : / - 隔开，如 "account for v. 解释"
 *   ③ 交替式：英文一行、中文一行
 * 已有的词（不分大小写）自动跳过。 */
void words_add_text(WordList *wl, const char *text, AddStat *st);

/* 整个词表按标准格式写回文件（改了释义后用；手写的排版不保留） */
bool words_save(const WordList *wl);

/* 清空：删除词表文件、内存清零 */
void words_clear(WordList *wl);

/* "n. 过程 v. 处理" → pos="n. v."、meaning="过程 处理"（规则同 Python 版） */
void split_pos_meaning(const char *rest, char *pos, size_t pc, char *meaning, size_t mc);
