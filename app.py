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
import threading
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

PIBT_SESSION_TTL_SECONDS = int(os.environ.get("PIBT_SESSION_TTL_SECONDS", "300"))


class PibtRelayConnection:
    def __init__(self, tcp_socket: socket.socket, process: subprocess.Popen, port: int):
        self.socket = tcp_socket
        self.process = process
        self.port = port
        self.buffer = bytearray()
        self.lock = threading.Lock()
        self.last_used = time.time()

    def exchange(self, payload: str, timeout: float) -> dict[str, Any]:
        with self.lock:
            self.last_used = time.time()
            self.socket.settimeout(timeout)
            self.socket.sendall(payload.encode("utf-8") + b"\n")

            while b"\n" not in self.buffer:
                chunk = self.socket.recv(65536)
                if not chunk:
                    raise ConnectionError("PIBT server closed the TCP connection")
                self.buffer.extend(chunk)
                if len(self.buffer) > 4 * 1024 * 1024:
                    raise ValueError("PIBT response exceeded 4 MiB")

            line, _, remainder = self.buffer.partition(b"\n")
            self.buffer = bytearray(remainder)
            return json.loads(line.rstrip(b"\r").decode("utf-8"))

    def close(self) -> None:
        try:
            self.socket.close()
        except OSError:
            pass
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()


PIBT_CONNECTIONS: dict[str, PibtRelayConnection] = {}
PIBT_CONNECTIONS_LOCK = threading.Lock()


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
    version="2.2.0",
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


def _remove_pibt_connection(session_id: str) -> None:
    with PIBT_CONNECTIONS_LOCK:
        connection = PIBT_CONNECTIONS.pop(session_id, None)
    if connection is not None:
        connection.close()


def _prune_pibt_connections(max_idle_seconds: int | None = None) -> int:
    idle_limit = PIBT_SESSION_TTL_SECONDS if max_idle_seconds is None else max_idle_seconds
    cutoff = time.time() - idle_limit
    with PIBT_CONNECTIONS_LOCK:
        expired = [
            session_id
            for session_id, connection in PIBT_CONNECTIONS.items()
            if connection.last_used < cutoff
        ]
    for session_id in expired:
        _remove_pibt_connection(session_id)
    return len(expired)


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((TCP_HOST, 0))
        return s.getsockname()[1]

def _connect_new_pibt_session() -> tuple[socket.socket, subprocess.Popen, int]:
    port = _find_free_port()
    proc = subprocess.Popen(
        [PIBT_BIN, "--host", TCP_HOST, "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    ready = _wait_tcp_ready(TCP_HOST, port, timeout=15.0)
    if not ready:
        proc.terminate()
        proc.kill()
        raise ConnectionError(f"PIBT server failed to bind on port {port}")
    sock = socket.create_connection((TCP_HOST, port), timeout=10.0)
    return sock, proc, port


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


class ResetRequest(BaseModel):
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
    pruned = _prune_pibt_connections()
    bin_ok = os.path.isfile(PIBT_BIN) and os.access(PIBT_BIN, os.X_OK)
    return {
        "status": "ok" if bin_ok else "degraded",
        "binary_exists": bin_ok,
        "cors_origins": ALLOWED_ORIGINS,
        "active_sessions": len(PIBT_CONNECTIONS),
        "pruned_sessions": pruned,
        "version": "2.2.0",
    }


@app.get("/api/sessions/pibt/healthz")
async def pibt_healthz():
    pruned = _prune_pibt_connections()
    return {
        "ok": True,
        "relay": "pibt-tcp-multiprocess",
        "active_sessions": len(PIBT_CONNECTIONS),
        "pruned_sessions": pruned,
        "version": "2.3.0",
    }


@app.post("/api/sessions/pibt/hello")
async def pibt_session_hello(req: HelloRequest):
    session_id = req.session_id.strip()
    if not session_id:
        raise HTTPException(400, detail="session_id is required")

    _prune_pibt_connections()
    _remove_pibt_connection(session_id)

    msg = {
        "type": "hello",
        "sessionId": session_id,
        "teamSize": req.team_size,
        "map": req.map.model_dump(),
    }

    connection: PibtRelayConnection | None = None
    try:
        tcp_socket, proc, port = await asyncio.to_thread(_connect_new_pibt_session)
        connection = PibtRelayConnection(tcp_socket, proc, port)
        resp = await asyncio.to_thread(connection.exchange, json.dumps(msg), 70.0)
        if resp.get("type") != "hello_ack":
            connection.close()
            raise HTTPException(502, detail=resp)
        with PIBT_CONNECTIONS_LOCK:
            PIBT_CONNECTIONS[session_id] = connection
        return JSONResponse(resp)
    except HTTPException:
        raise
    except (OSError, ValueError, ConnectionError) as exc:
        if connection is not None:
            connection.close()
        raise HTTPException(502, detail=f"PIBT relay hello failed: {exc}")


@app.post("/api/sessions/pibt/plan-step")
async def pibt_session_plan_step(req: PlanStepRequest):
    session_id = req.session_id.strip()
    with PIBT_CONNECTIONS_LOCK:
        connection = PIBT_CONNECTIONS.get(session_id)
    if connection is None:
        raise HTTPException(404, detail="PIBT relay session was not found or expired")

    msg = {
        "type": "plan_step",
        "sessionId": session_id,
        "requestId": req.request_id,
        "timestep": req.timestep,
        "agents": [a.model_dump() for a in req.agents],
    }
    try:
        resp = await asyncio.to_thread(connection.exchange, json.dumps(msg), 20.0)
        return JSONResponse(resp)
    except (OSError, ValueError, ConnectionError) as exc:
        _remove_pibt_connection(session_id)
        raise HTTPException(502, detail=f"PIBT relay plan-step failed: {exc}")


@app.post("/api/sessions/pibt/reset")
async def pibt_session_reset(req: ResetRequest):
    session_id = req.session_id.strip()
    with PIBT_CONNECTIONS_LOCK:
        connection = PIBT_CONNECTIONS.get(session_id)
    if connection is None:
        raise HTTPException(404, detail="PIBT relay session was not found or expired")

    msg = {
        "type": "reset",
        "sessionId": session_id,
    }
    try:
        resp = await asyncio.to_thread(connection.exchange, json.dumps(msg), 5.0)
        return JSONResponse(resp)
    except (OSError, ValueError, ConnectionError) as exc:
        _remove_pibt_connection(session_id)
        raise HTTPException(502, detail=f"PIBT relay reset failed: {exc}")


@app.post("/api/sessions/pibt/shutdown")
async def pibt_session_shutdown(req: ShutdownRequest):
    session_id = req.session_id.strip()
    with PIBT_CONNECTIONS_LOCK:
        connection = PIBT_CONNECTIONS.get(session_id)
    if connection is None:
        raise HTTPException(404, detail="PIBT relay session was not found or expired")

    msg = {
        "type": "shutdown",
        "sessionId": session_id,
    }
    try:
        resp = await asyncio.to_thread(connection.exchange, json.dumps(msg), 5.0)
        return JSONResponse(resp)
    except (OSError, ValueError, ConnectionError) as exc:
        raise HTTPException(502, detail=f"PIBT relay shutdown failed: {exc}")
    finally:
        _remove_pibt_connection(session_id)





