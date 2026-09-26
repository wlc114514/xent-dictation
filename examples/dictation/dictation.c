/**
 * @file dictation.c
 * @brief 「单词默写」— Project-Xent (fluxent) 前端。
 *
 * 与 Python 版 dictation.py 共用数据文件（都用相对当前工作目录的路径）：
 *   · dictation_extra.json  —— 词表 [["word","pos","meaning"], ...]
 *   · dictation_judge.json  —— 判分配置（enabled/port/threshold/exe/model/timeout_s）
 *
 * 判分：本地 llama-server（Jev-Style-0.8B-Decision-v3）的 /completion 接口，
 * 取 " yes"(id 9542) / " no"(id 874) 两个 token 的 logprob 差，
 * 按校准温度换算成「意思接近」的概率（与 Python 版完全同算法）。
 *
 * 两种模式：cn2en = 看释义写单词（默认）；en2cn = 看单词写释义（宽松判分 + JEV 兜底）。
 */
#include <fluxent/fluxent.h>
#include <fluxent/flux_app.h>

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DICT_MAX_WORDS   2000
#define DICT_MAX_WORD    96
#define DICT_MAX_POS     96
#define DICT_MAX_MEANING 640
#define DICT_MAX_ANSWER  512
#define JEV_TEMPERATURE  0.8800546821789332
#define JEV_TOKEN_YES    9542
#define JEV_TOKEN_NO     874

/* ------------------------------------------------------------------ */
/* 消息标签（0 保留给「无消息」）                                        */
/* ------------------------------------------------------------------ */
enum {
	MSG_SUBMIT = 1,
	MSG_DONT,
	MSG_NEXT,
	MSG_RESTART,
	MSG_MODE,
	MSG_ANSWER,
	MSG_START_ROUND,
};

typedef enum Phase {
	PH_NORMAL = 0,
	PH_WRONG,
	PH_CORRECT,
	PH_REVEALED,
	PH_DONE,
	PH_PREVIEW,
} Phase;

typedef struct Word {
	char word[DICT_MAX_WORD];
	char pos[DICT_MAX_POS];
	char meaning[DICT_MAX_MEANING];
} Word;

typedef struct JudgeCfg {
	bool   enabled;
	char   exe[MAX_PATH];    /* llama-server.exe（可相对路径） */
	char   model[MAX_PATH];  /* GGUF（可相对路径） */
	char   host[64];
	int    port;
	double threshold;
	int    timeout_ms;
} JudgeCfg;

typedef struct Dict {
	Word words[DICT_MAX_WORDS];
	int  nwords;

	int  queue[DICT_MAX_WORDS];        /* 本轮词序（words 下标） */
	int  qlen;
	int  round_words[DICT_MAX_WORDS];  /* 本轮起始词序（重开恢复） */
	int  rwlen;
	int  wrong[DICT_MAX_WORDS];        /* 本轮错词 */
	int  wlen;
	int  preview[DICT_MAX_WORDS];      /* 错词预览清单 */
	int  preview_n;

	int    round;          /* 1 = 第一轮 */
	int    pos;            /* 已出题数（1-based 指向当前词） */
	Phase  phase;
	int    mode;           /* 0 = cn2en，1 = en2cn */

	char     answer[DICT_MAX_ANSWER];  /* 输入框当前文本（UTF-8） */
	char     result[800];              /* 结果行文本 */
	XtkColor result_color;
	bool     result_set;
	char     masked[256];              /* 首字母提示（cn2en） */

	JudgeCfg    jcfg;
	volatile LONG server_ready;
	volatile LONG server_started;
	HANDLE        server_proc;

	/* 判分缓存（最近 8 条） */
	struct {
		char   meaning[DICT_MAX_MEANING];
		char   ans[DICT_MAX_ANSWER];
		double p;
	} cache[8];
	int cache_n;

	/* 数据文件解析出来的根目录（用于相对路径回退） */
	char root[MAX_PATH];
} Dict;

static Dict g_app;

/* ================================================================== */
/* UTF-8 / 中文工具                                                    */
/* ================================================================== */

static int utf8_decode(const unsigned char *s, uint32_t *cp)
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

/* 收集汉字（与 Python 版一致：只认 U+4E00–U+9FFF） */
static int utf8_cjk(const char *s, uint32_t *out, int max)
{
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		uint32_t cp;
		int len = utf8_decode(p, &cp);
		if (len <= 0) break;
		p += len;
		if (cp >= 0x4E00 && cp <= 0x9FFF && n < max) out[n++] = cp;
	}
	return n;
}

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

static void lower_copy(const char *src, char *dst, size_t cap)
{
	size_t j = 0;
	for (const char *p = src; *p && j + 1 < cap; p++) {
		char c = *p;
		dst[j++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	dst[j] = 0;
}

/* 与 Python 版 meaning_match() 同逻辑的宽松判分 */
static bool meaning_match(const char *ans, const char *meaning)
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
		int len = utf8_decode(p, &cp);
		if (len <= 0) break;
		p += len;
		if (is_sep(cp)) {
			if (cur > 0) { lens[ns] = cur; ns++; cur = 0; }
		} else if (cp >= 0x4E00 && cp <= 0x9FFF) {
			if (cur < 64) senses[ns][cur++] = cp;
		}
	}
	if (cur > 0 && ns < 32) { lens[ns] = cur; ns++; }

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

/* 空白/纯标点：没有 0-9/a-z/汉字 就算空白（不给模型「撞运气」机会） */
static bool is_blank(const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		uint32_t cp;
		int len = utf8_decode(p, &cp);
		if (len <= 0) break;
		p += len;
		if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return false;
		if (cp >= 0x4E00 && cp <= 0x9FFF) return false;
	}
	return true;
}

static bool str_ci_eq(const char *a, const char *b)
{
	char la[512], lb[512];
	lower_copy(a, la, sizeof la);
	lower_copy(b, lb, sizeof lb);
	return strcmp(la, lb) == 0;
}

/* 开头提示字母原样显示、其余挖空（与 Python masked_display 一致） */
static void masked_display(const char *word, char *out, size_t cap)
{
	size_t n = strlen(word);
	size_t hint = (n + 1) / 2;
	unsigned j = 0;
	if (hint > 0) hint -= 1;
	for (size_t i = 0; i < n; i++) {
		if (j + 3 >= cap) break;
		if (i) out[j++] = ' ';
		out[j++] = (i < hint) ? word[i] : '_';
	}
	out[j] = 0;
}

/* ================================================================== */
/* JSON：读文件 / 字符串解析（含 \uXXXX） / 判分配置键扫描                  */
/* ================================================================== */

static const char *skip_ws(const char *p)
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

/* 解析一个 JSON 字符串（*p 应为 '"'），输出 UTF-8 到 out */
static const char *js_str(const char *p, char *out, size_t cap)
{
	if (*p != '"') return NULL;
	p++;
	size_t j = 0;
	while (*p && *p != '"') {
		unsigned char c = (unsigned char)*p;
		if (c == '\\') {
			p++;
			char e = *p;
			switch (e) {
			case 'n': c = '\n'; p++; break;
			case 't': c = '\t'; p++; break;
			case 'r': c = '\r'; p++; break;
			case 'b': c = '\b'; p++; break;
			case 'f': c = '\f'; p++; break;
			case '"': c = '"';  p++; break;
			case '\\': c = '\\'; p++; break;
			case '/': c = '/';  p++; break;
			case 'u': {
				unsigned cp = 0;
				for (int i = 0; i < 4; i++) {
					int h = hexv(p[1 + i]);
					if (h < 0) { cp = 0xFFFD; break; }
					cp = cp * 16 + (unsigned)h;
				}
				p += 5;
				if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
					unsigned lo = 0;
					bool oklo = true;
					for (int i = 0; i < 4; i++) {
						int h = hexv(p[2 + i]);
						if (h < 0) { oklo = false; break; }
						lo = lo * 16 + (unsigned)h;
					}
					if (oklo && lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					}
				}
				/* 编码为 UTF-8 */
				if (cp < 0x80) {
					if (j + 1 < cap) out[j++] = (char)cp;
				} else if (cp < 0x800) {
					if (j + 2 < cap) { out[j++] = (char)(0xC0 | (cp >> 6)); out[j++] = (char)(0x80 | (cp & 0x3F)); }
				} else if (cp < 0x10000) {
					if (j + 3 < cap) {
						out[j++] = (char)(0xE0 | (cp >> 12));
						out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
						out[j++] = (char)(0x80 | (cp & 0x3F));
					}
				} else {
					if (j + 4 < cap) {
						out[j++] = (char)(0xF0 | (cp >> 18));
						out[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
						out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
						out[j++] = (char)(0x80 | (cp & 0x3F));
					}
				}
				continue;
			}
			default: p++; continue;
			}
			if (j + 1 < cap) out[j++] = (char)c;
		} else {
			if (j + 1 < cap) out[j++] = (char)c;
			p++;
		}
	}
	if (*p == '"') p++;
	out[j] = 0;
	return p;
}

static char *read_file(const char *path, long *out_sz)
{
	FILE *f = fopen(path, "rb");
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

/* 词表：[[word,pos,meaning], ...] */
static bool load_words(const char *path, Dict *d)
{
	char *buf = read_file(path, NULL);
	if (!buf) return false;
	const char *p = skip_ws(buf);
	d->nwords = 0;
	if (*p != '[') { free(buf); return false; }
	p = skip_ws(p + 1);
	while (*p && *p != ']') {
		if (*p == ',') { p = skip_ws(p + 1); continue; }
		if (*p != '[') break;
		p = skip_ws(p + 1);
		char w[DICT_MAX_WORD] = {0}, ps[DICT_MAX_POS] = {0}, mn[DICT_MAX_MEANING] = {0};
		p = js_str(p, w, sizeof w);
		if (!p) break;
		p = skip_ws(p);
		if (*p == ',') p = skip_ws(p + 1);
		p = js_str(p, ps, sizeof ps);
		if (!p) break;
		p = skip_ws(p);
		if (*p == ',') p = skip_ws(p + 1);
		p = js_str(p, mn, sizeof mn);
		if (!p) break;
		/* 跳到本项结尾 */
		p = skip_ws(p);
		while (*p && *p != ']') {
			if (*p == '"') {
				char tmp[16];
				p = js_str(p, tmp, sizeof tmp);
				if (!p) break;
			} else {
				p++;
			}
			p = skip_ws(p);
		}
		if (*p == ']') p = skip_ws(p + 1);
		if (w[0] && mn[0] && d->nwords < DICT_MAX_WORDS) {
			Word *wp = &d->words[d->nwords++];
			snprintf(wp->word, sizeof wp->word, "%s", w);
			snprintf(wp->pos, sizeof wp->pos, "%s", ps);
			snprintf(wp->meaning, sizeof wp->meaning, "%s", mn);
		}
	}
	free(buf);
	return true;
}

/* 判分配置：逐键扫描（enabled/port/threshold/exe/model/host/timeout_s） */
static const char *find_key(const char *buf, const char *key)
{
	char pat[64];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *p = strstr(buf, pat);
	if (!p) return NULL;
	p = skip_ws(p + strlen(pat));
	if (*p != ':') return NULL;
	return skip_ws(p + 1);
}

static void load_judge_cfg(const char *path, JudgeCfg *c)
{
	c->enabled = false;
	snprintf(c->exe, sizeof c->exe, "jev/llama/llama-server.exe");
	snprintf(c->model, sizeof c->model, "jev/Jev-Style-0.8B-Decision-v3-Q4_K_M.gguf");
	snprintf(c->host, sizeof c->host, "127.0.0.1");
	c->port = 8087;
	c->threshold = 0.9;
	c->timeout_ms = 15000;

	char *buf = read_file(path, NULL);
	if (!buf) return;                 /* 没有配置文件 → 判分器关闭 */
	c->enabled = true;

	const char *p;
	if ((p = find_key(buf, "enabled"))) {
		if (strncmp(p, "false", 5) == 0) c->enabled = false;
	}
	if ((p = find_key(buf, "port")))          c->port = (int)strtol(p, NULL, 10);
	if ((p = find_key(buf, "threshold")))     c->threshold = strtod(p, NULL);
	double tsec = 0;
	if ((p = find_key(buf, "timeout_s")))     { tsec = strtod(p, NULL); if (tsec > 0) c->timeout_ms = (int)(tsec * 1000); }
	if ((p = find_key(buf, "exe")))           js_str(p, c->exe, sizeof c->exe);
	if ((p = find_key(buf, "model")))         js_str(p, c->model, sizeof c->model);
	if ((p = find_key(buf, "host")))          js_str(p, c->host, sizeof c->host);
	free(buf);
}

/* ================================================================== */
/* HTTP（WinHTTP）与 JEV 判分                                          */
/* ================================================================== */

static bool http_request(const char *host, int port, const wchar_t *method, const char *path,
                         const char *body, char *resp, size_t cap, size_t *out_len, int timeout_ms)
{
	bool ok = false;
	wchar_t whost[128], wpath[256];
	if (MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 128) <= 0) return false;
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 256) <= 0) return false;

	HINTERNET ses = WinHttpOpen(L"dictation-xent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
	                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) return false;
	WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

	HINTERNET con = WinHttpConnect(ses, whost, (INTERNET_PORT)port, 0);
	if (con) {
		HINTERNET req = WinHttpOpenRequest(con, method, wpath, NULL, WINHTTP_NO_REFERER,
		                                   WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
		if (req) {
			DWORD blen = body ? (DWORD)strlen(body) : 0;
			LPCWSTR hdr = L"Content-Type: application/json\r\n";
			if (WinHttpSendRequest(req, body ? hdr : WINHTTP_NO_ADDITIONAL_HEADERS, -1L,
			                       (LPVOID)body, blen, blen, 0) &&
			    WinHttpReceiveResponse(req, NULL)) {
				size_t used = 0;
				DWORD got = 0;
				while (used + 1 < cap) {
					if (!WinHttpReadData(req, resp + used, (DWORD)(cap - 1 - used), &got) || got == 0) break;
					used += got;
				}
				resp[used] = 0;
				if (out_len) *out_len = used;
				ok = used > 0;
			}
			WinHttpCloseHandle(req);
		}
		WinHttpCloseHandle(con);
	}
	WinHttpCloseHandle(ses);
	return ok;
}

static bool server_health(const Dict *d)
{
	char resp[512];
	size_t n = 0;
	if (!http_request(d->jcfg.host, d->jcfg.port, L"GET", "/health", NULL,
	                  resp, sizeof resp, &n, 1200))
		return false;
	return strstr(resp, "\"ok\"") != NULL;
}

static void spawn_server(Dict *d)
{
	if (InterlockedCompareExchange(&d->server_started, 1, 0) != 0) return;

	wchar_t wexe[MAX_PATH], wmodel[MAX_PATH], cmd[4096];
	MultiByteToWideChar(CP_UTF8, 0, d->jcfg.exe, -1, wexe, MAX_PATH);
	MultiByteToWideChar(CP_UTF8, 0, d->jcfg.model, -1, wmodel, MAX_PATH);
	_snwprintf(cmd, 4096,
	           L"\"%ls\" -m \"%ls\" --host 127.0.0.1 --port %d -c 2048 -t 8 --no-webui",
	           wexe, wmodel, d->jcfg.port);

	SECURITY_ATTRIBUTES sa = {0};
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	HANDLE log = CreateFileW(L"jev\\server.log", FILE_APPEND_DATA,
	                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	STARTUPINFOW si = {0};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	if (log != INVALID_HANDLE_VALUE) {
		si.hStdOutput = log;
		si.hStdError = log;
	}
	PROCESS_INFORMATION pi = {0};
	if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		d->server_proc = pi.hProcess;
		CloseHandle(pi.hThread);
	}
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
}

static DWORD WINAPI warmup_thread(LPVOID param)
{
	Dict *d = (Dict *)param;
	if (!server_health(d)) {
		spawn_server(d);
		for (int i = 0; i < 240; i++) {          /* 最多 60 秒 */
			if (server_health(d)) break;
			Sleep(250);
		}
	}
	if (server_health(d)) InterlockedExchange(&d->server_ready, 1);
	return 0;
}

static void start_warmup(Dict *d)
{
	HANDLE h = CreateThread(NULL, 0, warmup_thread, d, 0, NULL);
	if (h) CloseHandle(h);
}

/* 把 prompt 转义进 JSON 字符串 */
static void json_escape(const char *src, char *dst, size_t cap)
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
			if (c < 0x20) {
				int w = snprintf(dst + j, cap - j, "\\u%04x", c);
				if (w > 0) j += (size_t)w;
			} else {
				dst[j++] = (char)c;
			}
		}
	}
	dst[j] = 0;
}

/* 从 /completion 响应里取 " yes"/" no" 的 logprob */
static bool parse_yesno(const char *resp, double *yes, double *no)
{
	const char *start = strstr(resp, "\"top_logprobs\"");
	const char *p = start ? start : resp;
	const char *end = p + 8000;
	*yes = NAN;
	*no = NAN;
	for (int guard = 0; guard < 64 && *p; guard++) {
		const char *idp = strstr(p, "\"id\"");
		if (!idp || idp > end) break;
		const char *q = skip_ws(idp + 4);
		if (*q != ':') { p = idp + 4; continue; }
		q = skip_ws(q + 1);
		long id = strtol(q, NULL, 10);
		const char *lp = strstr(q, "\"logprob\"");
		if (!lp || lp > end) break;
		const char *r = skip_ws(lp + 9);
		if (*r != ':') { p = lp + 9; continue; }
		r = skip_ws(r + 1);
		double v = strtod(r, NULL);
		if (id == JEV_TOKEN_YES) *yes = v;
		else if (id == JEV_TOKEN_NO) *no = v;
		p = r;
	}
	return !isnan(*yes) && !isnan(*no);
}

/* 一次 slot 判定：k = 0 问「一致」，k = 1 问「不一致」；返回 logit 差 */
static bool jev_slot(const Dict *d, const char *meaning, const char *ans, int k, double *out_d)
{
	static char prompt[8192];
	static char body[16384];
	static char resp[131072];
	static const char *opt0 = "一致: 意思相同或非常接近";
	static const char *opt1 = "不一致: 意思不同或无关";

	int n;
	if (k == 0) {
		n = snprintf(prompt, sizeof prompt,
		    "State:\n标准释义：%s\n学生写的释义：%s\n\n"
		    "Question [choice]: 学生写的释义与标准释义表达的意思是否一致？\nOptions:\n"
		    "- %s\n- %s\nJudge each option:\n%s ->",
		    meaning, ans, opt0, opt1, opt0);
	} else {
		n = snprintf(prompt, sizeof prompt,
		    "State:\n标准释义：%s\n学生写的释义：%s\n\n"
		    "Question [choice]: 学生写的释义与标准释义表达的意思是否一致？\nOptions:\n"
		    "- %s\n- %s\nJudge each option:\n%s ->\n%s ->",
		    meaning, ans, opt0, opt1, opt0, opt1);
	}
	if (n <= 0 || (size_t)n >= sizeof prompt) return false;

	static char esc[12288];
	json_escape(prompt, esc, sizeof esc);
	n = snprintf(body, sizeof body,
	    "{\"prompt\":\"%s\",\"n_predict\":1,\"temperature\":0,\"n_probs\":32,\"cache_prompt\":false}",
	    esc);
	if (n <= 0 || (size_t)n >= sizeof body) return false;

	size_t rl = 0;
	if (!http_request(d->jcfg.host, d->jcfg.port, L"POST", "/completion", body,
	                  resp, sizeof resp, &rl, d->jcfg.timeout_ms))
		return false;

	double yes, no;
	if (!parse_yesno(resp, &yes, &no)) return false;
	*out_d = yes - no;
	return true;
}

/* 判分主入口：返回「意思接近」概率（0-1），-1 = 不可用 */
static double judge_p(Dict *d, const char *meaning, const char *ans)
{
	for (int i = 0; i < d->cache_n && i < 8; i++) {
		if (strcmp(d->cache[i].meaning, meaning) == 0 && strcmp(d->cache[i].ans, ans) == 0)
			return d->cache[i].p;
	}
	double d0, d1;
	if (!jev_slot(d, meaning, ans, 0, &d0)) return -1;
	if (!jev_slot(d, meaning, ans, 1, &d1)) return -1;
	double z = (d0 - d1) / JEV_TEMPERATURE;
	if (z > 30.0) z = 30.0;
	if (z < -30.0) z = -30.0;
	double p = 1.0 / (1.0 + exp(-z));

	int idx = d->cache_n < 8 ? d->cache_n++ : (d->cache_n++ % 8);
	snprintf(d->cache[idx].meaning, sizeof d->cache[idx].meaning, "%s", meaning);
	snprintf(d->cache[idx].ans, sizeof d->cache[idx].ans, "%s", ans);
	d->cache[idx].p = p;
	return p;
}

/* ================================================================== */
/* 出题流程（与 Python 版逐条对应）                                     */
/* ================================================================== */

static const XtkColor COL_RED    = {0xE81123FF};  /* 结果红 */
static const XtkColor COL_GREEN  = {0x10893EFF};  /* 结果绿 */
static const XtkColor COL_GRAY   = {0x808080FF};
static const XtkColor COL_BLUE   = {0x0067C0FF};  /* 提示字母蓝 */

static Word *cur_word(Dict *m) { return &m->words[m->queue[m->pos - 1]]; }

static void set_result(Dict *m, const char *text, XtkColor color)
{
	snprintf(m->result, sizeof m->result, "%s", text);
	m->result_color = color;
	m->result_set = true;
}

static void clear_result(Dict *m)
{
	m->result[0] = 0;
	m->result_set = false;
}

static bool in_wrong(const Dict *m, int widx)
{
	for (int i = 0; i < m->wlen; i++) if (m->wrong[i] == widx) return true;
	return false;
}

static void next_word(Dict *m)
{
	if (m->qlen == 0) {
		m->phase = PH_DONE;
		return;
	}
	if (m->phase == PH_WRONG) {
		/* 答错后放弃：公布答案（挂屏幕上），该词记入错词 */
		int widx = m->queue[m->pos - 1];
		if (!in_wrong(m, widx)) m->wrong[m->wlen++] = widx;
		Word *w = &m->words[widx];
		char t[800];
		if (m->mode == 1) snprintf(t, sizeof t, "释义：%s", w->meaning);
		else              snprintf(t, sizeof t, "原词：%s", w->word);
		set_result(m, t, COL_RED);
	} else {
		clear_result(m);
	}

	if (m->pos >= m->qlen) {
		if (m->wlen > 0) {
			memcpy(m->preview, m->wrong, sizeof(int) * (size_t)m->wlen);
			m->preview_n = m->wlen;
			m->phase = PH_PREVIEW;
			return;
		}
		m->phase = PH_DONE;
		return;
	}

	m->pos++;
	m->phase = PH_NORMAL;
	m->answer[0] = 0;
}

static void do_submit(Dict *m)
{
	if (m->phase != PH_NORMAL && m->phase != PH_WRONG) return;
	Word *w = cur_word(m);

	bool ok = (m->mode == 1) ? meaning_match(m->answer, w->meaning)
	                         : str_ci_eq(m->answer, w->word);
	if (ok) {
		set_result(m, (m->mode == 1) ? "答对了！" : "答对了！", COL_GREEN);
		m->phase = PH_CORRECT;
		m->answer[0] = 0;
		return;
	}

	if (m->mode == 1 && m->jcfg.enabled && !is_blank(m->answer)) {
		if (!m->server_ready && server_health(m))
			InterlockedExchange(&m->server_ready, 1);
		if (m->server_ready) {
			double p = judge_p(m, w->meaning, m->answer);
			if (p >= 0.0) {
				char t[256];
				if (p >= m->jcfg.threshold) {
					snprintf(t, sizeof t, "答对了！模型判为接近（%.2f）", p);
					set_result(m, t, COL_GREEN);
					m->phase = PH_CORRECT;
					m->answer[0] = 0;
				} else {
					snprintf(t, sizeof t, "答错了，再试一次（接近度 %.2f）", p);
					set_result(m, t, COL_RED);
					m->phase = PH_WRONG;
				}
				return;
			}
		}
	}

	set_result(m, "答错了，再试一次", COL_RED);
	m->phase = PH_WRONG;
}

static void do_dont(Dict *m)
{
	if (m->phase != PH_NORMAL && m->phase != PH_WRONG) return;
	int widx = m->queue[m->pos - 1];
	if (!in_wrong(m, widx)) m->wrong[m->wlen++] = widx;
	Word *w = &m->words[widx];
	char t[800];
	if (m->mode == 1) snprintf(t, sizeof t, "释义：%s", w->meaning);
	else              snprintf(t, sizeof t, "原词：%s", w->word);
	set_result(m, t, COL_RED);
	m->phase = PH_REVEALED;
	m->answer[0] = 0;
}

static void start_round(Dict *m)
{
	m->round++;
	memcpy(m->queue, m->preview, sizeof(int) * (size_t)m->preview_n);
	m->qlen = m->preview_n;
	memcpy(m->round_words, m->preview, sizeof(int) * (size_t)m->preview_n);
	m->rwlen = m->preview_n;
	m->wlen = 0;
	m->pos = 0;
	m->phase = PH_NORMAL;
	m->answer[0] = 0;
	clear_result(m);
	next_word(m);
}

static void do_restart(Dict *m)
{
	if (m->round == 1 || m->phase == PH_DONE) {
		m->round = 1;
		m->rwlen = m->nwords;
		for (int i = 0; i < m->nwords; i++) m->round_words[i] = i;
	}
	memcpy(m->queue, m->round_words, sizeof(int) * (size_t)m->rwlen);
	m->qlen = m->rwlen;
	m->wlen = 0;
	m->pos = 0;
	m->phase = PH_NORMAL;
	m->answer[0] = 0;
	clear_result(m);
	next_word(m);
}

static void do_toggle_mode(Dict *m)
{
	m->mode = !m->mode;
	if (m->mode == 1 && m->jcfg.enabled && !m->server_ready)
		start_warmup(m);                     /* 切到 en2cn：预热本地 JEV */
	if (m->qlen == 0) return;
	if (m->phase == PH_DONE) { do_restart(m); return; }
	m->phase = PH_NORMAL;
	m->answer[0] = 0;
	clear_result(m);
}

static void update(void *model, XtkMsg msg)
{
	Dict *m = (Dict *)model;
	switch (msg.tag) {
	case MSG_ANSWER:
		if (msg.ptr) snprintf(m->answer, sizeof m->answer, "%s", (const char *)msg.ptr);
		break;
	case MSG_SUBMIT:      do_submit(m);      break;
	case MSG_DONT:        do_dont(m);        break;
	case MSG_NEXT:        next_word(m);      break;
	case MSG_RESTART:     do_restart(m);     break;
	case MSG_MODE:        do_toggle_mode(m); break;
	case MSG_START_ROUND: start_round(m);    break;
	default: break;
	}
}

/* ================================================================== */
/* 视图                                                                */
/* ================================================================== */

static XtkEl *view_impl(XtkUi *ui, void *model)
{
	Dict *m = (Dict *)model;

	if (m->phase == PH_PREVIEW) {
		/* —— 错词预览：列出全部错词，过一遍再开默写 —— */
		static XtkEl *items[DICT_MAX_WORDS + 1];
		int k = 0;
		for (int i = 0; i < m->preview_n && k < DICT_MAX_WORDS; i++) {
			Word *w = &m->words[m->preview[i]];
			items[k++] = xtk_text(ui, xtk_fmt(ui, "%d. %s  [%s]  %s", i + 1, w->word, w->pos, w->meaning),
			                      (XtkTextDesc){.size = 14});
		}
		items[k] = XTK_END;

		return xtk_grow(xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
		    xtk_kids(
		        xtk_titlebar(ui, "单词默写", (XtkTitleBarDesc){.subtitle = "Project-Xent 前端", .integrate_window = true}),
		        xtk_grow(xtk_column(ui, (XtkStackDesc){.gap = 12, .padding = {28, 16, 28, 24}, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
		            xtk_kids(
		                xtk_text(ui, xtk_fmt(ui, "本轮错词 %d 个，过一遍再开始", m->preview_n),
		                         (XtkTextDesc){.size = 16, .weight = XTK_FONT_SEMI_BOLD, .color = COL_RED}),
		                xtk_sized(xtk_scroll(ui, xtk_kids(
		                    xtk_column(ui, (XtkStackDesc){.gap = 6, .padding = {12, 8, 12, 8}, .align = XENT_FLEX_ALIGN_STRETCH},
		                        items))), NAN, 400),
		                xtk_row(ui, (XtkStackDesc){.gap = 10, .justify = XENT_FLEX_JUSTIFY_CENTER},
		                    xtk_kids(
		                        xtk_button(ui, "开始默写", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .click = xtk_msg(MSG_START_ROUND)}),
		                        xtk_button(ui, "重新开始", (XtkButtonDesc){.click = xtk_msg(MSG_RESTART)}))))), 1.0f))), 1.0f);
	}

	/* —— 主界面 —— */
	bool empty = (m->qlen == 0);
	bool done  = (m->phase == PH_DONE);
	bool answering = (m->phase == PH_NORMAL || m->phase == PH_WRONG);
	bool show_masked = (m->mode == 0) && !done && !empty;

	const char *progress_text;
	if (empty)      progress_text = "词表为空";
	else if (done)  progress_text = "全部完成！";
	else if (m->round == 1) progress_text = xtk_fmt(ui, "第 %d / %d 词", m->pos, m->qlen);
	else            progress_text = xtk_fmt(ui, "第 %d / %d 词 · 第 %d 轮（错词）", m->pos, m->qlen, m->round);

	const char *main_text;
	XtkTextDesc main_desc = {.size = 46, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER};
	const char *pos_text = "";
	char masked[256] = "";

	if (empty) {
		main_text = "词表为空 · 把词填进 dictation_extra.json 后重开窗口";
		main_desc.size = 15;
		main_desc.color = COL_GRAY;
	} else if (done) {
		main_text = xtk_fmt(ui, "共 %d 个词，默写 %d 轮", m->nwords, m->round);
		main_desc.size = 28;
	} else {
		Word *w = cur_word(m);
		pos_text = w->pos;
		if (m->mode == 1) {
			main_text = w->word;
			main_desc.size = 54;
		} else {
			main_text = w->meaning;
			main_desc.size = 42;
			masked_display(w->word, masked, sizeof masked);
		}
	}

	XtkEl *btn_submit = (m->phase == PH_NORMAL || m->phase == PH_WRONG)
	    ? xtk_button(ui, "提交", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .click = xtk_msg(MSG_SUBMIT)}) : NULL;
	XtkEl *btn_dont = (m->phase == PH_NORMAL)
	    ? xtk_button(ui, "不知道", (XtkButtonDesc){.click = xtk_msg(MSG_DONT)}) : NULL;
	XtkEl *btn_next = (m->phase == PH_CORRECT || m->phase == PH_REVEALED || m->phase == PH_WRONG)
	    ? xtk_button(ui, "下一个", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .click = xtk_msg(MSG_NEXT)}) : NULL;
	XtkEl *btn_again = done
	    ? xtk_button(ui, "再来一轮", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .click = xtk_msg(MSG_RESTART)}) : NULL;

	return xtk_grow(xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(
	        xtk_titlebar(ui, "单词默写", (XtkTitleBarDesc){.subtitle = "Project-Xent 前端", .integrate_window = true}),
	        /* 顶部：进度（小、安静，左对齐） */
	        xtk_column(ui, (XtkStackDesc){.gap = 8, .padding = {32, 16, 32, 10}, .align = XENT_FLEX_ALIGN_STRETCH},
	            xtk_kids(
	                xtk_text(ui, progress_text, (XtkTextDesc){.size = 12, .color = COL_GRAY}),
	                xtk_progress(ui, (XtkProgressDesc){.value = (float)m->pos, .max = (float)(m->qlen > 0 ? m->qlen : 1)}))),
	        xtk_divider(ui),
	        /* 中部：题面舞台——撑满剩余高度，内容垂直居中 */
	        xtk_grow(xtk_column(ui, (XtkStackDesc){.padding = {28, 14, 28, 10}, .align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	            xtk_kids(
	                xtk_grow(xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {36, 44, 36, 44}, .align = XENT_FLEX_ALIGN_STRETCH,
	                                                    .justify = XENT_FLEX_JUSTIFY_CENTER, .fill = true},
	                    xtk_kids(
	                        xtk_text(ui, pos_text, (XtkTextDesc){.size = 13, .align = XTK_TEXT_CENTER, .color = COL_GRAY}),
	                        xtk_text(ui, main_text, main_desc),
	                        show_masked ? xtk_text(ui, masked, (XtkTextDesc){.size = 26, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER, .color = COL_BLUE}) : NULL,
	                        m->result_set ? xtk_text(ui, m->result, (XtkTextDesc){.size = 17, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER, .color = m->result_color}) : NULL,
	                        (empty || done) ? NULL : xtk_row(ui, (XtkStackDesc){.justify = XENT_FLEX_JUSTIFY_CENTER},
	                            xtk_kids(xtk_sized(xtk_textbox(ui, (m->mode == 1) ? "输入中文释义" : "输入英文单词",
	                                (XtkTextBoxDesc){.content = m->answer, .change = xtk_msg(MSG_ANSWER), .disabled = !answering}), 460, 48))),
	                        xtk_row(ui, (XtkStackDesc){.gap = 12, .justify = XENT_FLEX_JUSTIFY_CENTER},
	                            xtk_kids(btn_submit, btn_dont, btn_next, btn_again)))), 1.0f))), 1.0f),
	        /* 底部：次级操作，两端分开放 */
	        xtk_row(ui, (XtkStackDesc){.padding = {32, 0, 32, 18}, .justify = XENT_FLEX_JUSTIFY_SPACE_BETWEEN, .align = XENT_FLEX_ALIGN_CENTER},
	            xtk_kids(
	                xtk_button(ui, "重新开始", (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .click = xtk_msg(MSG_RESTART)}),
	                xtk_button(ui, (m->mode == 1) ? "切换到「释义 → 单词」" : "切换到「单词 → 释义」",
	                           (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .click = xtk_msg(MSG_MODE)}))))), 1.0f);
}

/* ================================================================== */
/* 入口                                                                */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* 去掉系统标题栏                                                       */
/* ------------------------------------------------------------------ */
/* 框架的 .integrate_window = true 本应摘掉 OS caption（自绘标题栏 + 自带
 * 按钮），但在本机环境下 DWM 仍画了一条原生标题栏，窗口顶部就出现了两组
 * 最小化/最大化/关闭。等窗口一出现自己摘掉 WS_CAPTION 即可；框架负责的
 * 拖动区、按钮命中（HTCAPTION/HTMINBUTTON/HTMAXBUTTON）实测都不受影响。 */
static DWORD WINAPI caption_fix_thread(LPVOID unused)
{
	(void)unused;
	for (int i = 0; i < 600; i++) {
		HWND h = FindWindowW(L"FluxentWindowClass", NULL);
		/* 必须等窗口真正显示出来再动手：太早改样式会打断框架的显示流程 */
		if (h && IsWindowVisible(h)) {
			LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
			if (st & WS_CAPTION) {
				SetWindowLongPtrW(h, GWL_STYLE, st & ~(LONG_PTR)WS_CAPTION);
				SetWindowPos(h, NULL, 0, 0, 0, 0,
				    SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
			return 0;
		}
		Sleep(15);
	}
	return 1;
}

/* 解析相对路径：先看 CWD，再看 exe 往上 5 级的「背词工具」根目录 */
static void resolve_path(const char *in, char *out, size_t cap)
{
	if (in[0] == '/' || in[0] == '\\' || (in[0] && in[1] == ':')) {
		snprintf(out, cap, "%s", in);
		return;
	}
	FILE *f = fopen(in, "rb");
	if (f) { fclose(f); snprintf(out, cap, "%s", in); return; }

	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	wchar_t *slash = wcsrchr(exe, L'\\');
	if (slash) *slash = 0;
	for (int i = 0; i < 5; i++) {
		slash = wcsrchr(exe, L'\\');
		if (!slash) break;
		*slash = 0;
	}
	wchar_t wrel[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, in, -1, wrel, MAX_PATH);
	wchar_t full[MAX_PATH];
	_snwprintf(full, MAX_PATH, L"%ls\\%ls", exe, wrel);
	if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
		char utf8[MAX_PATH * 3];
		WideCharToMultiByte(CP_UTF8, 0, full, -1, utf8, sizeof utf8, NULL, NULL);
		snprintf(out, cap, "%s", utf8);
		return;
	}
	snprintf(out, cap, "%s", in);   /* 都不在：原样返回（上层会失败并回落） */
}

#ifndef DICT_LAYOUT_TEST
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int showCmd)
{
	(void)hInst; (void)hPrev; (void)cmdLine; (void)showCmd;

	Dict *m = &g_app;
	memset(m, 0, sizeof *m);

	/* 数据文件路径：命令行参数优先（UTF-16），否则用 CWD 下的默认名 */
	char words_path[MAX_PATH] = "dictation_extra.json";
	char judge_path[MAX_PATH] = "dictation_judge.json";
	{
		int argc = 0;
		LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (argv) {
			if (argc >= 2) WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, words_path, MAX_PATH, NULL, NULL);
			if (argc >= 3) WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, judge_path, MAX_PATH, NULL, NULL);
			LocalFree(argv);
		}
	}
	resolve_path(words_path, m->root, sizeof m->root);   /* 记住可用的词表路径 */

	load_judge_cfg(judge_path, &m->jcfg);
	{
		char p[MAX_PATH];
		resolve_path(m->jcfg.exe, p, sizeof p);   snprintf(m->jcfg.exe, sizeof m->jcfg.exe, "%s", p);
		resolve_path(m->jcfg.model, p, sizeof p); snprintf(m->jcfg.model, sizeof m->jcfg.model, "%s", p);
	}

	if (!load_words(m->root, m)) {
		/* 词表读不出来：空表运行（界面会提示） */
		m->nwords = 0;
	}

	/* 初始状态：第 1 轮全部词 */
	m->round = 1;
	m->mode = 0;
	m->rwlen = m->nwords;
	for (int i = 0; i < m->nwords; i++) m->round_words[i] = i;
	memcpy(m->queue, m->round_words, sizeof(int) * (size_t)m->rwlen);
	m->qlen = m->rwlen;
	m->pos = 0;
	m->phase = PH_NORMAL;
	next_word(m);

	FluxAppCfg cfg = {
	    .title = L"单词默写",
	    .width = 760,
	    .height = 700,
	    .dark_mode = flux_theme_systemdark(),
	    .backdrop = FLUX_BACKDROP_MICA,
	};
	/* 摘掉系统标题栏（见 caption_fix_thread） */
	{
		HANDLE th = CreateThread(NULL, 0, caption_fix_thread, NULL, 0, NULL);
		if (th) CloseHandle(th);
	}

	int rc = flux_run(&cfg, m, update, view_impl);

	if (m->server_proc) {
		TerminateProcess(m->server_proc, 0);
		CloseHandle(m->server_proc);
	}
	return rc;
}
#endif /* !DICT_LAYOUT_TEST */

/* ================================================================== */
/* 无头布局自检（xmake build test_dict_layout && xmake run test_dict_layout） */
/* ================================================================== */
#ifdef DICT_LAYOUT_TEST

#include "flux_internal.h"
#include <fluxent/flux_host.h>

/* 最小视图：按 STAGE 环境变量逐级加元素，定位崩溃点 */
static int g_stage = 0;

static XtkEl *view_min(XtkUi *ui, void *model)
{
	Dict     *m      = (Dict *)model;
	XtkEl    *title  = xtk_titlebar(ui, "单词默写", (XtkTitleBarDesc){.subtitle = "Project-Xent 前端", .integrate_window = true});
	XtkEl    *t1     = xtk_text(ui, "发誓", (XtkTextDesc){.size = 42, .weight = XTK_FONT_SEMI_BOLD, .align = XTK_TEXT_CENTER});
	XtkEl    *bar    = xtk_progress(ui, (XtkProgressDesc){.value = 1, .max = 3});
	XtkEl    *dv     = xtk_divider(ui);
	XtkEl    *tbrow  = xtk_row(ui, (XtkStackDesc){.justify = XENT_FLEX_JUSTIFY_CENTER},
	    xtk_kids(xtk_sized(xtk_textbox(ui, "输入英文单词",
	        (XtkTextBoxDesc){.content = m->answer, .change = xtk_msg(MSG_ANSWER)}), 460, 48)));
	XtkEl    *brrow  = xtk_row(ui, (XtkStackDesc){.gap = 12, .justify = XENT_FLEX_JUSTIFY_CENTER},
	    xtk_kids(xtk_button(ui, "提交", (XtkButtonDesc){.style = XTK_BUTTON_ACCENT, .click = xtk_msg(MSG_SUBMIT)})));
	XtkEl    *card_e = xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {36, 44, 36, 44}, .align = XENT_FLEX_ALIGN_STRETCH},
	    xtk_kids(t1, xtk_spacer(ui), tbrow, brrow));
	XtkEl    *foot   = xtk_row(ui, (XtkStackDesc){.padding = {32, 0, 32, 18}, .justify = XENT_FLEX_JUSTIFY_SPACE_BETWEEN, .align = XENT_FLEX_ALIGN_CENTER},
	    xtk_kids(xtk_button(ui, "重新开始", (XtkButtonDesc){.style = XTK_BUTTON_SUBTLE, .click = xtk_msg(MSG_RESTART)})));

	switch (g_stage) {
	case 0: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(title));
	case 1: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(title, t1, bar, dv));
	case 2: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(title, t1, bar, dv, card_e));
	case 3: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(title, t1, bar, dv, xtk_grow(card_e, 1.0f)));
	case 4: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(title, t1, bar, dv, xtk_grow(card_e, 1.0f), foot));
	case 5: return view_impl(ui, model);
	case 6: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(xtk_titlebar(ui, "单词默写", (XtkTitleBarDesc){.subtitle = "Project-Xent 前端"})));   /* 不整合 */
	case 10: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {36, 44, 36, 44}, .align = XENT_FLEX_ALIGN_STRETCH}, xtk_kids(t1))));
	case 11: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {36, 44, 36, 44}, .align = XENT_FLEX_ALIGN_STRETCH}, xtk_kids(t1, xtk_spacer(ui)))));
	case 12: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {36, 44, 36, 44}, .align = XENT_FLEX_ALIGN_STRETCH}, xtk_kids(t1, tbrow))));
	case 13: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(xtk_card(ui, (XtkStackDesc){.gap = 16, .padding = {36, 44, 36, 44}, .align = XENT_FLEX_ALIGN_STRETCH}, xtk_kids(t1, tbrow, brrow))));
	case 14: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(t1, bar, dv));
	case 7: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true},
	    xtk_kids(t1, bar, dv));                                                                          /* 没有标题栏 */
	default: return xtk_column(ui, (XtkStackDesc){.align = XENT_FLEX_ALIGN_STRETCH, .fill = true}, xtk_kids(title));
	}
}

static void dump_node(XentCtx *ctx, XtkNode const *n, const char *path, int depth)
{
	XentRect r  = {0};
	bool     ok = xent_layout_rect(ctx, xtk_node_id(n), &r);
	printf("%-12s d=%d  %s x=%7.1f y=%7.1f w=%7.1f h=%7.1f\n", path, depth, ok ? "rect" : "----", r.x, r.y, r.w, r.h);
	int nc = xtk_node_nchild(n);
	for (int i = 0; i < nc; i++) {
		char sub[128];
		snprintf(sub, sizeof sub, "%s.%d", path, i);
		dump_node(ctx, xtk_node_child(n, i), sub, depth + 1);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃也要能看到进度 */
	{ const char *s = getenv("STAGE"); if (s) g_stage = atoi(s); }
	printf("STAGE=%d\n", g_stage);
	Dict *m = &g_app;
	memset(m, 0, sizeof *m);

	/* 造 3 个假词，不依赖词表文件 */
	static const char *fake[3][3] = {{"swear", "v.", "发誓"}, {"swirl", "v.", "盘旋"}, {"symmetric", "adj.", "对称的"}};
	m->nwords = 3;
	for (int i = 0; i < 3; i++) {
		snprintf(m->words[i].word, DICT_MAX_WORD, "%s", fake[i][0]);
		snprintf(m->words[i].pos, DICT_MAX_POS, "%s", fake[i][1]);
		snprintf(m->words[i].meaning, DICT_MAX_MEANING, "%s", fake[i][2]);
	}
	m->round = 1; m->mode = 0; m->rwlen = 3;
	for (int i = 0; i < 3; i++) m->round_words[i] = i;
	memcpy(m->queue, m->round_words, sizeof(int) * 3);
	m->qlen = 3; m->pos = 0; m->phase = PH_NORMAL;
	next_word(m);

	XentCfg config = {0};
	XentCtx *ctx = xent_ctx_create(&config);
	/* 文本测量服务：没有它，文本节点的高度会退回默认字号（假高度） */
	FluxText *txt = flux_text_create();
	printf("flux_text_register=%d\n", (int)flux_text_register(txt, ctx));
	FluxStore *store = flux_store_create(64);
	flux_store_bind(store, ctx);
	XentNodeId host = xent_node_create(ctx);
	xent_setproto(ctx, host, XENT_PROTOCOL_FLEX);
	xent_setflexdir(ctx, host, XENT_FLEX_COLUMN);
	xent_setitems(ctx, host, XENT_FLEX_ALIGN_STRETCH);
	int mm = 0;
	FluxBackendCtx bctx = {.ctx = ctx, .store = store, .app = NULL, .runtime = NULL};
	XtkHost *xtkhost = flux_host_adopt(ctx, store, host);
	bctx.host = xtkhost;
	XtkRtCfg tcfg = flux_xtk_runtime_config(&bctx);
	( void ) mm;
	XtkRt *rt = xtk_rt_create(xtkhost, &tcfg, host, m, update, view_min);   /* model 必须是 Dict* */
	if (rt) bctx.runtime = rt;
	printf("S3 rt ok rt=%p\n", (void *)rt);
	XtkFrameResult fr = xtk_rt_frame(rt);
	( void ) fr;
	printf("S4 frame ok\n");
	xent_layout(ctx, host, 760.0f, 700.0f);
	printf("S5 layout ok\n");
	dump_node(ctx, xtk_rt_root(rt), "root", 0);

	xtk_rt_destroy(rt);
	flux_host_destroy(xtkhost);
	flux_store_destroy(store);
	flux_text_destroy(txt);
	xent_ctx_destroy(ctx);
	return 0;
}
#endif /* DICT_LAYOUT_TEST */
