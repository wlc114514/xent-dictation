"""本地小模型判分（「单词 → 释义」字面判不出时兜底）。

配置文件 dictation_judge.json（没有 = 关闭，只用字面判分）：
  enabled / exe / model / host / port / threshold / timeout_s / api / args / merge_threshold
三种后端（"api"）：
  不写        → 0.8B llama-server 的 /completion，取 " yes"(9542) / " no"(874) 的 logprob 差，按校准温度换成概率
  "judge"     → 2B 判分服务 POST /judge {"meaning", "answer"} → {"p"}（tools/jev2b_server.py）
  "systemone" → 新版 llama-server 的 /v1/systemone（vllm-d3-nano 等决策模型），取「一致」选项的概率
"""

from __future__ import annotations

import ctypes
import http.client
import json
import math
import os
import subprocess
import threading
import time

TEMPERATURE = 0.8800546821789332
TOKEN_YES, TOKEN_NO = 9542, 874
CACHE = 8

_OPT0 = "一致: 意思相同或非常接近"
_OPT1 = "不一致: 意思不同或无关"


def _resolve(path: str, base: str) -> str:
    if not path or os.path.isabs(path):
        return path
    return os.path.normpath(os.path.join(base, path))


class _Job:
    """作业对象：本程序退出（哪怕崩溃）时，拉起的服务连同它的子进程一起结束。"""

    def __init__(self) -> None:
        self.handle = None
        if os.name != "nt":
            return
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = ctypes.c_void_p
        h = k32.CreateJobObjectW(None, None)
        if not h:
            return

        class BASIC(ctypes.Structure):
            _fields_ = [("a", ctypes.c_int64), ("b", ctypes.c_int64), ("LimitFlags", ctypes.c_uint32),
                        ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t),
                        ("ActiveProcessLimit", ctypes.c_uint32), ("Affinity", ctypes.c_size_t),
                        ("PriorityClass", ctypes.c_uint32), ("SchedulingClass", ctypes.c_uint32)]

        class EXT(ctypes.Structure):
            _fields_ = [("Basic", BASIC), ("Io", ctypes.c_uint64 * 6), ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t)]

        info = EXT()
        info.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        k32.SetInformationJobObject(ctypes.c_void_p(h), 9, ctypes.byref(info), ctypes.sizeof(info))
        self.handle = h
        self.k32 = k32

    def adopt(self, proc: subprocess.Popen) -> None:
        if self.handle:
            self.k32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(proc._handle)))

    def close(self) -> None:
        if self.handle:
            self.k32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


class Judge:
    def __init__(self, cfg_path: str | None = None, base: str = ".") -> None:
        self.enabled = False
        self.api = ""
        self.exe = "jev/llama/llama-server.exe"
        self.model = "jev/Jev-Style-0.8B-Decision-v3-Q4_K_M.gguf"
        self.args = ""
        self.host = "127.0.0.1"
        self.port = 8087
        self.threshold = 0.9
        self.timeout = 15.0
        self.merge_threshold = 0.0
        self.base = base
        self._lock = threading.Lock()  # 答题判分和合并检查各在自己的线程里调 p()
        self._warm_lock = threading.Lock()
        self._ready = False
        self._warming = False
        self._proc: subprocess.Popen | None = None
        self._job: _Job | None = None
        self._cache: list[tuple[str, str, float]] = []
        if cfg_path:
            self._load(cfg_path)

    def _load(self, path: str) -> None:
        try:
            with open(path, encoding="utf-8-sig") as f:
                cfg = json.load(f)
        except (OSError, ValueError):
            return
        if not isinstance(cfg, dict):
            return
        self.enabled = cfg.get("enabled", True) is not False
        self.api = str(cfg.get("api", ""))
        self.exe = str(cfg.get("exe", self.exe))
        self.model = str(cfg.get("model", self.model))
        self.args = str(cfg.get("args", ""))
        self.host = str(cfg.get("host", self.host))
        self.port = int(cfg.get("port", self.port))
        self.threshold = float(cfg.get("threshold", self.threshold))
        if float(cfg.get("timeout_s", 0) or 0) > 0:
            self.timeout = float(cfg["timeout_s"])
        # 合并要宁缺毋滥：0.98 是 vllm-d3-nano 在 268 对标注释义上测的（错合并 3/120，同义合并 137/148）；
        # 别的模型没测过，不写就不合并
        self.merge_threshold = float(cfg.get("merge_threshold", 0.98 if self.api == "systemone" else 0.0))
        self.exe = _resolve(self.exe, self.base)
        self.model = _resolve(self.model, self.base)

    def merge_threshold_or_zero(self) -> float:
        return self.merge_threshold if self.enabled else 0.0

    def close(self) -> None:
        if self._proc and self._proc.poll() is None:
            self._proc.kill()
        if self._job:
            self._job.close()

    # ------------------------------------------------------------ HTTP

    def _request(self, method: str, path: str, body: dict | None, timeout: float) -> str | None:
        try:
            conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
            data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
            headers = {"Content-Type": "application/json"} if data is not None else {}
            conn.request(method, path, body=data, headers=headers)
            resp = conn.getresponse().read().decode("utf-8", "replace")
            conn.close()
            return resp or None
        except (OSError, http.client.HTTPException):
            return None

    def _health(self) -> bool:
        resp = self._request("GET", "/health", None, 1.2)
        return resp is not None and '"ok"' in resp

    # ------------------------------------------------------------ 预热：拉起服务并等它就绪

    def _command(self) -> str:
        exe = f'"{self.exe}"'
        if self.api == "judge":
            return f"{exe} {self.args} --port {self.port} --parent-pid {os.getpid()}"
        if self.api == "systemone":  # -ngl 99：有显卡就全放上去，没有自动回落 CPU
            return f'{exe} -m "{self.model}" --host 127.0.0.1 --port {self.port} -c 2048 -ngl 99 --no-webui {self.args}'
        return f'{exe} -m "{self.model}" --host 127.0.0.1 --port {self.port} -c 2048 -t 8 --no-webui'

    def _spawn(self) -> None:
        if self._proc is not None:
            return
        log_dir = os.path.join(self.base, "jev")
        try:
            log = open(os.path.join(log_dir, "server.log"), "ab") if os.path.isdir(log_dir) else subprocess.DEVNULL
        except OSError:
            log = subprocess.DEVNULL
        flags = 0x08000000 | 0x00000004 if os.name == "nt" else 0  # CREATE_NO_WINDOW | CREATE_SUSPENDED
        try:
            self._proc = subprocess.Popen(self._command(), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                          creationflags=flags)
        except OSError:
            self._proc = None
            return
        finally:
            if log is not subprocess.DEVNULL:
                log.close()
        if os.name == "nt":  # 先入作业再运行，子进程自动跟着入
            self._job = _Job()
            self._job.adopt(self._proc)
            ctypes.WinDLL("ntdll").NtResumeProcess(ctypes.c_void_p(int(self._proc._handle)))

    def _warm(self) -> None:
        try:
            if not self._health():
                self._spawn()
                for _ in range(240):  # 最多约 60 秒
                    if self._health():
                        break
                    time.sleep(0.25)
            if self._health():
                self._ready = True
        finally:
            self._warming = False

    def warmup(self) -> None:
        """后台预热，不阻塞调用方。"""
        if not self.enabled or self._ready:
            return
        with self._warm_lock:
            if self._warming:
                return
            self._warming = True
        threading.Thread(target=self._warm, daemon=True).start()

    # ------------------------------------------------------------ 判分

    def _completion(self, meaning: str, ans: str) -> float | None:
        def slot(k: int) -> float | None:
            prompt = (f"State:\n标准释义：{meaning}\n学生写的释义：{ans}\n\n"
                      f"Question [choice]: 学生写的释义与标准释义表达的意思是否一致？\nOptions:\n"
                      f"- {_OPT0}\n- {_OPT1}\nJudge each option:\n{_OPT0} ->"
                      + (f"\n{_OPT1} ->" if k else ""))
            resp = self._request("POST", "/completion", {"prompt": prompt, "n_predict": 1, "temperature": 0,
                                                         "n_probs": 32, "cache_prompt": False}, self.timeout)
            yes, no = _yes_no(resp)
            return None if yes is None or no is None else yes - no

        d0 = slot(0)
        d1 = slot(1) if d0 is not None else None
        if d1 is None:
            return None
        z = max(-30.0, min(30.0, (d0 - d1) / TEMPERATURE))
        return 1.0 / (1.0 + math.exp(-z))

    def _service(self, meaning: str, ans: str) -> float | None:
        resp = self._request("POST", "/judge", {"meaning": meaning, "answer": ans}, self.timeout)
        try:
            p = float(json.loads(resp)["p"])
        except (TypeError, ValueError, KeyError):
            return None
        return p if 0 <= p <= 1 else None

    def _systemone(self, meaning: str, ans: str) -> float | None:
        body = {"state": f"标准释义：{meaning}\n学生写的释义：{ans}",
                "questions": {"q": {"type": "choice", "instructions": "学生写的释义与标准释义表达的意思是否一致？",
                                    "criteria": {"一致": "意思相同或非常接近", "不一致": "意思不同或无关"}}}}
        resp = self._request("POST", "/v1/systemone", body, self.timeout)
        try:
            p = float(json.loads(resp)["answers"]["q"]["probabilities"]["一致"])
        except (TypeError, ValueError, KeyError):
            return None
        return p if 0 <= p <= 1 else None

    def p(self, meaning: str, ans: str) -> float:
        """「意思接近」概率 0–1；服务不可用返回 -1。线程安全（同一时刻只发一个请求）。"""
        if not self.enabled:
            return -1.0
        with self._lock:
            if not self._ready:
                if not self._health():
                    self.warmup()
                    return -1.0
                self._ready = True
            for m, a, p in self._cache:
                if m == meaning and a == ans:
                    return p
            if self.api == "judge":
                p = self._service(meaning, ans)
            elif self.api == "systemone":
                p = self._systemone(meaning, ans)
            else:
                p = self._completion(meaning, ans)
            if p is None:
                return -1.0
            self._cache = ([(meaning, ans, p)] + self._cache)[:CACHE]
            return p


def _yes_no(resp: str | None) -> tuple[float | None, float | None]:
    """从 /completion 响应里取 " yes" / " no" 两个 token 的 logprob。"""
    try:
        data = json.loads(resp) if resp else None
    except ValueError:
        return None, None
    found: dict[int, float] = {}

    def walk(x) -> None:
        if isinstance(x, dict):
            if "id" in x and "logprob" in x and x["id"] in (TOKEN_YES, TOKEN_NO) and x["id"] not in found:
                found[x["id"]] = float(x["logprob"])
            for v in x.values():
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)

    walk(data)
    return found.get(TOKEN_YES), found.get(TOKEN_NO)
