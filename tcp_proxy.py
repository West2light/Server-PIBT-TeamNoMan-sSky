import asyncio
import json
import subprocess
import socket
import time
import os

PIBT_BIN = os.environ.get("PIBT_BIN", "/var/www/Server-PIBT-TeamNoMan-sSky/build/pibt_tcp_server")
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 7777

def get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def spawn_pibt_server():
    port = get_free_port()
    proc = subprocess.Popen([PIBT_BIN, "--host", "127.0.0.1", "--port", str(port)])
    
    # Wait for the C++ server to be ready
    for _ in range(50):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return proc, port
        except (OSError, ConnectionRefusedError):
            time.sleep(0.1)
            
    proc.terminate()
    raise RuntimeError("Failed to start PIBT C++ server subprocess")

async def handle_tcp_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    proc = None
    addr = writer.get_extra_info('peername')
    print(f"[{addr}] New TCP connection received")
    try:
        # Read the first line which should be the hello message
        hello_line = await reader.readline()
        if not hello_line:
            print(f"[{addr}] Client disconnected before sending hello")
            return

        try:
            req = json.loads(hello_line.decode('utf-8'))
            if req.get('type') != 'hello':
                print(f"[{addr}] Invalid initial message type, expected hello")
                return
            session_id = req.get('sessionId', 'unknown')
            print(f"[{addr}] Hello received for session: {session_id}")
        except json.JSONDecodeError:
            print(f"[{addr}] Invalid JSON received")
            return

        # Spawn a dedicated C++ server
        print(f"[{addr}] Spawning dedicated C++ server...")
        proc, port = await asyncio.to_thread(spawn_pibt_server)
        print(f"[{addr}] C++ server spawned on internal port {port}")

        # Connect to the C++ server
        server_reader, server_writer = await asyncio.open_connection("127.0.0.1", port)

        # Forward the hello message
        server_writer.write(hello_line)
        await server_writer.drain()

        # Bi-directional forwarding
        async def forward(src: asyncio.StreamReader, dst: asyncio.StreamWriter, direction: str):
            try:
                while True:
                    data = await src.read(65536)
                    if not data:
                        break
                    dst.write(data)
                    await dst.drain()
            except Exception as e:
                print(f"[{addr}] Forward {direction} error: {e}")
            finally:
                dst.close()

        task1 = asyncio.create_task(forward(reader, server_writer, "client->server"))
        task2 = asyncio.create_task(forward(server_reader, writer, "server->client"))

        await asyncio.gather(task1, task2)

    except Exception as e:
        print(f"[{addr}] TCP Proxy handler error: {e}")
    finally:
        print(f"[{addr}] Closing connection and cleaning up subprocess...")
        writer.close()
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()

async def main():
    server = await asyncio.start_server(handle_tcp_client, LISTEN_HOST, LISTEN_PORT)
    addr = server.sockets[0].getsockname()
    print(f'Starting PIBT Multi-Process TCP Proxy on {addr}')
    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("TCP Proxy stopped manually.")
