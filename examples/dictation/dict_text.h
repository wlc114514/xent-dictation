/**
 * @file dict_text.h
 * @brief 后端 · 文本工具：UTF-8、判分规则、首字母提示、最小 JSON 读写。
 *
 * 纯 C，不依赖 Win32 / UI，可以单独编进无头测试。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* 解一个 UTF-8 码点，返回消耗字节数（非法字节按 1 字节 U+FFFD 处理） */
int  utf8_decode(const unsigned char *s, uint32_t *cp);

/* 收集汉字（只认 U+4E00–U+9FFF，与 Python 版一致），返回个数 */
int  utf8_cjk(const char *s, uint32_t *out, int max);
bool has_cjk(const char *s);

void lower_copy(const char *src, char *dst, size_t cap);
bool str_ci_eq(const char *a, const char *b);

/* 空白/纯标点：没有 0-9 / a-z / 汉字 就算空白 */
bool is_blank(const char *s);

/* 「单词 → 释义」宽松判分（与 Python 版 meaning_match 同规则） */
bool meaning_match(const char *ans, const char *meaning);

/* ---- 一词多义 ---- */
#define DICT_MAX_SENSES 16    /* 不超过 32：答出的义项用 uint32_t 位掩码记 */
#define DICT_SENSE_LEN  128

/* 把释义拆成义项：、，,；;/ 分隔，两个汉字之间的空格也算；括号里的不拆，重复的只留一个。
 * "行为、传导; 实施、传导" → 行为 / 传导 / 实施；"使沉淀、(水汽等)凝结(成雨、露、雪等)" → 2 个 */
int  split_senses(const char *meaning, char out[][DICT_SENSE_LEN], int max);

/* 答案（可一次写多个，同样分隔）逐条对还没答出的义项，命中的在 *got 里置位；返回新答出的个数 */
int  senses_mark(const char *ans, char senses[][DICT_SENSE_LEN], int ns, uint32_t *got);

/* 「释义 → 单词」判对：不分大小写、忽略 '.'、多余空格不计（"take sth into account" == "take sth. into  account"） */
bool answer_eq(const char *ans, const char *word);

/* 「释义 → 单词」首字母提示：前半段原样、其余挖空，如 "s w _ _ _"；短语逐词处理 */
void masked_display(const char *word, char *out, size_t cap);

/* ---- 最小 JSON 工具 ---- */
const char *skip_ws(const char *p);
/* 解析一个 JSON 字符串（*p 必须是 '"'），UTF-8 写入 out；失败返回 NULL */
const char *js_str(const char *p, char *out, size_t cap);
/* 把任意 UTF-8 文本转义成 JSON 字符串内容（不含两侧引号） */
void json_escape(const char *src, char *dst, size_t cap);
/* 在扁平 JSON 对象里找 "key": 后的值起点；没有返回 NULL */
const char *json_find_key(const char *buf, const char *key);
/* 按 UTF-8 路径打开/删除文件（Windows 下走宽字符 API，中文路径也能用） */
FILE *u8_fopen(const char *path, const char *mode);
int   u8_remove(const char *path);
/* 整个文件读进内存（以 0 结尾，调用方 free）；失败返回 NULL */
char *read_file(const char *path, long *out_sz);
