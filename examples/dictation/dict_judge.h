/**
 * @file dict_judge.h
 * @brief 后端 · 本地小模型判分（「单词 → 释义」字面判不出时兜底）。
 *
 * 本地 llama-server（Jev-Style-0.8B-Decision-v3）的 /completion 接口，
 * 取 " yes"(id 9542) / " no"(id 874) 两个 token 的 logprob 差，
 * 按校准温度换算成「意思接近」的概率（与 Python 版同算法）。
 *
 * 另有两种后端（"api" 字段）："judge" = 2B 判分服务 POST /judge；
 * "systemone" = 新版 llama-server 的 /v1/systemone（vllm-d3-nano 等决策模型），取「一致」选项的概率。
 *
 * 配置文件 dictation_judge.json：enabled/exe/model/host/port/threshold/timeout_s/api/args。
 * 没有配置文件 = 关闭，只用字面判分。
 */
#pragma once

#include <stdbool.h>

typedef struct Judge Judge;

/* 读配置；exe/model 的相对路径由 resolve(in, out, cap) 解析（可为 NULL） */
Judge *judge_create(const char *cfg_path, void (*resolve)(const char *in, char *out, int cap));
void   judge_destroy(Judge *j);   /* 会结束本程序拉起的 llama-server */

bool   judge_enabled(const Judge *j);
double judge_threshold(const Judge *j);
/* 一词多义合并的阈值（双向都大于它才合并）；0 = 不做合并（"merge_threshold"，systemone 默认 0.98，其余默认 0） */
double judge_merge_threshold(const Judge *j);

/* 后台预热：服务没起就拉起来并等它就绪（不阻塞调用方） */
void   judge_warmup(Judge *j);

/* 「意思接近」概率 0–1；服务不可用返回 -1。线程安全（内部加锁，同一时刻只发一个请求） */
double judge_p(Judge *j, const char *meaning, const char *ans);
