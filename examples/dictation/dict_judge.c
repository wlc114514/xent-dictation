/**
 * @file dict_judge.c
 * @brief 后端 · 本地 JEV 判分实现（WinHTTP + 子进程 llama-server）。
 */
#include "dict_judge.h"
#include "dict_text.h"

#include <windows.h>
#include <winhttp.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JEV_TEMPERATURE 0.8800546821789332
#define JEV_TOKEN_YES   9542
#define JEV_TOKEN_NO    874
#define JUDGE_CACHE     8

struct Judge {
	bool   enabled;
	char   exe[MAX_PATH * 3];
	char   model[MAX_PATH * 3];
	char   host[64];
	int    port;
	double threshold;
	int    timeout_ms;

	volatile LONG ready;
	volatile LONG spawned;
	volatile LONG warming;
	HANDLE        proc;

	struct { char meaning[640]; char ans[512]; double p; } cache[JUDGE_CACHE];
	int cache_n;
};

/* ================================================================== */
/* 配置                                                                */
/* ================================================================== */

Judge *judge_create(const char *cfg_path, void (*resolve)(const char *in, char *out, int cap))
{
	Judge *j = (Judge *)calloc(1, sizeof *j);
	if (!j) return NULL;
	snprintf(j->exe, sizeof j->exe, "jev/llama/llama-server.exe");
	snprintf(j->model, sizeof j->model, "jev/Jev-Style-0.8B-Decision-v3-Q4_K_M.gguf");
	snprintf(j->host, sizeof j->host, "127.0.0.1");
	j->port       = 8087;
	j->threshold  = 0.9;
	j->timeout_ms = 15000;

	char *buf = read_file(cfg_path, NULL);
	if (!buf) return j;                       /* 没有配置文件 → 关闭 */
	j->enabled = true;

	const char *p;
	if ((p = json_find_key(buf, "enabled")) && strncmp(p, "false", 5) == 0) j->enabled = false;
	if ((p = json_find_key(buf, "port")))      j->port = (int)strtol(p, NULL, 10);
	if ((p = json_find_key(buf, "threshold"))) j->threshold = strtod(p, NULL);
	if ((p = json_find_key(buf, "timeout_s"))) {
		double t = strtod(p, NULL);
		if (t > 0) j->timeout_ms = (int)(t * 1000);
	}
	if ((p = json_find_key(buf, "exe")))   js_str(p, j->exe, sizeof j->exe);
	if ((p = json_find_key(buf, "model"))) js_str(p, j->model, sizeof j->model);
	if ((p = json_find_key(buf, "host")))  js_str(p, j->host, sizeof j->host);
	free(buf);

	if (resolve) {
		char tmp[MAX_PATH * 3];
		resolve(j->exe, tmp, (int)sizeof tmp);   snprintf(j->exe, sizeof j->exe, "%s", tmp);
		resolve(j->model, tmp, (int)sizeof tmp); snprintf(j->model, sizeof j->model, "%s", tmp);
	}
	return j;
}

void judge_destroy(Judge *j)
{
	if (!j) return;
	if (j->proc) {
		TerminateProcess(j->proc, 0);
		CloseHandle(j->proc);
	}
	free(j);
}

bool   judge_enabled(const Judge *j)   { return j && j->enabled; }
double judge_threshold(const Judge *j) { return j ? j->threshold : 1.0; }

/* ================================================================== */
/* HTTP                                                                */
/* ================================================================== */

static bool http_request(const Judge *j, const wchar_t *method, const wchar_t *path,
                         const char *body, char *resp, size_t cap, int timeout_ms)
{
	bool    ok = false;
	wchar_t whost[64];
	if (MultiByteToWideChar(CP_UTF8, 0, j->host, -1, whost, 64) <= 0) return false;

	HINTERNET ses = WinHttpOpen(L"dictation-xent/2.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
	                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) return false;
	WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

	HINTERNET con = WinHttpConnect(ses, whost, (INTERNET_PORT)j->port, 0);
	HINTERNET req = con ? WinHttpOpenRequest(con, method, path, NULL, WINHTTP_NO_REFERER,
	                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
	if (req) {
		DWORD blen = body ? (DWORD)strlen(body) : 0;
		if (WinHttpSendRequest(req, body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
		                       (DWORD)-1L, (LPVOID)body, blen, blen, 0) &&
		    WinHttpReceiveResponse(req, NULL)) {
			size_t used = 0;
			DWORD  got  = 0;
			while (used + 1 < cap &&
			       WinHttpReadData(req, resp + used, (DWORD)(cap - 1 - used), &got) && got > 0)
				used += got;
			resp[used] = 0;
			ok = used > 0;
		}
	}
	if (req) WinHttpCloseHandle(req);
	if (con) WinHttpCloseHandle(con);
	WinHttpCloseHandle(ses);
	return ok;
}

static bool server_health(const Judge *j)
{
	char resp[512];
	return http_request(j, L"GET", L"/health", NULL, resp, sizeof resp, 1200) &&
	       strstr(resp, "\"ok\"") != NULL;
}

/* ================================================================== */
/* 预热：拉起 llama-server 并等它就绪                                   */
/* ================================================================== */

static void spawn_server(Judge *j)
{
	if (InterlockedCompareExchange(&j->spawned, 1, 0) != 0) return;

	wchar_t wexe[MAX_PATH], wmodel[MAX_PATH], cmd[4096];
	MultiByteToWideChar(CP_UTF8, 0, j->exe, -1, wexe, MAX_PATH);
	MultiByteToWideChar(CP_UTF8, 0, j->model, -1, wmodel, MAX_PATH);
	_snwprintf(cmd, 4096, L"\"%ls\" -m \"%ls\" --host 127.0.0.1 --port %d -c 2048 -t 8 --no-webui",
	           wexe, wmodel, j->port);

	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE log = CreateFileW(L"jev\\server.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                         &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	STARTUPINFOW si = {0};
	si.cb          = sizeof si;
	si.dwFlags     = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	if (log != INVALID_HANDLE_VALUE) {
		si.dwFlags   |= STARTF_USESTDHANDLES;
		si.hStdOutput = log;
		si.hStdError  = log;
	}
	PROCESS_INFORMATION pi = {0};
	if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		j->proc = pi.hProcess;
		CloseHandle(pi.hThread);
	}
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
}

static DWORD WINAPI warmup_thread(LPVOID param)
{
	Judge *j = (Judge *)param;
	if (!server_health(j)) {
		spawn_server(j);
		for (int i = 0; i < 240 && !server_health(j); i++) Sleep(250);   /* 最多约 60 秒 */
	}
	if (server_health(j)) InterlockedExchange(&j->ready, 1);
	InterlockedExchange(&j->warming, 0);
	return 0;
}

void judge_warmup(Judge *j)
{
	if (!judge_enabled(j) || j->ready) return;
	if (InterlockedCompareExchange(&j->warming, 1, 0) != 0) return;
	HANDLE h = CreateThread(NULL, 0, warmup_thread, j, 0, NULL);
	if (h) CloseHandle(h);
	else   InterlockedExchange(&j->warming, 0);
}

/* ================================================================== */
/* 判分                                                                */
/* ================================================================== */

/* 从 /completion 响应里取 " yes"/" no" 的 logprob */
static bool parse_yesno(const char *resp, double *yes, double *no)
{
	const char *start = strstr(resp, "\"top_logprobs\"");
	const char *p     = start ? start : resp;
	const char *end   = p + strlen(p);
	if (end - p > 8000) end = p + 8000;
	*yes = *no = NAN;
	for (int guard = 0; guard < 64 && *p; guard++) {
		const char *idp = strstr(p, "\"id\"");
		if (!idp || idp > end) break;
		const char *q = skip_ws(idp + 4);
		if (*q != ':') { p = idp + 4; continue; }
		long id = strtol(skip_ws(q + 1), NULL, 10);
		const char *lp = strstr(q, "\"logprob\"");
		if (!lp || lp > end) break;
		const char *r = skip_ws(lp + 9);
		if (*r != ':') { p = lp + 9; continue; }
		r = skip_ws(r + 1);
		double v = strtod(r, NULL);
		if (id == JEV_TOKEN_YES)     *yes = v;
		else if (id == JEV_TOKEN_NO) *no = v;
		p = r;
	}
	return !isnan(*yes) && !isnan(*no);
}

/* 一次 slot 判定：k = 0 问「一致」，k = 1 问「不一致」；返回 logit 差 */
static bool jev_slot(const Judge *j, const char *meaning, const char *ans, int k, double *out)
{
	static char prompt[8192], esc[12288], body[16384], resp[131072];
	static const char *opt0 = "一致: 意思相同或非常接近";
	static const char *opt1 = "不一致: 意思不同或无关";

	int n = snprintf(prompt, sizeof prompt,
	    "State:\n标准释义：%s\n学生写的释义：%s\n\n"
	    "Question [choice]: 学生写的释义与标准释义表达的意思是否一致？\nOptions:\n"
	    "- %s\n- %s\nJudge each option:\n%s ->%s%s%s",
	    meaning, ans, opt0, opt1, opt0,
	    k ? "\n" : "", k ? opt1 : "", k ? " ->" : "");
	if (n <= 0 || (size_t)n >= sizeof prompt) return false;

	json_escape(prompt, esc, sizeof esc);
	n = snprintf(body, sizeof body,
	    "{\"prompt\":\"%s\",\"n_predict\":1,\"temperature\":0,\"n_probs\":32,\"cache_prompt\":false}", esc);
	if (n <= 0 || (size_t)n >= sizeof body) return false;

	if (!http_request(j, L"POST", L"/completion", body, resp, sizeof resp, j->timeout_ms)) return false;
	double yes, no;
	if (!parse_yesno(resp, &yes, &no)) return false;
	*out = yes - no;
	return true;
}

double judge_p(Judge *j, const char *meaning, const char *ans)
{
	if (!judge_enabled(j)) return -1;
	if (!j->ready) {
		if (!server_health(j)) { judge_warmup(j); return -1; }
		InterlockedExchange(&j->ready, 1);
	}

	int have = j->cache_n < JUDGE_CACHE ? j->cache_n : JUDGE_CACHE;
	for (int i = 0; i < have; i++)
		if (strcmp(j->cache[i].meaning, meaning) == 0 && strcmp(j->cache[i].ans, ans) == 0)
			return j->cache[i].p;

	double d0, d1;
	if (!jev_slot(j, meaning, ans, 0, &d0) || !jev_slot(j, meaning, ans, 1, &d1)) return -1;
	double z = (d0 - d1) / JEV_TEMPERATURE;
	if (z > 30.0)  z = 30.0;
	if (z < -30.0) z = -30.0;
	double p = 1.0 / (1.0 + exp(-z));

	int slot = j->cache_n++ % JUDGE_CACHE;
	snprintf(j->cache[slot].meaning, sizeof j->cache[slot].meaning, "%s", meaning);
	snprintf(j->cache[slot].ans, sizeof j->cache[slot].ans, "%s", ans);
	j->cache[slot].p = p;
	return p;
}
