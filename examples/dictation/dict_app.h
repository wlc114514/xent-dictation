/**
 * @file dict_app.h
 * @brief 前端 · 模型与消息（dictation.c 处理消息，dict_view.c 画界面）。
 */
#pragma once

#include <xtk/xtk.h>

#include <stdbool.h>
#include <stdint.h>

#include "dict_judge.h"
#include "dict_session.h"
#include "dict_text.h"
#include "dict_words.h"

#define DICT_MAX_ANSWER 512

enum {
	MSG_NAV = 1,       /* 导航栏切页（.i = 条目下标） */
	MSG_DIR,           /* 题型选择条（.i = Dir） */
	MSG_ANSWER,        /* 输入框内容变化（.ptr = 文本） */
	MSG_ENTER,         /* 输入框里按回车 */
	MSG_SUBMIT,
	MSG_DONT,
	MSG_NEXT,          /* 下一个 / 无尽模式的「重新开始」 */
	MSG_RESTART,       /* 从头开始 / 再来一轮 */
	MSG_START_ROUND,   /* 错词预览页「开始下一轮」 */
	MSG_JUDGED,        /* 后台判分完成（.i = 请求号，.d = 概率，-1 = 不可用） */
	MSG_GO_LIBRARY,
	MSG_ADD_DIALOG,    /* 打开多行输入窗口 */
	MSG_ADD_DONE,      /* 输入窗口提交完毕（刷新提示） */
	MSG_ADD_CLIP,
	MSG_ADD_MSG_CLOSE,
	MSG_CLEAR_ASK,     /* 打开「清空词库」确认框 */
	MSG_CLEAR_RESULT,  /* 确认框结果（.i = XtkDialogResult） */
	MSG_ALL_SENSES,    /* 「多义词要全答」开关（.b = 新状态） */
};

/* 导航栏条目（下标即 MSG_NAV 的 .i） */
typedef enum Page {
	PAGE_PRACTICE = 0,
	PAGE_ENDLESS,
	PAGE_SEP,
	PAGE_LIBRARY,
	PAGE_HELP,
	PAGE_COUNT,
} Page;

typedef enum Dir { DIR_CN2EN = 0, DIR_EN2CN } Dir;   /* 看释义写单词 / 看单词写释义 */

/* 题目下方的反馈条 */
typedef struct Feedback {
	bool               on;
	XtkInfoBarSeverity sev;
	char               title[64];
	char               msg[800];
} Feedback;

/* 一套练习：普通模式、无尽模式各一套，切页互不影响 */
typedef struct Drill {
	Session  s;
	Feedback fb;
	int      answered;   /* 本次练习答过的题数（统计卡用） */
	int      correct;

	/* 一词多义（单词 → 释义）：当前词拆出的义项、已答出的位掩码 */
	char     senses[DICT_MAX_SENSES][DICT_SENSE_LEN];
	int      nsense;
	uint32_t got;
	int      sense_of;   /* 义项属于哪个词（词表下标 + 1；0 = 需要重算） */
} Drill;

typedef struct App {
	WordList wl;
	uint32_t wl_rev;     /* 词表版本号：词库列表只在变化时重建 */
	Judge   *judge;

	Page  page;
	Dir   dir;
	Drill drill[2];      /* [0] 普通模式，[1] 无尽模式 */
	char  answer[DICT_MAX_ANSWER];

	int  judge_seq;      /* 后台判分请求号（过期结果直接丢弃） */
	bool judging;

	Feedback add_fb;     /* 词库页的导入结果 */
	bool     clear_open; /* 「清空词库」确认框 */

	bool all_senses;     /* 一词多义要把每个意思都答出来；关掉则答出任一个就算对 */
	char settings_path[DICT_MAX_ANSWER];   /* dictation_settings.json（保存上面的开关） */

	bool dark;
} App;

/* 当前页对应的练习（词库/说明页返回 NULL） */
Drill *app_drill(App *a);

/* 让 d->senses 对上当前题目（换词时重拆、清空已答出）；返回已答出的个数 */
int drill_senses(const App *a, Drill *d);

/* 视图入口（dict_view.c）：app_view = 导航栏 + 当前页；app_page = 只有当前页（无头布局自检用） */
XtkEl *app_view(XtkUi *ui, void *model);
XtkEl *app_page(XtkUi *ui, void *model);
