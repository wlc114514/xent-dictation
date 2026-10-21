/**
 * @file dict_view.c
 * @brief 前端 · 视图。WinUI 3 / Fluent 版式：
 *   NavigationView（与标题栏一体）→ 页标题 28 号 → 卡片承载内容 → InfoBar 给反馈。
 *   颜色只在两处自定：首字母提示用强调色、说明文字用次要灰；其余全部跟随系统主题。
 */
#include "dict_app.h"
#include "dict_text.h"

#include <math.h>
#include <stdio.h>

static const XtkColor COL_SECONDARY = {0x8A8A8AFF};

static XtkColor accent(const App *a) { return (XtkColor){a->dark ? 0x60CDFFFF : 0x005FB8FF}; }

/* ================================================================== */
/* 通用部件                                                            */
/* ================================================================== */

static XtkEl *caption(XtkUi *ui, const char *t)
{
	return xtk_text(ui, t, (XtkTextDesc){.size = 12, .color = COL_SECONDARY});
}

static XtkEl *body(XtkUi *ui, const char *t)
{
	return xtk_text(ui, t, (XtkTextDesc){.size = 13, .color = COL_SECONDARY});
}

static XtkEl *subtitle(XtkUi *ui, const char *t)
{
	return xtk_text(ui, t, (XtkTextDesc){.size = 16, .weight = XTK_FONT_SEMI_BOLD});
}

/* 页标题：左边大标题 + 说明，右边放页级操作 */
static XtkEl *page_header(XtkUi *ui, const char *title, const char *blurb, XtkEl *trailing)
{
	return xtk_row(ui, (XtkStackDesc){.gap = 16, .align = XENT_FLEX_ALIGN_CENTER, .justify = XENT_FLEX_JUSTIFY_SPACE_BETWEEN, .fill = true},
	    xtk_kids(
	        xtk_shrink(xtk_column(ui, (XtkStackDesc){.gap = 6},
	            xtk_kids(
	                xtk_text(ui, title, (XtkTextDesc){.size = 28, .weight = XTK_FONT_SEMI_BOLD}),
	                blurb ? body(ui, blurb) : NULL)), 1.0f),
	        trailing ? xtk_shrink(xtk_row(ui, (XtkStackDesc){0}, xtk_kids(trailing)), 0.0f) : NULL));   /* 控件不接受伸缩属性，套一层 */
}

/* 带标题的分区卡片 */
static XtkEl *section(XtkUi *ui, const char *title, const char *desc, XtkEl *content)
{
	return xtk_card(ui, (XtkStackDesc){.gap = 12, .padding = {20, 18, 20, 18}, .align = XENT_FLEX_ALIGN_STRETCH},
	    xtk_kids(
	        subtitle(ui, title),
	        desc ? body(ui, desc) : NULL,
	        content));
}

/* 统计小卡：说明 + 大数字 */
static XtkEl *stat_tile(XtkUi *ui, const char *label, const char *value)
{
	return xtk_grow(xtk_card(ui, (XtkStackDesc){.gap = 4, .padding = {18, 14, 18, 14}},
	    xtk_kids(
	        caption(ui, label),
	        xtk_text(ui, value, (XtkTextDesc){.size = 24, .weight = XTK_FONT_SEMI_BOLD}))), 1.0f);
}

static XtkEl *feedback_bar(XtkUi *ui, const Feedback *fb, XtkMsg close)
{
	if (!fb->on) return NULL;
	return xtk_infobar(ui, fb->title, fb->msg, (XtkInfoBarDesc){.severity = fb->sev, .closable = close.tag != 0, .close = close});
}

/* 普通页：整页可滚动 */
static XtkEl *scroll_page(XtkUi *ui, Page p, XtkEl *content)
{
	return xtk_grow(xtk_keyed(ui, xtk_fmt(ui, "page-%d", p),
	    xtk_scroll(ui, xtk_kids(
	        xtk_shrink(xtk_column(ui, (XtkStackDesc){.padding = {36, 28, 36, 28}, .align = XENT_FLEX_ALIGN_STRETCH},
	            xtk_kids(content)), 0.0f)))), 1.0f);   /* 长内容不能被压到视口高度 */
}

/* 练习页：不滚动，卡片撑满剩余高度 */
static XtkEl *fill_page(XtkUi *ui, const char *key, XtkEl *kids[])
{
	return xtk_grow(xtk_keyed(ui, key,
	    xtk_column(ui, (XtkStackDesc){.gap = 20, .padding = {36, 28, 36, 28}, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	        kids)), 1.0f);
}

/* 定宽列（文字节点本身不接受尺寸属性，套一层容器） */
static XtkEl *cell(XtkUi *ui, XtkEl *el, float w)
{
	return xtk_sized(xtk_column(ui, (XtkStackDesc){0}, xtk_kids(el)), w, NAN);
}

/* 词表一行：序号 · 单词 · 词性 · 释义 */
static XtkEl *word_row(XtkUi *ui, int no, const Word *w)
{
	return xtk_row(ui, (XtkStackDesc){.gap = 16, .align = XENT_FLEX_ALIGN_CENTER, .padding = {4, 8, 4, 8}},
	    xtk_kids(
	        cell(ui, caption(ui, xtk_fmt(ui, "%d", no)), 36),
	        cell(ui, xtk_text(ui, w->word, (XtkTextDesc){.size = 15, .weight = XTK_FONT_SEMI_BOLD}), 190),
	        cell(ui, caption(ui, w->pos), 72),
	        xtk_shrink(xtk_grow(xtk_column(ui, (XtkStackDesc){0}, xtk_kids(xtk_text(ui, w->meaning, (XtkTextDesc){.size = 14}))), 1.0f), 1.0f)));
}

/* 虚拟化词表：只构建视口里看得见的几行，词再多也不卡 */
typedef struct ListEnv {
	const App *a;
	const int *idx;   /* NULL = 整个词表 */
} ListEnv;

static XtkEl *list_item(XtkUi *ui, void *env, int i)
{
	const ListEnv *e = (const ListEnv *)env;
	return word_row(ui, i + 1, &e->a->wl.words[e->idx ? e->idx[i] : i]);
}

static XtkEl *word_list(XtkUi *ui, ListEnv *env, int n)
{
	return xtk_grow(xtk_list(ui, (XtkListDesc){.nitem = n, .itemh = 44, .item = list_item, .env = env,
	                                          .sel_mode = XTK_LIST_SELECT_NONE, .sel = -1}), 1.0f);
}

/* ================================================================== */
/* 练习页（普通模式 / 无尽模式共用）                                    */
/* ================================================================== */

static const XtkSelectorItem DIR_ITEMS[] = {
	{.text = "释义 → 单词", .icon = "Edit"},
	{.text = "单词 → 释义", .icon = "Font"},
};

static XtkEl *drill_tools(XtkUi *ui, const App *a, bool endless)
{
	return xtk_row(ui, (XtkStackDesc){.gap = 8, .align = XENT_FLEX_ALIGN_CENTER},
	    xtk_kids(
	        xtk_selector(ui, (XtkSelectorBarDesc){.items = DIR_ITEMS, .nitem = 2, .sel = (int)a->dir, .select = xtk_msg(MSG_DIR)}),
	        xtk_tooltip(ui,
	            xtk_checkbox(ui, "多义词全答", (XtkToggleDesc){.checked = a->all_senses, .change = xtk_msg(MSG_ALL_SENSES)}),
	            a->all_senses ? "单词 → 释义：有几个意思要全部写出来" : "单词 → 释义：写出任意一个意思就算对"),
	        xtk_tooltip(ui,
	            xtk_button(ui, NULL, (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .icon = "Refresh", .click = xtk_msg(MSG_RESTART)}),
	            endless ? "从第 1 组重新开始" : "从第 1 轮重新开始")));
}

static const char *drill_blurb(XtkUi *ui, const App *a, const Session *s, bool endless)
{
	if (endless) return "每 20 词一组 · 错一次本组打乱重来 · 整组全对才进下一组";
	if (s->round == 1) return xtk_fmt(ui, "按词表顺序默写全部 %d 个词，错的词进下一轮。", a->wl.n);
	return xtk_fmt(ui, "第 %d 轮 · 只默上一轮错的 %d 个词。", s->round, s->qlen);
}

/* 词库为空 */
static XtkEl *view_empty(XtkUi *ui, const char *title)
{
	return fill_page(ui, "empty", xtk_kids(
	    page_header(ui, title, NULL, NULL),
	    xtk_infobar(ui, "词库还是空的", "先去词库录入单词，再回来默写。", (XtkInfoBarDesc){0}),
	    xtk_row(ui, (XtkStackDesc){0},
	        xtk_kids(xtk_button(ui, "去词库添加", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Add", .click = xtk_msg(MSG_GO_LIBRARY)})))));
}

/* 普通模式一轮结束：错词预览 */
static XtkEl *view_preview(XtkUi *ui, const App *a, const Drill *d)
{
	const Session *s = &d->s;
	static ListEnv env;
	env = (ListEnv){a, s->wrong};
	return fill_page(ui, "preview", xtk_kids(
	    page_header(ui, xtk_fmt(ui, "第 %d 轮结束", s->round), "把错的词过一遍，再开始下一轮。", NULL),
	    xtk_infobar(ui, xtk_fmt(ui, "这一轮错了 %d 个词", s->wlen), xtk_fmt(ui, "共 %d 个词，答对 %d 个。", s->qlen, s->qlen - s->wlen),
	                (XtkInfoBarDesc){.severity = XTK_INFOBAR_WARNING}),
	    xtk_grow(xtk_card(ui, (XtkStackDesc){.padding = {8, 8, 8, 8}, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	        xtk_kids(word_list(ui, &env, s->wlen))), 1.0f),
	    xtk_row(ui, (XtkStackDesc){.gap = 8, .justify = XENT_FLEX_JUSTIFY_END, .fill = true},
	        xtk_kids(
	            xtk_button(ui, "从头开始", (XtkButtonDesc){.icon = "Refresh", .click = xtk_msg(MSG_RESTART)}),
	            xtk_button(ui, xtk_fmt(ui, "开始第 %d 轮", s->round + 1),
	                       (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Play", .click = xtk_msg(MSG_START_ROUND)})))));
}

/* 全部完成 */
static XtkEl *view_done(XtkUi *ui, const App *a, const Drill *d, bool endless)
{
	const Session *s = &d->s;
	const char *big = endless ? xtk_fmt(ui, "%d 组全部过关", s->group + 1)
	                          : xtk_fmt(ui, "%d 个词全部默对", s->nwords);
	const char *sub = endless ? xtk_fmt(ui, "无尽模式完成 · 共 %d 个词", s->nwords)
	                          : xtk_fmt(ui, "用了 %d 轮", s->round);
	return fill_page(ui, endless ? "done-endless" : "done", xtk_kids(
	    page_header(ui, endless ? "无尽模式" : "默写", NULL, drill_tools(ui, a, endless)),
	    xtk_grow(xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {40, 32, 40, 32}, .align = XENT_FLEX_ALIGN_CENTER,
	                                        .justify = XENT_FLEX_JUSTIFY_CENTER, .fill = true},
	        xtk_kids(
	            xtk_button(ui, NULL, (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .icon = "Flag", .disabled = true}),
	            xtk_text(ui, big, (XtkTextDesc){.size = 34, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER}),
	            body(ui, sub),
	            xtk_button(ui, endless ? "再来一遍" : "再来一轮",
	                       (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Refresh", .click = xtk_msg(MSG_RESTART)}))), 1.0f)));
}

/* 一词多义的进度：答出的打勾，没答出的留空（公布答案后标出漏掉的） */
static XtkEl *view_senses(XtkUi *ui, const App *a, const Drill *d, int got, bool revealed)
{
	char   line[DICT_MAX_SENSES * (DICT_SENSE_LEN + 16)];
	size_t j = 0;
	for (int k = 0; k < d->nsense && j < sizeof line; k++) {
		const char *gap = k ? "　　" : "";
		if (d->got & (1u << k)) j += (size_t)snprintf(line + j, sizeof line - j, "%s✓ %s", gap, d->senses[k]);
		else if (revealed)      j += (size_t)snprintf(line + j, sizeof line - j, "%s✗ %s", gap, d->senses[k]);
		else                    j += (size_t)snprintf(line + j, sizeof line - j, "%s＿＿＿", gap);
	}
	return xtk_column(ui, (XtkStackDesc){.gap = 6, .align = XENT_FLEX_ALIGN_CENTER},
	    xtk_kids(
	        caption(ui, xtk_fmt(ui, "共 %d 个意思 · 已答出 %d 个", d->nsense, got)),
	        xtk_text(ui, xtk_fmt(ui, "%s", line), (XtkTextDesc){.size = 18, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER, .color = accent(a)})));
}

static XtkEl *view_drill(XtkUi *ui, App *a, Drill *d, bool endless)
{
	const Session *s     = &d->s;
	const char    *title = endless ? "无尽模式" : "默写";
	if (s->phase == SP_EMPTY)   return view_empty(ui, title);
	if (s->phase == SP_PREVIEW) return view_preview(ui, a, d);
	if (s->phase == SP_DONE)    return view_done(ui, a, d, endless);

	const Word *w      = &a->wl.words[sess_current(s)];
	bool        asking = (s->phase == SP_ASKING || s->phase == SP_WRONG);
	bool        busy   = a->judging;

	/* —— 进度 —— */
	const char *where = endless
	    ? xtk_fmt(ui, "第 %d / %d 组%s", s->group + 1, sess_ngroups(s), s->restarts ? xtk_fmt(ui, " · 本组已重开 %d 次", s->restarts) : "")
	    : (s->round > 1 ? xtk_fmt(ui, "第 %d 轮 · 错词复习", s->round) : "第 1 轮");
	XtkEl *progress = xtk_column(ui, (XtkStackDesc){.gap = 8, .align = XENT_FLEX_ALIGN_STRETCH},
	    xtk_kids(
	        xtk_row(ui, (XtkStackDesc){.justify = XENT_FLEX_JUSTIFY_SPACE_BETWEEN, .fill = true},
	            xtk_kids(
	                xtk_text(ui, xtk_fmt(ui, "%d / %d", s->pos + 1, s->qlen), (XtkTextDesc){.size = 13, .weight = XTK_FONT_SEMI_BOLD}),
	                caption(ui, where))),
	        xtk_progress(ui, (XtkProgressDesc){.value = (float)s->pos, .max = (float)s->qlen, .indeterminate = busy})));

	/* —— 题面 —— */
	XtkEl *prompt, *hint = NULL;
	if (a->dir == DIR_EN2CN) {
		prompt = xtk_text(ui, w->word, (XtkTextDesc){.size = 52, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER});
		int got = drill_senses(a, d);
		if (d->nsense > 1 && a->all_senses) hint = view_senses(ui, a, d, got, s->phase == SP_REVEALED);
	} else {
		char masked[256];
		masked_display(w->word, masked, sizeof masked);
		prompt = xtk_text(ui, w->meaning, (XtkTextDesc){.size = 38, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER});
		hint   = xtk_text(ui, xtk_fmt(ui, "%s", masked), (XtkTextDesc){.size = 24, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER, .color = accent(a)});
	}

	/* —— 输入 —— */
	XtkEl *input = xtk_row(ui, (XtkStackDesc){.gap = 8, .align = XENT_FLEX_ALIGN_STRETCH, .justify = XENT_FLEX_JUSTIFY_CENTER},
	    xtk_kids(
	        xtk_sized(xtk_textbox(ui, (a->dir == DIR_EN2CN) ? "写出中文释义，回车提交" : "写出英文单词，回车提交",
	            (XtkTextBoxDesc){.content = a->answer, .change = xtk_msg(MSG_ANSWER), .submit = xtk_msg(MSG_ENTER),
	                             .disabled = !asking || busy}), 380, 36),
	        asking ? xtk_button(ui, "提交", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Accept", .disabled = busy,
	                                                     .click = xtk_msg(MSG_SUBMIT)}) : NULL));

	/* —— 操作：随阶段变化 —— */
	XtkEl *act1 = NULL, *act2 = NULL;
	switch (s->phase) {
	case SP_ASKING:
		act1 = xtk_button(ui, "不知道", (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .icon = "Help", .disabled = busy, .click = xtk_msg(MSG_DONT)});
		break;
	case SP_WRONG:
		act1 = xtk_button(ui, "不知道", (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .icon = "Help", .disabled = busy, .click = xtk_msg(MSG_DONT)});
		act2 = endless ? NULL : xtk_button(ui, "跳过", (XtkButtonDesc){.icon = "Next", .disabled = busy, .click = xtk_msg(MSG_NEXT)});
		break;
	case SP_CORRECT:
		act1 = xtk_button(ui, "下一个", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Next", .click = xtk_msg(MSG_NEXT)});
		break;
	case SP_REVEALED:
		/* 无尽模式错了：原「下一个」的位置换成「重新开始」，答案留在屏幕上 */
		act1 = endless ? xtk_button(ui, "重新开始本组", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Refresh", .click = xtk_msg(MSG_NEXT)})
		               : xtk_button(ui, "下一个", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Next", .click = xtk_msg(MSG_NEXT)});
		break;
	default:
		break;
	}

	XtkEl *card = xtk_grow(xtk_card(ui, (XtkStackDesc){.gap = 18, .padding = {40, 28, 40, 28}, .align = XENT_FLEX_ALIGN_STRETCH,
	                                                   .justify = XENT_FLEX_JUSTIFY_CENTER, .fill = true},
	    xtk_kids(
	        xtk_text(ui, w->pos, (XtkTextDesc){.size = 13, .align = XTK_TEXT_CENTER, .color = COL_SECONDARY}),
	        prompt,
	        hint,
	        xtk_margin(input, (XentInsets){0, 8, 0, 0}),
	        feedback_bar(ui, &d->fb, (XtkMsg){0}),
	        xtk_row(ui, (XtkStackDesc){.gap = 8, .justify = XENT_FLEX_JUSTIFY_CENTER}, xtk_kids(act1, act2)))), 1.0f);

	/* —— 统计 —— */
	XtkEl *stats = endless
	    ? xtk_row(ui, (XtkStackDesc){.gap = 12, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	        xtk_kids(
	            stat_tile(ui, "当前组", xtk_fmt(ui, "%d / %d", s->group + 1, sess_ngroups(s))),
	            stat_tile(ui, "本组已见过", xtk_fmt(ui, "%d / %d", s->seen > s->pos + 1 ? s->seen : s->pos + 1, s->qlen)),
	            stat_tile(ui, "已过关的词", xtk_fmt(ui, "%d", s->gstart))))
	    : xtk_row(ui, (XtkStackDesc){.gap = 12, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	        xtk_kids(
	            stat_tile(ui, "已作答", xtk_fmt(ui, "%d", d->answered)),
	            stat_tile(ui, "一次答对", xtk_fmt(ui, "%d", d->correct)),
	            stat_tile(ui, "本轮错词", xtk_fmt(ui, "%d", s->wlen))));

	return fill_page(ui, endless ? "endless" : "practice", xtk_kids(
	    page_header(ui, title, drill_blurb(ui, a, s, endless), drill_tools(ui, a, endless)),
	    progress,
	    card,
	    stats));
}

/* ================================================================== */
/* 词库                                                                */
/* ================================================================== */

static XtkEl *view_library(XtkUi *ui, App *a)
{
	static ListEnv env;
	env = (ListEnv){a, NULL};
	return fill_page(ui, "library", xtk_kids(
	    page_header(ui, "词库", xtk_fmt(ui, "共 %d 个词 · %s", a->wl.n, a->wl.path[0] ? a->wl.path : "dictation_extra.json"),
	        xtk_button(ui, "清空词库", (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .icon = "Delete", .disabled = a->wl.n == 0,
	                                               .click = xtk_msg(MSG_CLEAR_ASK)})),
	    feedback_bar(ui, &a->add_fb, xtk_msg(MSG_ADD_MSG_CLOSE)),
	    section(ui, "添加单词",
	        "每行一条「单词 词性 释义」，单词可以是短语（如 account for v. 解释、导致；也可以用 Tab 或 = 隔开）；"
	        "或者英文、中文一行一行交替；或者先写全部英文、再写对应的中文，两段数量一致。已在词库里的词自动跳过。",
	        xtk_row(ui, (XtkStackDesc){.gap = 8},
	            xtk_kids(
	                xtk_button(ui, "打开输入窗口", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .icon = "Add", .click = xtk_msg(MSG_ADD_DIALOG)}),
	                xtk_button(ui, "从剪贴板导入", (XtkButtonDesc){.icon = "Paste", .click = xtk_msg(MSG_ADD_CLIP)}),
	                judge_merge_threshold(a->judge) > 0
	                    ? xtk_tooltip(ui,
	                          xtk_button(ui, a->merging ? "正在检查…" : "合并同义释义",
	                                     (XtkButtonDesc){.icon = "Link", .disabled = a->merging || a->wl.n == 0, .click = xtk_msg(MSG_MERGE_ALL)}),
	                          "用智能判分检查整个词库：一词多义里意思相同的释义合并成一项")
	                    : NULL))),
	    xtk_grow(xtk_card(ui, (XtkStackDesc){.gap = 8, .padding = {20, 18, 20, 12}, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	        xtk_kids(
	            subtitle(ui, "全部单词"),
	            a->wl.n ? word_list(ui, &env, a->wl.n) : body(ui, "还没有词。"))), 1.0f),
	    xtk_dialog(ui, "清空词库？",
	        (XtkDialogDesc){.open = a->clear_open, .primary = "清空", .close = "取消", .w = 420, .contenth = 64,
	                        .result = xtk_msg(MSG_CLEAR_RESULT)},
	        xtk_kids(xtk_text(ui, xtk_fmt(ui, "会删掉全部 %d 个词，并删除词库文件。这一步不能撤销。", a->wl.n), (XtkTextDesc){.size = 14})))));
}

/* ================================================================== */
/* 说明                                                                */
/* ================================================================== */

static XtkEl *para(XtkUi *ui, const char *t) { return xtk_text(ui, t, (XtkTextDesc){.size = 14}); }

static XtkEl *view_help(XtkUi *ui)
{
	return scroll_page(ui, PAGE_HELP, xtk_column(ui, (XtkStackDesc){.gap = 16, .align = XENT_FLEX_ALIGN_STRETCH},
	    xtk_kids(
	        page_header(ui, "说明", "两种练习各自保留进度，在左侧随时切换；关掉程序再打开会接着上次的进度。", NULL),
	        section(ui, "默写", NULL, para(ui,
	            "第 1 轮按词表顺序出全部词。答错可以再试；点「不知道」或「跳过」会公布答案，这个词记为错词。\n"
	            "一轮结束后先看错词清单，下一轮只默错的词，直到全部答对。")),
	        section(ui, "无尽模式", NULL, para(ui,
	            "词表按顺序每 20 个分一组，组内按原顺序出题。\n"
	            "提交答错可以继续重试；点「不知道」才公布答案并触发本组重开：本组见过的词（之前答对的和这次错的）打乱顺序排在前面，没见过的词保持原顺序跟在后面，从头再答。\n"
	            "整组一次不错地答完才进入下一组，上一组的词不再出现；所有组都过关才算完成。")),
	        section(ui, "题型与判分", NULL, para(ui,
	            "释义 → 单词：看中文写英文，下方蓝色字母是开头提示，不分大小写。\n"
	            "单词 → 释义：看英文写中文。有多个意思的词（释义里用 、 ; 或空格隔开）要把每个意思都写出来："
	            "一次写一个，也可以一次写几个用 、 隔开；答出的会打勾，全部答出才算对，括号里的注释不用写。"
	            "右上角取消勾选「多义词全答」后，写出任意一个意思就算对（开关会记住）。"
	            "录入新词时，一词多义里意思相同的释义（如「缓解、减轻」）会用智能判分自动合并成「缓解（减轻）」，写其中任何一个都算答对这一项；"
	            "已有的词库可以在词库页点「合并同义释义」。"
	            "字面判不出时，会请本地小模型判断意思是否接近。\n"
	            "短语按整条比对：不分大小写，多余的空格和 sth. 的点号不计较。")),
	        section(ui, "快捷键", NULL, para(ui, "输入框里按回车 = 提交；答完后按回车 = 下一个（无尽模式里是重新开始本组）。")))));
}

/* ================================================================== */
/* 根视图                                                              */
/* ================================================================== */

static const XtkNavItemDesc NAV_ITEMS[PAGE_COUNT] = {
	[PAGE_PRACTICE] = {XTK_NAV_ITEM, "Edit", "默写"},
	[PAGE_ENDLESS]  = {XTK_NAV_ITEM, "Sync", "无尽模式"},
	[PAGE_SEP]      = {XTK_NAV_SEPARATOR, NULL, NULL},
	[PAGE_LIBRARY]  = {XTK_NAV_ITEM, "List", "词库"},
	[PAGE_HELP]     = {XTK_NAV_FOOTER, "Help", "说明"},
};

XtkEl *app_page(XtkUi *ui, void *model)
{
	App *a = (App *)model;
	switch (a->page) {
	case PAGE_ENDLESS: return view_drill(ui, a, &a->drill[1], true);
	case PAGE_LIBRARY: return view_library(ui, a);
	case PAGE_HELP:    return view_help(ui);
	default:           return view_drill(ui, a, &a->drill[0], false);
	}
}

XtkEl *app_view(XtkUi *ui, void *model)
{
	App   *a    = (App *)model;
	XtkEl *page = app_page(ui, model);
	return xtk_grow(xtk_nav(ui,
	    (XtkNavDesc){.items = NAV_ITEMS, .nitem = PAGE_COUNT, .sel = (int)a->page, .window_title_bar = true,
	                 .app_title = "单词默写", .select = xtk_msg(MSG_NAV)},
	    xtk_kids(page)), 1.0f);
}
