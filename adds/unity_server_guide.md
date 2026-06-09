# Hướng dẫn setup và chạy `pibt_tcp_server` cho Unity

Tài liệu này mô tả cách chuẩn bị môi trường, build project, khởi động server TCP, và cách Unity giao tiếp với server hiện tại của start-kit.

## 1. Tổng quan

Project đã được mở rộng thêm một executable riêng cho Unity là `pibt_tcp_server`.

Server này:

- Lắng nghe TCP trên `host:port` do bạn truyền vào.
- Nhận protocol JSON theo từng dòng, tức là mỗi message kết thúc bằng ký tự xuống dòng `\n`.
- Xử lý các message chính: `hello`, `plan_step`, `shutdown`.
- Dùng `DefaultPlanner` bên trong để sinh hành động cho từng agent.

Mặc định:

- `host = 127.0.0.1`
- `port = 7777`

## 2. Yêu cầu hệ thống

Trên Linux, các dependency tối thiểu để build phần server là:

- CMake 3.16 trở lên
- Compiler C++17
- Boost headers/libs, đặc biệt `program_options`, `system`, `filesystem`, `log`, `log_setup`

Trên Ubuntu/Debian, cài nhanh bằng:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake libboost-all-dev
```

Nếu bạn chỉ chạy server Unity, không cần bật Python binding. Build hiện tại dùng `-DPYTHON=false`.

## 3. Setup project

Clone repository như bình thường rồi vào thư mục project:

```bash
git clone <your_repo>
cd Server-PIBT-TeamNoMan-sSky
```

Kiểm tra nhanh rằng bạn đang ở root của project, nơi có `CMakeLists.txt` và `compile.sh`.

## 4. Build

### Cách khuyến nghị cho Unity server

Nếu chỉ cần chạy Unity server, hãy build riêng target `pibt_tcp_server`:

```bash
mkdir -p build
cmake -B build -S . -DPYTHON=false -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pibt_tcp_server --parallel 2
```

Lệnh này chỉ build binary cần cho Unity và bỏ qua executable `lifelong`.

Nếu máy đủ RAM/CPU, có thể tăng số job:

```bash
cmake --build build --target pibt_tcp_server --parallel "$(nproc)"
```

Nếu build bị chậm hoặc có dấu hiệu đứng ở bước link, giảm lại `--parallel 1` hoặc `--parallel 2`.

### Dùng script có sẵn

Script trong repo:

```bash
./compile.sh
```

Script này tạo thư mục `build/`, cấu hình CMake với `-DPYTHON=false`, rồi build toàn bộ project, bao gồm cả `lifelong` và `pibt_tcp_server`.

Với Unity server, cách này không bắt buộc. Nếu log dừng lâu ở khoảng `94%`, thường đó là bước `Linking CXX executable lifelong`, không phải `pibt_tcp_server`. Binary Unity server thường đã được tạo trước đó tại `./build/pibt_tcp_server`.

### Build thủ công

Nếu muốn build toàn bộ project, dùng các lệnh sau:

```bash
mkdir -p build
cmake -B build ./ -DPYTHON=false -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

Sau khi build xong, binary server nằm tại:

```bash
./build/pibt_tcp_server
```

Nếu sau khi build target riêng mà vẫn không thấy binary, chạy:

```bash
cmake --build build --target pibt_tcp_server --parallel 1 --verbose
```

Log verbose sẽ cho biết chính xác file hoặc bước link nào đang lỗi.

## 5. Chạy server

Xem help:

```bash
./build/pibt_tcp_server --help
```

Chạy với cấu hình mặc định:

```bash
./build/pibt_tcp_server
```

Chạy với host/port cụ thể:

```bash
./build/pibt_tcp_server --host 0.0.0.0 --port 7777
```

Khuyến nghị:

- Nếu Unity chạy cùng máy với server, dùng `127.0.0.1`.
- Nếu Unity chạy trong Docker, trên VM, hoặc máy khác, bind `0.0.0.0` và mở port tương ứng.

Server sẽ log ra console các trạng thái như:

- client connected / disconnected
- hello session
- plan_step request
- timeout hoặc lỗi protocol

## 6. Cách Unity gọi server

Server dùng protocol JSON theo dòng. Mỗi message phải là một JSON object hợp lệ và kết thúc bằng `\n`.

Thứ tự làm việc chuẩn:

1. Unity mở kết nối TCP tới `host:port`.
2. Unity gửi `hello` để khởi tạo session.
3. Unity gửi các `plan_step` theo từng timestep.
4. Khi kết thúc, Unity gửi `shutdown` và đóng kết nối.

### 6.1. `hello`

Message `hello` phải chứa:

- `type = "hello"`
- `sessionId`
- `teamSize`
- `map`

Ví dụ:

```json
{
  "type": "hello",
  "sessionId": "demo-session-001",
  "teamSize": 2,
  "map": {
    "width": 5,
    "height": 5,
    "symbols": "........................."
  }
}
```

Lưu ý về `map`:

- `symbols` là chuỗi row-major, chiều dài phải bằng `width * height`.
- `.` là ô đi được.
- `@` hoặc ký tự khác được xem là vật cản.

Server sẽ trả về `hello_ack` với trạng thái `ok` hoặc `error`.

### 6.2. `plan_step`

Sau khi `hello` thành công, Unity gửi `plan_step` cho mỗi bước lập kế hoạch.

Ví dụ:

```json
{
  "type": "plan_step",
  "sessionId": "demo-session-001",
  "requestId": 1,
  "timestep": 0,
  "agents": [
    {"id": 0, "loc": 0, "orientation": 0, "goalLoc": 24},
    {"id": 1, "loc": 4, "orientation": 2, "goalLoc": 20}
  ]
}
```

Mỗi agent cần có:

- `id`
- `loc`
- `orientation`
- `goalLoc`

Quy ước hiện tại:

- `loc = y * width + x`
- `orientation = 0` là east, `1` là south, `2` là west, `3` là north
- `agents` nên được gửi theo thứ tự tăng dần của `id`

Server trả về `plan_result` với các trường chính:

- `actions`: danh sách hành động cho từng agent
- `computeMs`: thời gian tính toán thực tế
- `timeout`: `true` nếu vượt time budget
- `errors`: danh sách lỗi nếu có

Hành động có thể là:

- `FW`
- `CR`
- `CCR`
- `W`

### 6.3. `shutdown`

Khi kết thúc session, Unity gửi:

```json
{
  "type": "shutdown",
  "sessionId": "demo-session-001"
}
```

Server sẽ trả về `shutdown_ack` rồi đóng session hiện tại.

## 7. Giới hạn và lưu ý khi tích hợp Unity

- Server hiện tại xử lý theo kiểu tuần tự: mỗi process chỉ phục vụ một client tại một thời điểm.
- Nếu gửi `plan_step` trước khi gửi `hello`, server sẽ trả lỗi `no active session; send hello first`.
- Nếu `map`, `loc`, `goalLoc`, hoặc `orientation` không hợp lệ, server sẽ từ chối session hoặc trả lỗi tương ứng.
- Planner đang dùng budget mặc định nội bộ là `preprocess = 3000 ms` và `plan_step = 90 ms`.

Nếu Unity và server nằm ở hai máy khác nhau, hãy đảm bảo firewall/port forwarding cho cổng bạn đang bind.

## 8. Kiểm tra nhanh

1. Build xong, chạy `./build/pibt_tcp_server --help` để xác nhận binary tồn tại.
2. Chạy server với `--host 0.0.0.0 --port 7777` nếu cần nhận kết nối từ bên ngoài.
3. Kết nối từ Unity và gửi `hello` trước.
4. Theo dõi console để xem server nhận `hello`, `plan_step`, và `shutdown` đúng thứ tự.

## 9. Ghi chú nguồn

Phần server Unity hiện tại được ghép từ các file chính:

- `src/tcp_server_main.cpp`
- `src/PibtTcpServer.cpp`
- `src/PlannerSession.cpp`
- `src/UnityStartKitAdapter.cpp`

Binary được khai báo trong `CMakeLists.txt` với tên `pibt_tcp_server`.
