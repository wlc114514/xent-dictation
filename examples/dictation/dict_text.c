/**
 * @file dict_text.c
 * @brief 后端 · 文本工具实现（规则与 Python 版 dictation.py 逐条对应）。
 */
#include "dict_text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ================================================================== */
/* UTF-8                                                               */
/* ================================================================== */

int utf8_decode(const unsigned char *s, uint32_t *cp)
{
	unsigned char c = s[0];
	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
		return 2;
	}
	if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		return 3;
	}
	if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
		    | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		return 4;
	}
	*cp = 0xFFFD;
	return 1;
}

static bool is_cjk_cp(uint32_t cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

int utf8_cjk(const char *s, uint32_t *out, int max)
{
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if (is_cjk_cp(cp) && n < max) out[n++] = cp;
	}
	return n;
}

bool has_cjk(const char *s)
{
	uint32_t dummy;
	return utf8_cjk(s, &dummy, 1) > 0;
}

void lower_copy(const char *src, char *dst, size_t cap)
{
	size_t j = 0;
	for (const char *p = src; *p && j + 1 < cap; p++) {
		char c = *p;
		dst[j++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	dst[j] = 0;
}

bool str_ci_eq(const char *a, const char *b)
{
	for (;; a++, b++) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb) return false;
		if (!ca) return true;
	}
}

bool is_blank(const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return false;
		if (is_cjk_cp(cp)) return false;
	}
	return true;
}

/* ================================================================== */
/* 判分                                                                */
/* ================================================================== */

static bool is_sep(uint32_t cp)
{
	return cp == ',' || cp == ';' || cp == '/' || cp == 0x3001 /* 、 */
	    || cp == 0xFF0C /* ， */ || cp == 0xFF1B /* ； */;
}

static bool cp_contains(const uint32_t *hay, int nh, const uint32_t *needle, int nn)
{
	if (nn == 0 || nh < nn) return false;
	for (int i = 0; i + nn <= nh; i++) {
		int j = 0;
		while (j < nn && hay[i + j] == needle[j]) j++;
		if (j == nn) return true;
	}
	return false;
}

static bool cp_subset(const uint32_t *m, int nm, const uint32_t *a, int na)
{
	for (int i = 0; i < na; i++) {
		bool hit = false;
		for (int j = 0; j < nm; j++) if (m[j] == a[i]) { hit = true; break; }
		if (!hit) return false;
	}
	return true;
}

bool meaning_match(const char *ans, const char *meaning)
{
	uint32_t a[512], m[1024];
	int na = utf8_cjk(ans, a, 512);
	int nm = utf8_cjk(meaning, m, 1024);
	if (na == 0) return false;
	if (nm == 0) {
		char la[512], lm[1024];
		lower_copy(ans, la, sizeof la);
		lower_copy(meaning, lm, sizeof lm);
		return la[0] && (strcmp(la, lm) == 0 || strstr(lm, la) != NULL);
	}
	if (na == nm && memcmp(a, m, (size_t)na * 4) == 0) return true;
	if (na >= 2 && cp_contains(m, nm, a, na)) return true;

	/* 按 ; ，、/ 拆义项 */
	uint32_t senses[32][64];
	int      lens[32];
	int      ns = 0, cur = 0;
	for (const unsigned char *p = (const unsigned char *)meaning; *p && ns < 32;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if (is_sep(cp)) {
			if (cur > 0) { lens[ns++] = cur; cur = 0; }
		} else if (is_cjk_cp(cp)) {
			if (cur < 64) senses[ns][cur++] = cp;
		}
	}
	if (cur > 0 && ns < 32) lens[ns++] = cur;

	for (int i = 0; i < ns; i++) {
		if (na == lens[i] && memcmp(a, senses[i], (size_t)na * 4) == 0) return true;
		if (lens[i] >= 2 && cp_contains(a, na, senses[i], lens[i])) return true;
		if (na >= 2 && cp_contains(senses[i], lens[i], a, na)) return true;
	}
	if (na >= 2 && cp_subset(m, nm, a, na)) {
		int shortest = nm;
		for (int i = 0; i < ns; i++) if (lens[i] < shortest) shortest = lens[i];
		if (na * 2 >= shortest) return true;
	}
	return false;
}


/* ================================================================== */
/* 一词多义：拆义项、逐条判                                            */
/* ================================================================== */

static bool is_open_cp(uint32_t cp)  { return cp == '(' || cp == '[' || cp == 0xFF08 /* （ */ || cp == 0x3010 /* 【 */; }
static bool is_close_cp(uint32_t cp) { return cp == ')' || cp == ']' || cp == 0xFF09 /* ） */ || cp == 0x3011 /* 】 */; }

/* 把 [p, e) 去掉首尾空白后作为一个义项收下（重复的不收） */
static void sense_put(const char *p, const char *e, char out[][DICT_SENSE_LEN], int *n, int max)
{
	while (p < e && (*p == ' ' || *p == '\t')) p++;
	while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
	if (p == e || *n >= max) return;
	size_t len = (size_t)(e - p);
	if (len >= DICT_SENSE_LEN) len = DICT_SENSE_LEN - 1;
	char s[DICT_SENSE_LEN];
	memcpy(s, p, len);
	s[len] = 0;
	if (is_blank(s)) return;
	for (int i = 0; i < *n; i++) if (strcmp(out[i], s) == 0) return;
	memcpy(out[(*n)++], s, len + 1);
}

int split_senses(const char *meaning, char out[][DICT_SENSE_LEN], int max)
{
	int         n     = 0, depth = 0;
	uint32_t    prev  = 0;   /* 上一个非空白字符 */
	const char *start = meaning;
	for (const char *p = meaning; *p;) {
		uint32_t cp;
		int      len = utf8_decode((const unsigned char *)p, &cp);
		if (is_open_cp(cp)) depth++;
		else if (is_close_cp(cp) && depth > 0) depth--;

		bool split = false;
		if (depth == 0 && is_sep(cp)) split = true;
		if (depth == 0 && (cp == ' ' || cp == '\t') && is_cjk_cp(prev)) {
			/* 空格只在两个汉字之间才算分隔：「复杂的 复合体」拆，「使 (决定) 转向」不拆 */
			const char *q = p;
			while (*q == ' ' || *q == '\t') q++;
			uint32_t next = 0;
			if (*q) utf8_decode((const unsigned char *)q, &next);
			split = is_cjk_cp(next);
		}
		if (split) {
			sense_put(start, p, out, &n, max);
			start = p + len;
		}
		if (cp != ' ' && cp != '\t') prev = split ? 0 : cp;
		p += len;
	}
	sense_put(start, start + strlen(start), out, &n, max);
	return n;
}

int senses_mark(const char *ans, char senses[][DICT_SENSE_LEN], int ns, uint32_t *got)
{
	char parts[DICT_MAX_SENSES][DICT_SENSE_LEN];
	int  np    = split_senses(ans, parts, DICT_MAX_SENSES);
	int  fresh = 0;
	for (int i = 0; i < np; i++) {
		for (int k = 0; k < ns; k++) {
			if (*got & (1u << k) || !meaning_match(parts[i], senses[k])) continue;
			*got |= 1u << k;
			fresh++;
			break;
		}
	}
	return fresh;
}

bool senses_merge(char senses[][DICT_SENSE_LEN], int ns, const uint32_t *link, char *out, size_t cap)
{
	int root[DICT_MAX_SENSES];
	for (int i = 0; i < ns; i++) root[i] = i;
	bool any = false;
	for (int i = 0; i < ns; i++) {
		for (int j = i + 1; j < ns; j++) {
			if (!(link[i] & (1u << j)) && !(link[j] & (1u << i))) continue;
			int a = i, b = j;
			while (root[a] != a) a = root[a];
			while (root[b] != b) b = root[b];
			if (a == b) continue;
			if (a < b) root[b] = a; else root[a] = b;   /* 组以最靠前的一条为首 */
			any = true;
		}
	}
	if (!any) return false;

	size_t j = 0;
	out[0]   = 0;
	for (int i = 0; i < ns && j < cap; i++) {
		int r = i;
		while (root[r] != r) r = root[r];
		if (r != i) continue;   /* 不是组首：跟着组首写 */
		j += (size_t)snprintf(out + j, cap - j, "%s%s", j ? "、" : "", senses[i]);
		int more = 0;
		for (int k = i + 1; k < ns && j < cap; k++) {
			int rk = k;
			while (root[rk] != rk) rk = root[rk];
			if (rk != i) continue;
			j += (size_t)snprintf(out + j, cap - j, "%s%s", more++ ? "、" : "（", senses[k]);
		}
		if (more && j < cap) j += (size_t)snprintf(out + j, cap - j, "）");
	}
	return true;
}

/* 规整成比对用的形式：小写、去掉 '.'、连续空白压成一个空格、去首尾 */
static void answer_norm(const char *src, char *dst, size_t cap)
{
	size_t j  = 0;
	bool   sp = false;
	for (const char *p = src; *p && j + 2 < cap; p++) {
		char c = *p;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (j > 0) sp = true; continue; }
		if (c == '.') continue;
		if (sp) { dst[j++] = ' '; sp = false; }
		dst[j++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	dst[j] = 0;
}

bool answer_eq(const char *ans, const char *word)
{
	char a[256], w[256];
	answer_norm(ans, a, sizeof a);
	answer_norm(word, w, sizeof w);
	return a[0] && strcmp(a, w) == 0;
}

void masked_display(const char *word, char *out, size_t cap)
{
	size_t j = 0;
	for (const char *p = word; *p;) {
		while (*p == ' ') p++;
		if (!*p) break;
		const char *e = p;
		while (*e && *e != ' ') e++;
		if (j > 0 && j + 4 < cap) { memcpy(out + j, "   ", 3); j += 3; }   /* 短语：词与词之间留宽空 */

		/* 每个词各自露出前半段；字母以外的字符（- ' /）原样显示 */
		size_t n    = (size_t)(e - p);
		size_t hint = (n + 1) / 2;
		if (hint > 0) hint -= 1;
		for (size_t i = 0; i < n; i++) {
			if (j + 3 >= cap) break;
			if (i) out[j++] = ' ';
			bool letter = isalnum((unsigned char)p[i]) || (unsigned char)p[i] >= 0x80;
			out[j++] = (i < hint || !letter) ? p[i] : '_';
		}
		p = e;
	}
	out[j] = 0;
}

/* ================================================================== */
/* JSON                                                                */
/* ================================================================== */

const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	return p;
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool read_hex4(const char *p, unsigned *out)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexv(p[i]);
		if (h < 0) return false;
		v = v * 16 + (unsigned)h;
	}
	*out = v;
	return true;
}

static size_t put_utf8(char *out, size_t j, size_t cap, unsigned cp)
{
	if (cp < 0x80) {
		if (j + 1 < cap) out[j++] = (char)cp;
	} else if (cp < 0x800) {
		if (j + 2 < cap) {
			out[j++] = (char)(0xC0 | (cp >> 6));
			out[j++] = (char)(0x80 | (cp & 0x3F));
		}
	} else if (cp < 0x10000) {
		if (j + 3 < cap) {
			out[j++] = (char)(0xE0 | (cp >> 12));
			out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[j++] = (char)(0x80 | (cp & 0x3F));
		}
	} else if (j + 4 < cap) {
		out[j++] = (char)(0xF0 | (cp >> 18));
		out[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[j++] = (char)(0x80 | (cp & 0x3F));
	}
	return j;
}

const char *js_str(const char *p, char *out, size_t cap)
{
	if (*p != '"') return NULL;
	p++;
	size_t j = 0;
	while (*p && *p != '"') {
		if (*p != '\\') {
			if (j + 1 < cap) out[j++] = *p;
			p++;
			continue;
		}
		p++;
		char e = *p;
		if (!e) break;
		p++;
		switch (e) {
		case 'n': if (j + 1 < cap) out[j++] = '\n'; break;
		case 't': if (j + 1 < cap) out[j++] = '\t'; break;
		case 'r': if (j + 1 < cap) out[j++] = '\r'; break;
		case 'b': if (j + 1 < cap) out[j++] = '\b'; break;
		case 'f': if (j + 1 < cap) out[j++] = '\f'; break;
		case 'u': {
			unsigned cp;
			if (!read_hex4(p, &cp)) { cp = 0xFFFD; break; }
			p += 4;
			unsigned lo;
			if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u' &&
			    read_hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				p += 6;
			}
			j = put_utf8(out, j, cap, cp);
			break;
		}
		default: /* \" \\ \/ 以及未知转义：原样取字符 */
			if (j + 1 < cap) out[j++] = e;
			break;
		}
	}
	if (*p == '"') p++;
	out[j] = 0;
	return p;
}

void json_escape(const char *src, char *dst, size_t cap)
{
	size_t j = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && j + 7 < cap; p++) {
		unsigned char c = *p;
		switch (c) {
		case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
		case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
		case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
		case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
		case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
		default:
			if (c < 0x20) j += (size_t)snprintf(dst + j, cap - j, "\\u%04x", c);
			else          dst[j++] = (char)c;
		}
	}
	dst[j] = 0;
}

const char *json_find_key(const char *buf, const char *key)
{
	char pat[64];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *p = strstr(buf, pat);
	if (!p) return NULL;
	p = skip_ws(p + strlen(pat));
	if (*p != ':') return NULL;
	return skip_ws(p + 1);
}

#ifdef _WIN32
static bool to_wide(const char *s, wchar_t *out, int cap)
{
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}
#endif

FILE *u8_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
	wchar_t wp[1024], wm[16];
	if (to_wide(path, wp, 1024) && to_wide(mode, wm, 16)) return _wfopen(wp, wm);
#endif
	return fopen(path, mode);
}

int u8_remove(const char *path)
{
#ifdef _WIN32
	wchar_t wp[1024];
	if (to_wide(path, wp, 1024)) return _wremove(wp);
#endif
	return remove(path);
}

char *read_file(const char *path, long *out_sz)
{
	FILE *f = u8_fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 16 * 1024 * 1024) { fclose(f); return NULL; }
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	buf[rd] = 0;
	fclose(f);
	if (out_sz) *out_sz = (long)rd;
	return buf;
}
