"""
FastAPI Proxy Layer – HF Space ↔ pibt_tcp_server
=================================================
Kiến trúc:
  - Khi container khởi động, pibt_tcp_server được spawn ở background (TCP 7777)
  - Client WebGL (Unity / browser tại luminx.io.vn) gọi HTTPS REST (port 7860)
  - FastAPI dịch sang JSON-over-TCP → pibt_tcp_server → trả kết quả về HTTP

Tham khảo protocol: unity_server_guide.md §6
  - hello      : khởi tạo session (map, teamSize)
  - plan_step  : yêu cầu lập kế hoạch 1 timestep
  - shutdown   : kết thúc session

CORS: cho phép origin https://luminx.io.vn (Unity WebGL client)
      và * cho môi trường dev / Swagger UI.

Concurrency: asyncio.Lock đảm bảo chỉ 1 request tương tác với
             pibt_tcp_server tại 1 thời điểm (server sequential §7).

Chú ý: HF Spaces chỉ expose port 7860 (HTTPS tự động).
        Port 7777 (TCP) là nội bộ container, không expose ra ngoài.
"""

from __future__ import annotations

import asyncio
import json
import os
import socket
import subprocess
import time
import uuid
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, JSONResponse
from pydantic import BaseModel, Field

# ─────────────────────────────────────────────────────────────
# Cấu hình
# ─────────────────────────────────────────────────────────────
TCP_HOST = "127.0.0.1"
TCP_PORT = int(os.environ.get("PIBT_TCP_PORT", "7777"))
PIBT_BIN = os.environ.get("PIBT_BIN", "/usr/local/bin/pibt_tcp_server")
SERVER_PROC: subprocess.Popen | None = None

# Lock đảm bảo chỉ 1 request TCP tại 1 thời điểm
# (pibt_tcp_server xử lý tuần tự theo unity_server_guide §7)
_TCP_LOCK: asyncio.Lock | None = None


# ─────────────────────────────────────────────────────────────
# Lifecycle: spawn / teardown pibt_tcp_server
# ─────────────────────────────────────────────────────────────
def _wait_tcp_ready(host: str, port: int, timeout: float = 15.0) -> bool:
    """Poll cho đến khi pibt_tcp_server lắng nghe TCP."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.3)
    return False


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Spawn pibt_tcp_server khi container khởi động."""
    global SERVER_PROC, _TCP_LOCK
    _TCP_LOCK = asyncio.Lock()

    if os.path.isfile(PIBT_BIN) and os.access(PIBT_BIN, os.X_OK):
        SERVER_PROC = subprocess.Popen(
            [PIBT_BIN, "--host", TCP_HOST, "--port", str(TCP_PORT)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        ready = _wait_tcp_ready(TCP_HOST, TCP_PORT, timeout=15.0)
        if ready:
            print(f"[startup] pibt_tcp_server sẵn sàng tại {TCP_HOST}:{TCP_PORT}")
        else:
            print("[startup] WARNING: pibt_tcp_server chưa lắng nghe sau 15s")
    else:
        print(f"[startup] WARNING: Binary không tìm thấy: {PIBT_BIN}")

    yield  # ← app đang chạy

    # Teardown
    if SERVER_PROC and SERVER_PROC.poll() is None:
        SERVER_PROC.terminate()
        try:
            SERVER_PROC.wait(timeout=5)
        except subprocess.TimeoutExpired:
            SERVER_PROC.kill()
        print("[shutdown] pibt_tcp_server đã dừng")


# ─────────────────────────────────────────────────────────────
# FastAPI app
# ─────────────────────────────────────────────────────────────
app = FastAPI(
    title="MAPF TCP Proxy – Server-PIBT TeamNoMan's Sky",
    description=(
        "HTTP proxy cho pibt_tcp_server. "
        "Dịch REST request sang JSON-over-TCP protocol (unity_server_guide §6). "
        "Client: Unity WebGL tại https://luminx.io.vn"
    ),
    version="2.1.0",
    lifespan=lifespan,
)

# ─────────────────────────────────────────────────────────────
# CORS Middleware – BẮT BUỘC cho Unity WebGL
# Browser block cross-origin request nếu không có header này.
# Client: https://luminx.io.vn (Unity WebGL / TankMAPF)
# ─────────────────────────────────────────────────────────────
ALLOWED_ORIGINS = [
    "https://luminx.io.vn",       # Unity WebGL client (production)
    "http://luminx.io.vn",        # Fallback HTTP
    "http://localhost",            # Local dev Unity Editor / browser
    "http://localhost:3000",       # Local dev web frontend
    "http://127.0.0.1",
    "null",                        # file:// origin (Unity WebGL local build)
]

# Đọc thêm origin từ env var (tuỳ chỉnh không cần rebuild image)
_extra = os.environ.get("CORS_EXTRA_ORIGINS", "")
if _extra:
    ALLOWED_ORIGINS.extend([o.strip() for o in _extra.split(",") if o.strip()])

app.add_middleware(
    CORSMiddleware,
    allow_origins=ALLOWED_ORIGINS,
    allow_credentials=True,
    allow_methods=["GET", "POST", "OPTIONS"],   # OPTIONS cho preflight
    allow_headers=["*"],
    max_age=600,   # Cache preflight 10 phút
)


# ─────────────────────────────────────────────────────────────
# TCP helpers (blocking I/O – gọi trong asyncio executor)
# ─────────────────────────────────────────────────────────────
def _tcp_session_sync(messages: list[dict], timeout: float = 10.0) -> list[dict]:
    """
    Gửi nhiều message trong cùng 1 TCP connection (blocking).
    Phải được gọi qua asyncio.to_thread() để không block event loop.
    """
    responses: list[dict] = []
    with socket.create_connection((TCP_HOST, TCP_PORT), timeout=timeout) as sock:
        for msg in messages:
            raw = (json.dumps(msg) + "\n").encode()
            sock.sendall(raw)
            sock.settimeout(timeout)
            buf = b""
            while b"\n" not in buf:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
            line, buf = buf.split(b"\n", 1)
            responses.append(json.loads(line.strip()))
    return responses


def _tcp_send_recv_sync(message: dict, timeout: float = 5.0) -> dict:
    """Gửi 1 message, nhận 1 response (blocking)."""
    raw = (json.dumps(message) + "\n").encode()
    with socket.create_connection((TCP_HOST, TCP_PORT), timeout=timeout) as sock:
        sock.sendall(raw)
        sock.settimeout(timeout)
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.strip())


async def _tcp_session(messages: list[dict], timeout: float = 10.0) -> list[dict]:
    """Async wrapper: acquire lock → chạy blocking I/O trong thread pool."""
    assert _TCP_LOCK is not None, "Lock chưa được khởi tạo"
    async with _TCP_LOCK:
        return await asyncio.to_thread(_tcp_session_sync, messages, timeout)


async def _tcp_send_recv(message: dict, timeout: float = 5.0) -> dict:
    """Async wrapper single message."""
    assert _TCP_LOCK is not None
    async with _TCP_LOCK:
        return await asyncio.to_thread(_tcp_send_recv_sync, message, timeout)


# ─────────────────────────────────────────────────────────────
# Pydantic models – theo unity_server_guide.md §6
# ─────────────────────────────────────────────────────────────
class MapDef(BaseModel):
    width: int
    height: int
    symbols: str = Field(
        ...,
        description="Row-major chuỗi độ dài width*height. '.'=đi được, '@'=vật cản"
    )


class AgentState(BaseModel):
    id: int
    loc: int         = Field(..., description="loc = y * width + x")
    orientation: int = Field(..., description="0=East 1=South 2=West 3=North")
    goalLoc: int


class PlanRequest(BaseModel):
    """Full session trong 1 HTTP call: hello → plan_step → shutdown."""
    team_size: int
    map: MapDef
    agents: list[AgentState]
    timestep: int = 0
    time_budget_ms: int = Field(90, description="Budget tính toán ms (mặc định 90ms)")


class HelloRequest(BaseModel):
    session_id: str = Field(default_factory=lambda: str(uuid.uuid4())[:8])
    team_size: int
    map: MapDef


class PlanStepRequest(BaseModel):
    session_id: str
    request_id: int = 1
    timestep: int = 0
    agents: list[AgentState]


class ShutdownRequest(BaseModel):
    session_id: str


# ─────────────────────────────────────────────────────────────
# Endpoints
# ─────────────────────────────────────────────────────────────
@app.get("/", response_class=HTMLResponse)
async def root():
    """Landing page."""
    return """
    <html><head><title>MAPF TCP Proxy</title></head>
    <body style='font-family:sans-serif;max-width:760px;margin:40px auto;color:#1a1a2e'>
      <h1>🤖 MAPF TCP Proxy – Server-PIBT</h1>
      <p>HTTP proxy cho <code>pibt_tcp_server</code> (JSON-over-TCP).</p>
      <p>🎮 Client: <a href='https://luminx.io.vn'>Unity WebGL – TankMAPF</a></p>
      <table border=1 cellpadding=8 style='border-collapse:collapse'>
        <tr><th>Endpoint</th><th>Mô tả</th></tr>
        <tr><td><code>GET  /health</code></td><td>Kiểm tra binary + TCP server + CORS</td></tr>
        <tr><td><code>POST /plan</code></td><td>Full session (hello→plan_step→shutdown)</td></tr>
        <tr><td><code>POST /tcp/hello</code></td><td>Gửi hello thủ công</td></tr>
        <tr><td><code>POST /tcp/plan_step</code></td><td>Gửi plan_step thủ công</td></tr>
        <tr><td><code>POST /tcp/shutdown</code></td><td>Gửi shutdown thủ công</td></tr>
        <tr><td><a href='/docs'>/docs</a></td><td>Swagger UI</td></tr>
      </table>
    </body></html>
    """


@app.get("/health")
async def health():
    """Kiểm tra binary + TCP server + cấu hình CORS."""
    bin_ok = os.path.isfile(PIBT_BIN) and os.access(PIBT_BIN, os.X_OK)
    proc_ok = SERVER_PROC is not None and SERVER_PROC.poll() is None
    tcp_ok = False
    try:
        with socket.create_connection((TCP_HOST, TCP_PORT), timeout=1):
            tcp_ok = True
    except OSError:
        pass
    return {
        "status": "ok" if (bin_ok and proc_ok and tcp_ok) else "degraded",
        "binary_exists": bin_ok,
        "process_alive": proc_ok,
        "tcp_reachable": tcp_ok,
        "tcp_endpoint": f"{TCP_HOST}:{TCP_PORT}",
        "cors_origins": ALLOWED_ORIGINS,
        "version": "2.1.0",
    }


@app.post("/plan")
async def plan_full_session(req: PlanRequest):
    """
    Full session trong 1 HTTP call: hello → plan_step → shutdown.

    Được thiết kế cho Unity WebGL (luminx.io.vn) gọi mỗi timestep.
    Lock đảm bảo tuần tự hoá với pibt_tcp_server (single-client §7).
    """
    session_id = str(uuid.uuid4())[:12]

    hello_msg = {
        "type": "hello",
        "sessionId": session_id,
        "teamSize": req.team_size,
        "map": req.map.model_dump(),
    }
    plan_msg = {
        "type": "plan_step",
        "sessionId": session_id,
        "requestId": 1,
        "timestep": req.timestep,
        "agents": [a.model_dump() for a in req.agents],
    }
    shutdown_msg = {
        "type": "shutdown",
        "sessionId": session_id,
    }

    try:
        responses = await _tcp_session(
            [hello_msg, plan_msg, shutdown_msg], timeout=15.0
        )
    except OSError as e:
        raise HTTPException(
            status_code=503,
            detail=f"Không kết nối được pibt_tcp_server ({TCP_HOST}:{TCP_PORT}): {e}",
        )
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

    result: dict[str, Any] = {"session_id": session_id}
    for resp in responses:
        t = resp.get("type", "")
        if t == "hello_ack":
            result["hello_ack"] = resp
        elif t == "plan_result":
            result["plan_result"] = resp
        elif t == "shutdown_ack":
            result["shutdown_ack"] = resp
        else:
            result.setdefault("other", []).append(resp)

    return JSONResponse(result)


# ── Endpoints thủ công (từng message riêng) ──────────────────

@app.post("/tcp/hello")
async def tcp_hello(req: HelloRequest):
    """Gửi hello và nhận hello_ack."""
    msg = {
        "type": "hello",
        "sessionId": req.session_id,
        "teamSize": req.team_size,
        "map": req.map.model_dump(),
    }
    try:
        resp = await _tcp_send_recv(msg)
    except OSError as e:
        raise HTTPException(503, detail=str(e))
    return JSONResponse(resp)


@app.post("/tcp/plan_step")
async def tcp_plan_step(req: PlanStepRequest):
    """Gửi plan_step và nhận plan_result."""
    msg = {
        "type": "plan_step",
        "sessionId": req.session_id,
        "requestId": req.request_id,
        "timestep": req.timestep,
        "agents": [a.model_dump() for a in req.agents],
    }
    try:
        resp = await _tcp_send_recv(msg, timeout=10.0)
    except OSError as e:
        raise HTTPException(503, detail=str(e))
    return JSONResponse(resp)


@app.post("/tcp/shutdown")
async def tcp_shutdown(req: ShutdownRequest):
    """Gửi shutdown và nhận shutdown_ack."""
    msg = {"type": "shutdown", "sessionId": req.session_id}
    try:
        resp = await _tcp_send_recv(msg)
    except OSError as e:
        raise HTTPException(503, detail=str(e))
    return JSONResponse(resp)
