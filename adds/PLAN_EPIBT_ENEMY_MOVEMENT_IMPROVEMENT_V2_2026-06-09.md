# Plan V2: Milestone triển khai EPIBT và Enemy movement

Ngày lập: 2026-06-09
Repo Unity Client: `F:\DATN`
Repo Server WSL: `/home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky`
Scene mục tiêu: `Assets/Scenes/MapF_TankTest_PIBT_TCP.unity`
Tài liệu nền: `adds/epibt_full.md`
Plan V1: `adds/PLAN_EPIBT_ENEMY_MOVEMENT_IMPROVEMENT_2026-06-09.md`

## 1. Mục tiêu của V2

Bản V2 tách rõ từng milestone theo nơi implement:

- Client Unity: movement, rotation, strict action model, protocol DTO, diagnostics.
- Server WSL: EPIBT core, feature flag, planner session state, response metadata.
- Shared verification: baseline log, smoke test, A/B so sánh với DefaultPlanner.

Nguyên tắc chính: sửa rotation/movement Client trước để state execution ổn định, sau đó mới bật EPIBT Server. Nếu Client còn tự sửa action hoặc rotation bị giật/snap lớn, kết quả EPIBT sẽ nhiễu và khó debug.

## 2. Snapshot hiện tại

Client Unity hiện có:

- `Assets/Scripts/PibtTcp/PibtTcpProtocol.cs`: DTO chỉ có action một bước.
- `Assets/Scripts/PibtTcp/PibtTcpSessionState.cs`: lưu `ActionDto` mới nhất theo agent.
- `Assets/Scripts/PibtTcp/PibtTcpClient.cs`: gửi `hello`, `plan_step`; parse `PlanResult`.
- `Assets/Scripts/PibtTcp/MapScenarioBootstrapPIBTTcp.cs`: gửi state, nhận result, attach copy agent.
- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`: execute `FW/CR/CCR/W` trên copy route.
- `Assets/Scripts/TankMoverCopy.cs`: move vật lý cho enemy copy route.
- `Assets/Scripts/TankControllerCopy.cs`: wrapper gọi mover/turret.

Server WSL hiện có:

- `src/PibtTcpServer.cpp`: TCP line JSON server; `plan_step` gọi `session->Plan(..., 90)`.
- `src/PlannerSession.cpp`: gọi `DefaultPlanner::plan(...)`, sanitize action, trả `ActionDto`.
- `src/UnityStartKitAdapter.cpp`: map/agent adapter, `NextLoc`, `SanitizeAction`.
- `default_planner/planner.cpp`: current DefaultPlanner global state + causal PIBT.
- `default_planner/pibt.cpp`: causal PIBT một bước, `getAction`, `moveCheck`.
- `CMakeLists.txt`: build `pibt_tcp_server` từ `src/*.cpp` và `default_planner/*.cpp`.

## 3. Milestone Overview

| Milestone | Implement ở đâu | Mục tiêu | File chính |
|---|---|---|---|
| M0 | Client + Server | Implement logging infrastructure và freeze baseline | Client log sink + Server file logger + `adds/output/*` |
| M1 | Client Unity | Tối ưu Enemy rotation copy route | `TankMoverCopy.cs`, `GridEnemyAgentPIBTTcpCopy.cs`, `TankControllerCopy.cs` |
| M2 | Client Unity | Strict action model + diagnostics cho EPIBT | `GridEnemyAgentPIBTTcpCopy.cs`, `MapScenarioBootstrapPIBTTcp.cs`, `PibtTcpProtocol.cs` |
| M3 | Server WSL | Thêm EPIBT core planner module | `default_planner/epibt.*`, `CMakeLists.txt` |
| M4 | Server WSL | Tích hợp EPIBT vào TCP session với feature flag | `PlannerSession.*`, `PibtTcpServer.cpp`, `UnityStartKitAdapter.*` |
| M5 | Client + Server | Protocol metadata và trace operation | Client DTO/session/bootstrap + Server result JSON |
| M6 | Client + Server | Smoke test 60s và tuning movement/EPIBT | logs, screenshots, minor tuning |
| M7 | Server WSL | LNS/GG sau khi core ổn định | `default_planner/epibt_lns.*`, guidance code |

## 4. M0 - Logging infrastructure và baseline freeze

### Implement ở đâu

Client Unity và Server WSL.

### File thay đổi

Client Unity:

- `Assets/Scripts/PibtTcp/PibtTcpFileLogger.cs`
- `Assets/Scripts/PibtTcp/MapScenarioBootstrapPIBTTcp.cs`
- `Assets/Scripts/PibtTcp/PibtTcpClient.cs`
- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`

Server WSL:

- `src/FileLogger.h`
- `src/FileLogger.cpp`
- `src/PibtTcpServer.cpp`
- `src/PlannerSession.cpp`
- `CMakeLists.txt`

Evidence output:

- `adds/output/epibt_m0_unity_console_2026-06-09.txt`
- `adds/output/epibt_m0_server_defaultplanner_2026-06-09.txt`
- `adds/output/epibt_m0_summary_2026-06-09.md`

Neu chua co `adds/output/`, tao folder trong repo Unity. M0 khong duoc tinh la pass neu chi co log hien tren Console ma khong co file output tren dia.

### Nội dung thực hiện

1. Implement code ghi log file o Client Unity.

De xuat cho `PibtTcpFileLogger.cs`:

- Dung `Application.logMessageReceivedThreaded` de mirror `Debug.Log`, `Debug.LogWarning`, `Debug.LogError` ra file.
- Chi filter nhom can thiet:
  - `[PIBT_TCP_TRACE]`
  - `[PibtTcpClient]`
  - `[MapScenarioBootstrapPIBTTcp]`
  - `[PIBT_TCP_ROT]`
  - `PlanStep exception`
- File output:
  - `F:/DATN/adds/output/epibt_m0_unity_console_<timestamp>.txt`
- Logger phai:
  - tao thu muc neu chua co
  - append thread-safe
  - flush dinh ky hoac flush moi line
  - co `StartCapture(sessionId)` / `StopCapture()`

2. Implement code ghi log file o Server WSL.

De xuat cho `FileLogger.h/.cpp`:

- `FileLogger::Initialize(path)`
- `FileLogger::Info(line)`
- `FileLogger::Warn(line)`
- `FileLogger::Error(line)`
- mutex de ghi an toan
- flush moi line

Noi can gan:

- `PibtTcpServer.cpp`: `hello`, `plan_step`, `shutdown`, socket error
- `PlannerSession.cpp`: `initialize`, `plan`, `sanitize`, `timeout`, `exception`

File output:

- `/home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky/adds/output/epibt_m0_server_defaultplanner_<timestamp>.txt`

3. Sau khi logging infrastructure co san, chay server default:

```bash
cd /home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky
./build/pibt_tcp_server --host 127.0.0.1 --port 7777
```

4. Chay Unity scene `MapF_TankTest_PIBT_TCP` khoang 60 giay.
5. Tao `epibt_m0_summary_2026-06-09.md` tu cac file log da ghi.
6. Ghi lai so lieu:
   - `computeMs` trung bình/max.
   - timeout count.
   - `PlanStep exception`.
   - `sanitize` count ở server.
   - `snap_distance_large`.
   - số agent `rotating`, `fw_pending`, `shoot_eagle`.
   - nhận xét trực quan về rotation giật/snap.

### Gate pass

- Co file log Unity duoc ghi tu code.
- Co file log Server duoc ghi tu code.
- Co baseline summary du de so sanh M6.
- Console không có compile error.
- Server không crash.

## 5. M1 - Client Unity: tối ưu Enemy rotation copy route

### Implement ở đâu

Repo Unity Client: `F:\DATN`.

### File thay đổi

- `Assets/Scripts/TankMoverCopy.cs`
- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`
- `Assets/Scripts/TankControllerCopy.cs` nếu cần thêm wrapper API rotate.
- Không sửa `Assets/Scripts/TankMover.cs`.
- Không sửa `Assets/Scripts/TankController.cs`.
- Không sửa `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcp.cs`.

### Nội dung code

Trong `TankMoverCopy.cs`:

- Thêm state rotation target:
  - `private bool hasRotationTarget;`
  - `private float rotationTargetAngle;`
  - `private float rotationMaxDegreesPerSecond;`
  - `private float rotationToleranceDeg;`
- Thêm public API:
  - `BeginRotateToAngle(float targetAngle, float maxDegPerSecond, float toleranceDeg)`
  - `CancelRotationTarget()`
  - `IsRotationTargetComplete()`
  - `GetRotationDeltaToTarget()`
- Trong `FixedUpdate()`, nếu đang rotate target:
  - set `linearVelocity = Vector2.zero`.
  - set `angularVelocity = 0`.
  - dùng `Mathf.MoveTowardsAngle` hoặc `Rigidbody2D.MoveRotation`.
  - complete khi delta <= tolerance.

Trong `GridEnemyAgentPIBTTcpCopy.cs`:

- `StartRotation(float deltaDeg)` không ghi `Rigidbody2D.rotation` trực tiếp nữa.
- Tính orientation mục tiêu trước, rồi gọi `TankMoverCopy.BeginRotateToAngle(...)`.
- `ContinueRotation()` chỉ poll `TankMoverCopy.IsRotationTargetComplete()`.
- Khi complete:
  - update `committedFacing`.
  - stop velocity.
  - log `rotate_done`.
- Thêm metric:
  - `rotationStartedAt`.
  - `rotationStartAngle`.
  - `rotationTargetAngle`.
  - `rotationSettleMs`.
  - `snapAngleAtComplete`.

Trong `TankControllerCopy.cs`:

- Chỉ thêm wrapper nếu muốn agent không gọi mover trực tiếp:
  - `BeginRotateBodyToAngle(...)`
  - `IsBodyRotationComplete()`

### Diagnostics cần thêm

Log grep được:

```text
[PIBT_TCP_ROT] agent=... phase=start action=CR startAngle=... targetAngle=...
[PIBT_TCP_ROT] agent=... phase=done settleMs=... snapAngle=...
[PIBT_TCP_ROT] agent=... phase=timeout delta=...
```

### Gate pass

- Unity compile không error.
- `CR/CCR` không còn ghi rotation thủ công mỗi `Update`.
- Enemy rotate mượt hơn, không snap 90 độ rõ bằng mắt.
- `snapAngle` khi complete thường <= 3 độ.
- Không regress các fix đã có: shooting clear pending, timeout, recover, copy route.

## 6. M2 - Client Unity: strict action model cho EPIBT

### Implement ở đâu

Repo Unity Client: `F:\DATN`.

### File thay đổi

- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`
- `Assets/Scripts/PibtTcp/MapScenarioBootstrapPIBTTcp.cs`
- `Assets/Scripts/PibtTcp/PibtTcpProtocol.cs`
- Có thể chỉnh `Assets/Data/TankData/EnemyTankMovementDataCopy.asset` nếu cần tune speed/rotation sau M1.

### Nội dung code

Trong `GridEnemyAgentPIBTTcpCopy.cs`:

- Thêm flag:

```csharp
public bool strictEpibtActionModel = false;
```

Mặc định để `false` cho rollout an toàn. Khi server EPIBT ổn thì bật trong scene/bootstrap.

- Khi `strictEpibtActionModel=true`:
  - `CR/CCR` chỉ rotate in-place.
  - `W` không gọi `TryAutoAdvanceFromWait()`.
  - `CR/CCR` không gọi `TryCorrectBadTurnTarget(...)`.
  - `FW` chỉ accept nếu `nextLoc` đúng một cell phía trước theo `committedFacing`.
  - Reject action sai bằng log + `RequestImmediatePlanStep()`, không tự sửa target.

- Tách helper:
  - `IsForwardTargetConsistentWithCommittedFacing(ActionDto dto, out string reason)`
  - `RejectStrictAction(string reason, ActionDto dto)`

Trong `MapScenarioBootstrapPIBTTcp.cs`:

- Thêm inspector field:
  - `public bool strictEpibtActionModel = false;`
- Khi attach `GridEnemyAgentPIBTTcpCopy`, truyền flag này vào agent.
- `BuildMotionSummary()` thêm count:
  - `rotating`
  - `strictReject`
  - `rotateTimeout`

Trong `PibtTcpProtocol.cs`:

- Có thể thêm optional fields sớm để client không cần đổi lại ở M5:

```csharp
public string planner;
public string operation;
public int opIndex;
public string debugReason;
```

### Gate pass

- Khi strict off, behavior không đổi so với M1.
- Khi strict on, client không tự sửa `CR/CCR/W` thành move.
- Server default vẫn chạy được vì fields mới optional.
- Log cho biết rõ reject là strict reject hay old recovery reject.

## 7. M3 - Server WSL: EPIBT core planner module

### Implement ở đâu

Repo Server WSL: `/home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky`.

### File thay đổi

- `default_planner/epibt.h`
- `default_planner/epibt.cpp`
- `default_planner/epibt_types.h` nếu type nhiều.
- `CMakeLists.txt`

Không sửa sâu `default_planner/planner.cpp` ở milestone này, trừ khi cần reuse heuristic với include sạch.

### Nội dung code

Tạo namespace riêng:

```cpp
namespace EpibtPlanner {
    void reset();
    void initialize(int preprocess_time_limit, SharedEnvironment* env);
    void plan(int time_limit_ms, std::vector<Action>& actions, SharedEnvironment* env);
}
```

Type chính:

```cpp
struct Operation {
    std::array<Action, 3> actions;
    int beta;
};

struct OperationPath {
    std::array<State, 4> states;
};

struct EpibtStats {
    int revisitCount;
    int fallbackInherited;
    int acceptedInherited;
    int multiConflictSkipped;
};
```

Core cần implement:

- `BuildOperationsLen3()`.
- `ApplyAction(State s, Action a, SharedEnvironment* env)`.
- `BuildPath(State start, Operation op, ...)`.
- `IsPathValid(...)`.
- `GetUsedAgents(path, reservation)`.
- `ReservePath(agentId, path)`.
- `RemovePath(agentId, path)`.
- `SelectOperation(agentId, inheritedPriority)`.
- Operation inheritance:
  - `prevOperation[agentId]`.
  - inherited op = bỏ action đầu + thêm `W`.

Collision check bắt buộc:

- Vertex conflict theo từng `dt`.
- Edge swap conflict theo từng `dt`.
- Operation conflict với hơn 1 agent thì skip.

Priority:

- Milestone đầu dùng distance heuristic theo `goal_locations[i].front().first`.
- Nếu chưa reuse được `DefaultPlanner::get_h`, dùng Manhattan + obstacle-agnostic fallback.
- Sau khi core chạy, thay bằng heuristic table để tốt hơn.

### CMake

Nếu `CMakeLists.txt` glob `default_planner/*.cpp` đã include file mới thì chỉ cần đảm bảo header path đúng. Nếu không, thêm `default_planner/epibt.cpp` vào target `pibt_tcp_server`.

### Gate pass

- `cmake --build build --target pibt_tcp_server` pass.
- Có thể gọi `EpibtPlanner::initialize(...)` độc lập.
- Unit/smoke test nhỏ trong server cho:
  - 2 agent swap edge bị detect.
  - vertex conflict bị detect.
  - inherited op fallback hoạt động.
  - action đầu trả về hợp lệ.

## 8. M4 - Server WSL: tích hợp EPIBT vào TCP session

### Implement ở đâu

Repo Server WSL: `/home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky`.

### File thay đổi

- `src/PlannerSession.h`
- `src/PlannerSession.cpp`
- `src/PibtTcpServer.cpp`
- `src/UnityStartKitAdapter.h`
- `src/UnityStartKitAdapter.cpp`
- Có thể chỉnh `src/tcp_server_main.cpp` nếu thêm CLI flag.

### Nội dung code

Trong `PlannerSession.h`:

- Thêm enum planner mode:

```cpp
enum class PlannerMode {
    DefaultPlanner,
    Epibt
};
```

- Thêm field:
  - `PlannerMode planner_mode_;`
  - `std::string planner_name_;`

Trong `PlannerSession::Initialize(...)`:

- Đọc planner mode từ:
  - env var `PIBT_TCP_PLANNER`.
  - optional JSON `plannerMode`.
  - default là `DefaultPlanner`.
- Nếu `epibt`:
  - gọi `EpibtPlanner::reset()`.
  - gọi `EpibtPlanner::initialize(...)`.
- Nếu default:
  - giữ `DefaultPlanner::initialize(...)`.

Trong `PlannerSession::Plan(...)`:

- Nếu default:
  - giữ logic hiện tại.
- Nếu epibt:
  - gọi `EpibtPlanner::plan(...)`.
  - vẫn chạy `UnityAdapter::SanitizeAction(...)`.
  - response thêm metadata `planner="EPIBT3"`.

Trong `PibtTcpServer.cpp`:

- `BuildHelloAck()` trả `planner` đúng mode.
- Log plan step thêm planner:

```text
[pibt_tcp_server] plan_step ... planner=EPIBT3
```

Trong `UnityStartKitAdapter.*`:

- Thêm helper nếu cần:
  - `NextState(State s, Action a, int cols)`.
  - `IsActionModelConsistent(...)`.
- Không phá `SanitizeAction(...)` hiện có.

### Gate pass

- Chạy default mode vẫn giống trước:

```bash
./build/pibt_tcp_server --host 127.0.0.1 --port 7777
```

- Chạy EPIBT mode:

```bash
PIBT_TCP_PLANNER=epibt ./build/pibt_tcp_server --host 127.0.0.1 --port 7777
```

- `hello_ack.planner` phản ánh đúng mode.
- Nếu EPIBT lỗi, server không crash; có fallback/log rõ.

## 9. M5 - Client + Server: protocol metadata và operation trace

### Implement ở đâu

Client Unity và Server WSL.

### File thay đổi Client

- `Assets/Scripts/PibtTcp/PibtTcpProtocol.cs`
- `Assets/Scripts/PibtTcp/PibtTcpSessionState.cs`
- `Assets/Scripts/PibtTcp/PibtTcpClient.cs`
- `Assets/Scripts/PibtTcp/MapScenarioBootstrapPIBTTcp.cs`
- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`

### File thay đổi Server

- `src/PlannerSession.cpp`
- `default_planner/epibt.h`
- `default_planner/epibt.cpp`

### Nội dung code Client

Trong `ActionDto`:

```csharp
public string planner;
public string operation;
public int opIndex;
public string debugReason;
```

Trong `PlanResult`:

```csharp
public string planner;
public int opLen;
public int revisitLimit;
public int fallbackInherited;
public int multiConflictSkipped;
```

Trong `PibtTcpSessionState`:

- Lưu latest planner metadata:
  - `PlannerName`
  - `OpLen`
  - `FallbackInherited`
  - `MultiConflictSkipped`

Trong `MapScenarioBootstrapPIBTTcp`:

- `BuildResultTrace()` in operation:
  - `agent=0 action=CR op=CR,FW,W`.
- `BuildMotionSummary()` thêm:
  - `rotating`
  - `strictReject`
  - `epibtFallback`

Trong `GridEnemyAgentPIBTTcpCopy`:

- `TraceAgentExecution(...)` in thêm:
  - `planner`
  - `operation`
  - `opIndex`
  - `strictEpibtActionModel`

### Nội dung code Server

Trong `PlannerSession.cpp` response:

- Top-level:
  - `planner`
  - `opLen`
  - `revisitLimit`
  - `fallbackInherited`
  - `multiConflictSkipped`
- Per action:
  - `planner`
  - `operation`
  - `opIndex`
  - `debugReason`

### Gate pass

- Client cũ vẫn parse được nếu server không gửi metadata.
- Client mới parse được cả default và EPIBT.
- Trace đủ để debug 1 agent từ operation server đến movement Unity.

## 10. M6 - Client + Server: smoke test, tuning, evidence

### Implement ở đâu

Client Unity và Server WSL.

### File thay đổi

Không bắt buộc sửa code nếu M1-M5 đã pass. Có thể tune:

- `Assets/Data/TankData/EnemyTankMovementDataCopy.asset`
- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`
- `Assets/Scripts/TankMoverCopy.cs`
- `default_planner/epibt.cpp`

Evidence:

- `adds/output/epibt_m6_unity_console_2026-06-09.txt`
- `adds/output/epibt_m6_server_epibt_2026-06-09.txt`
- `adds/output/epibt_m6_summary_2026-06-09.md`
- screenshot nếu cần: `Assets/Screenshots/epibt_m6_*.png`

### Nội dung thực hiện

1. Build server.
2. Chạy server EPIBT:

```bash
cd /home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky
PIBT_TCP_PLANNER=epibt ./build/pibt_tcp_server --host 127.0.0.1 --port 7777
```

3. Bật strict mode trong Unity scene/bootstrap.
4. Chạy `MapF_TankTest_PIBT_TCP` 60 giây.
5. So sánh M6 với M0.

### Gate pass

- Không compile error.
- Không `PlanStep exception`.
- Không timeout spam.
- `computeMs < 90ms`, mục tiêu `< 10ms` với 6 enemy.
- `sanitize` gần 0 trong EPIBT mode.
- `snapDistance` phần lớn `< 0.15`.
- `snapAngle` phần lớn `< 3 độ`.
- Enemy đứng im chỉ khi `shoot_eagle` hoặc `W` hợp lệ từ operation.
- Không còn nhóm enemy bị treo vì pending action vô hạn.

## 11. M7 - Server WSL: LNS và Graph Guidance sau core

### Implement ở đâu

Repo Server WSL.

### File thay đổi

- `default_planner/epibt.cpp`
- `default_planner/epibt.h`
- `default_planner/epibt_lns.h`
- `default_planner/epibt_lns.cpp`
- `default_planner/epibt_guidance.h`
- `default_planner/epibt_guidance.cpp`
- `CMakeLists.txt` nếu không dùng glob.

### Nội dung code

LNS nhẹ:

- Chạy sau EPIBT core nếu còn time budget.
- Hard stop trước 80ms.
- Neighborhood nhỏ trước: 1 agent hoặc agent quanh conflict.
- Metric:
  - `sum(priority * operationWeight)`.
  - giảm `fallbackInherited`.
  - giảm wait không cần thiết.

Graph Guidance:

- Chỉ bật nếu M6 còn bottleneck.
- Tính directed edge cost.
- Dùng guided cost trong heuristic/sort operation.
- Log heatmap wait/cell congestion.

### Gate pass

- Không làm `computeMs` vượt budget.
- Throughput/progress tốt hơn EPIBT core.
- Có thể tắt bằng feature flag:
  - `PIBT_TCP_EPIBT_LNS=0`
  - `PIBT_TCP_EPIBT_GG=0`

## 12. Thứ tự triển khai khuyến nghị

1. M0 baseline.
2. M1 Client rotation.
3. M2 Client strict action model.
4. M3 Server EPIBT core.
5. M4 Server TCP integration.
6. M5 metadata trace.
7. M6 smoke test/tuning.
8. M7 LNS/GG.

Lý do M1-M2 đi trước M3-M4: EPIBT dựa vào action model có rotation. Nếu Client vẫn tự auto-correct hoặc rotate bằng snap, server đúng thuật toán nhưng biểu hiện trong Unity vẫn có thể sai.

## 13. Checklist file theo nơi implement

### Client Unity

| File | Milestone | Vai trò |
|---|---|---|
| `Assets/Scripts/TankMoverCopy.cs` | M1, M6 | Physics rotation target, move tuning |
| `Assets/Scripts/TankControllerCopy.cs` | M1 | Optional wrapper rotate API |
| `Assets/Scripts/PibtTcp/PibtTcpFileLogger.cs` | M0 | File logger cho Unity console/trace |
| `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs` | M1, M2, M5, M6 | Execute action, strict mode, rotation diagnostics |
| `Assets/Scripts/PibtTcp/MapScenarioBootstrapPIBTTcp.cs` | M0, M2, M5 | Wire file logger, expose strict flag, trace summary |
| `Assets/Scripts/PibtTcp/PibtTcpProtocol.cs` | M2, M5 | Optional protocol fields |
| `Assets/Scripts/PibtTcp/PibtTcpSessionState.cs` | M5 | Store planner metadata |
| `Assets/Scripts/PibtTcp/PibtTcpClient.cs` | M0, M5 | Emit network diagnostics and planner metadata |
| `Assets/Data/TankData/EnemyTankMovementDataCopy.asset` | M6 | Tune speed/rotation after smoke |

### Server WSL

| File | Milestone | Vai trò |
|---|---|---|
| `default_planner/epibt.h` | M3, M5, M7 | EPIBT public API/stats |
| `default_planner/epibt.cpp` | M3, M5, M7 | EPIBT core implementation |
| `default_planner/epibt_types.h` | M3 | Optional shared structs |
| `src/FileLogger.h` | M0 | File logger interface for server runtime |
| `src/FileLogger.cpp` | M0 | File logger implementation for server runtime |
| `src/PlannerSession.h` | M4 | Planner mode/session fields |
| `src/PlannerSession.cpp` | M0, M4, M5 | File log planning events, select planner, build response metadata |
| `src/PibtTcpServer.cpp` | M0, M4 | File log server lifecycle, planner mode in hello/log |
| `src/tcp_server_main.cpp` | M4 | Optional CLI planner flag |
| `src/UnityStartKitAdapter.h` | M4 | Optional helper declarations |
| `src/UnityStartKitAdapter.cpp` | M4 | NextState/action consistency helpers |
| `CMakeLists.txt` | M0, M3, M7 | Include new source files if needed |

### Evidence/docs

| File | Milestone | Vai trò |
|---|---|---|
| `adds/epibt_full.md` | All | Paper summary/reference |
| `adds/PLAN_EPIBT_ENEMY_MOVEMENT_IMPROVEMENT_2026-06-09.md` | All | Plan V1 |
| `adds/PLAN_EPIBT_ENEMY_MOVEMENT_IMPROVEMENT_V2_2026-06-09.md` | All | Milestone implementation plan |
| `adds/output/epibt_m0_*` | M0 | Baseline evidence |
| `adds/output/epibt_m6_*` | M6 | EPIBT smoke evidence |

## 14. Definition of Done

Plan V2 được coi là hoàn thành triển khai khi:

- M1-M2: Client rotation mượt hơn và strict mode có thể bật/tắt rõ.
- M3-M4: Server build được EPIBT core, bật bằng feature flag, default planner vẫn rollback được.
- M5: Client/Server trace được operation EPIBT.
- M6: Smoke test 60 giây pass, không regress các lỗi movement đã sửa.
- M7: Chỉ làm nếu core còn bottleneck; không bắt buộc cho bản EPIBT movement improvement đầu tiên.
