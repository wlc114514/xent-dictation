# -*- coding: utf-8 -*-
"""单词默写（Xent 版）的本地判分服务：Jev-Style-2B-Decision-v3 + 作者官方运行时（jev-score-v2）。

2B 要块注意力，通用 llama-server 跑不了，所以由这个小服务调官方运行时，Xent 用 HTTP 问它：
  GET  /health                               → {"status":"ok"}（模型加载好之后才回 ok）
  POST /judge {"meaning": 标准释义, "answer": 学生写的}  → {"p": 映射后的「意思一致」概率, "p_raw": 模型原始概率}

--pass-at 0.55（默认）：在对数几率上整体平移，把原始概率 0.55 映射成 0.9，让过线标准和 0.8B 一样写 0.9。
单调映射，不改变谁高谁低，等于把原始分的过线定在 0.55。0.55 是在 170 组人工标注的释义对上扫出来的：
0.3–0.6 准确率都在 90% 左右，0.55 时错写法判成对最少（4/80）且只多误伤 1 个近义写法；0.7 起误伤明显变多。

只监听 127.0.0.1。--parent-pid 给了的话，那个进程（Xent）退出后本服务也退出；
本服务退出时 jev-score-v2 读到输入结束自己退出，不留后台进程。

用法：python -I jev2b_server.py --port 8088 [--parent-pid PID] [--gguf 模型] [--n-ctx 4096]
"""
import argparse
import ctypes
import json
import math
import subprocess
import sys
import threading
import traceback
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))      # 用 -I 启动时脚本目录不在 sys.path 里
import jev_style_decision_gguf as J  # noqa: E402

QUESTION = "学生写的释义与标准释义表达的意思是否一致？"
OPTIONS = {"一致": "意思相同或非常接近", "不一致": "意思不同或无关"}
LOG = HERE / "server.log"


def log(msg):
    try:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(msg.rstrip() + "\n")
    except OSError:
        pass


def hide_scorer_window():
    """Xent 用无窗口方式拉起本服务；官方运行时再拉 jev-score-v2 时也别弹黑框"""
    if sys.platform != "win32":
        return
    orig = subprocess.Popen

    class Popen(orig):
        def __init__(self, *a, **kw):
            kw.setdefault("creationflags", subprocess.CREATE_NO_WINDOW)
            super().__init__(*a, **kw)

    J.subprocess.Popen = Popen


def watch_parent(pid):
    """父进程（Xent）退出 → 本服务退出"""
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x00100000, False, pid)    # SYNCHRONIZE
    if not h:
        log(f"parent {pid} not found, exiting")
        J_model_close_and_exit()
    k32.WaitForSingleObject(h, 0xFFFFFFFF)
    log(f"parent {pid} exited")
    J_model_close_and_exit()


model = None
lock = threading.Lock()
shift = 0.0   # 加在对数几率上的偏移，见 --pass-at


def J_model_close_and_exit():
    try:
        if model is not None:
            model.close()
    finally:
        import os
        os._exit(0)


def judge(meaning, answer):
    state = f"标准释义：{meaning}\n学生写的释义：{answer}"
    with lock:
        r = model.decide(state, QUESTION, options=OPTIONS)
    probs = r["probabilities"]
    return float(probs["一致"] if isinstance(probs, dict) else probs[0])


def remap(p):
    """原始概率在对数几率上平移 shift；0 和 1 原样返回"""
    if p <= 0.0 or p >= 1.0:
        return p
    z = math.log(p / (1.0 - p)) + shift
    return 1.0 / (1.0 + math.exp(-z))


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send(200, {"status": "ok", "model": J.MODEL_NAME})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/judge":
            return self._send(404, {"error": "not found"})
        try:
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf-8"))
            meaning, answer = str(req["meaning"]).strip(), str(req["answer"]).strip()
            if not meaning or not answer:
                return self._send(400, {"error": "meaning / answer 不能为空"})
            p = judge(meaning, answer)
            self._send(200, {"p": remap(p), "p_raw": p})
        except Exception as e:     # 判分出错不让服务挂掉；Xent 收到错误按字面判
            log(traceback.format_exc())
            self._send(500, {"error": str(e)})

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--parent-pid", type=int, default=0)
    ap.add_argument("--gguf", default=str(HERE / "Jev-Style-2B-Decision-v3-Q8_0.gguf"))
    ap.add_argument("--n-ctx", type=int, default=4096, help="判释义的输入不到 100 个词元，4096 足够，比默认 32768 省内存")
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--pass-at", type=float, default=0.55, help="原始概率多少映射成 0.9（0.9 = 不映射）")
    args = ap.parse_args()

    global model, shift
    if not 0.0 < args.pass_at < 1.0:
        ap.error("--pass-at 要在 0 和 1 之间")
    shift = math.log(0.9 / 0.1) - math.log(args.pass_at / (1.0 - args.pass_at))
    hide_scorer_window()
    J.LLAMA_N_CTX = args.n_ctx
    if args.parent_pid:
        threading.Thread(target=watch_parent, args=(args.parent_pid,), daemon=True).start()
    try:
        model = J.JevStyleDecisionGGUF(HERE, gguf=args.gguf, binary=str(HERE / "build" / "jev-score-v2.exe"),
                                       threads=args.threads or None)
    except Exception:
        log("model load failed:\n" + traceback.format_exc())
        raise
    server = HTTPServer(("127.0.0.1", args.port), Handler)   # 模型加载好才开始监听：/health 通了就能判
    log(f"ready on 127.0.0.1:{args.port} ({Path(args.gguf).name}, n_ctx {args.n_ctx}, {args.pass_at} → 0.9)")
    try:
        server.serve_forever()
    finally:
        model.close()


if __name__ == "__main__":
    main()
