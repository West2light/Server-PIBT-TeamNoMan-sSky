# Plan cải tiến EPIBT và Enemy movement cho PIBT TCP

Ngày lập: 2026-06-09
Repo Unity: `F:\DATN`
Scene mục tiêu: `Assets/Scenes/MapF_TankTest_PIBT_TCP.unity`
Server WSL: `/home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky`
Tài liệu nền: `adds/epibt_full.md`

## 1. Trạng thái hiện tại đã kiểm tra

- Unity đang ở nhánh `feature/pibt-tcp-client-server`.
- `adds/epibt_full.md` hiện là tài liệu tổng hợp EPIBT mới, chưa track trong Git tại thời điểm lập plan.
- Unity Editor đang mở `MapF_TankTest_PIBT_TCP`, không compiling, `ready_for_tools=true`.
- Protocol hiện tại chỉ trả action một bước:
  - `ActionDto.action`: `FW`, `CR`, `CCR`, `W`
  - `ActionDto.nextLoc`: cell sau action một bước
- Unity client dùng `Newtonsoft.Json`, nên có thể thêm field optional vào JSON server mà client cũ vẫn bỏ qua.
- Server hiện tại:
  - `PlannerSession::Plan()` gọi `DefaultPlanner::plan(time_limit_ms, actions, &env_)`.
  - `PibtTcpServer.cpp` hard-code `time_limit_ms=90`.
  - `UnityStartKitAdapter` đã validate map, loc, goal, orientation và sanitize `FW` invalid về `W`.
  - `DefaultPlanner` hiện dùng biến global trong namespace `DefaultPlanner`, `causalPIBT()`, `moveCheck()`, heuristic table và traffic flow.
- Unity movement copy hiện tại:
  - `GridEnemyAgentPIBTTcpCopy` nhận action một bước từ server.
  - `FW` đã chuyển sang `HandleMoveWorldDirection(directionToTarget.normalized)`.
  - `CR/CCR` vẫn rotate thủ công bằng `LerpAngle` trong `Update`, sau đó snap committed facing/body rotation.
  - Các vấn đề timeout, pending move, shooting clear action và diagnostics đã được xử lý trên copy route.

## 2. Mục tiêu

1. Cải tiến thuật toán server từ causal PIBT một bước sang EPIBT phù hợp rotation action model.
2. Giữ protocol một bước trong giai đoạn đầu: server có thể chọn operation nhiều action nhưng chỉ trả action đầu tiên cho Unity.
3. Tối ưu Enemy rotation để mượt, ổn định vật lý, không lệch state grid giữa Unity và server.
4. Giữ `DefaultPlanner` làm baseline rollback; EPIBT bật bằng config/feature flag.
5. Có metric rõ để so sánh trước/sau: runtime server, invalid action, timeout, snap distance, snap angle, rotation settle time, throughput/tiến độ tới Eagle slot.

## 3. Nguyên tắc triển khai

- Không sửa trực tiếp file movement gốc cho đến khi copy route ổn định:
  - Giữ `TankMover.cs`, `TankController.cs`, `GridEnemyAgentPIBTTcp.cs` làm baseline.
  - Tiếp tục triển khai trên `TankMoverCopy.cs`, `TankControllerCopy.cs`, `GridEnemyAgentPIBTTcpCopy.cs`.
- Server thêm module EPIBT riêng, không rewrite `DefaultPlanner` trước.
- Không để Unity tự "sửa" action server quá nhiều khi chạy EPIBT strict mode. Các helper như auto-advance/correct bad turn chỉ nên bật ở fallback mode vì chúng có thể phá operation inheritance.
- Collision correctness đặt ở server; Unity chỉ render/exe action được server duyệt.
- Mọi thay đổi phải có log ngắn, grep được:
  - `[EPIBT]`
  - `[EPIBT_OP]`
  - `[PIBT_TCP_ROT]`
  - `[PIBT_TCP_TRACE]`

## 4. Thiết kế server EPIBT giai đoạn đầu

### 4.1. Tạo module planner mới

File dự kiến trong server WSL:

- `default_planner/epibt.h`
- `default_planner/epibt.cpp`
- có thể thêm `default_planner/epibt_types.h` nếu type nhiều.

API đề xuất:

```cpp
namespace EpibtPlanner {
    void reset();
    void initialize(int preprocess_time_limit, SharedEnvironment* env);
    void plan(int time_limit_ms, std::vector<Action>& actions, SharedEnvironment* env);
}
```

Lý do dùng namespace riêng: tránh động vào global state hiện tại của `DefaultPlanner`, giảm rủi ro làm hỏng planner đang chạy được.

### 4.2. Operation model

Mặc định dùng cấu hình từ `adds/epibt_full.md`:

- `op_len = 3`
- revisit limit `L = 10`
- action alphabet: `FW`, `CR`, `CCR`, `W`
- tie-break: ưu tiên `FW > CR/CCR > W`
- timestep đầu: inherited operation = `WWW`
- timestep sau: inherited operation = bỏ action đầu + thêm `W`

Type đề xuất:

```cpp
struct Operation {
    std::array<Action, 3> actions;
    int beta;
};

struct OperationPath {
    std::array<State, 4> states; // state at t0..t3
    std::array<int, 3> from;
    std::array<int, 3> to;
};
```

Với `op_len=3`, có thể precompute candidate operations ngay trong code. Không cần build generic `4^op_len` ở milestone đầu, miễn là vẫn giữ đúng các operation rút gọn theo unique cell sequence.

### 4.3. Reservation table theo horizon 3

Server cần reservation theo thời gian:

- vertex reservation: `(dt, loc) -> agentId`
- edge reservation: `(dt, fromLoc, toLoc) -> agentId` cho `FW`
- `CR/CCR/W` giữ nguyên loc, không tạo edge move.

`getUsed(k, op, P)` trả tập agent conflict:

- vertex conflict ở cùng `dt`.
- edge conflict khi hai agent swap cạnh trong cùng `dt`.
- nếu conflict với hơn 1 agent: bỏ operation theo paper, không priority-inherit.

### 4.4. EPIBT_SELECT_OPERATION

Triển khai sát pseudocode:

1. Sort operations theo `alpha * h + beta`.
2. `visited[k]++`, `hit[k]=true`.
3. Với mỗi operation:
   - build path từ `env->curr_states[k]`.
   - reject nếu path ra map/obstacle.
   - lấy `U = getUsed(path, reservation)`.
   - `U=empty`: accept.
   - `|U|>1`: skip.
   - `|U|=1`: thử recursive push nếu `hit[l]==false`, `visited[l]<L`, priority thấp hơn.
4. Nếu thất bại: fallback về inherited operation.

Priority ban đầu:

- Milestone đầu dùng heuristic distance tới `goalLoc`, agent xa hơn goal có priority cao hơn.
- Có thể tái dùng `DefaultPlanner::get_h()` hoặc heuristic table hiện có nếu tách được sạch.
- Nếu heuristic chưa khởi tạo đủ, fallback Manhattan để tránh crash.

### 4.5. Operation inheritance trong `PlannerSession`

Vì Unity chỉ thực thi action đầu, server phải giữ operation còn lại theo agent qua các `plan_step`.

Đề xuất lưu trong `PlannerSession` hoặc `EpibtPlanner` session state:

- `prev_operations[agentId]`
- `last_timestep`
- reset khi:
  - hello mới.
  - team size thay đổi.
  - agent id count thay đổi.
  - Unity gửi state không khớp với expected state sau action đầu quá nhiều.

Giai đoạn đầu không cần client lưu operation queue. Server trả action đầu của operation đã chọn:

```json
{
  "id": 0,
  "action": "CR",
  "nextLoc": 123,
  "operation": "CR,FW,W",
  "planner": "EPIBT3",
  "opIndex": 0
}
```

Unity cũ sẽ chỉ đọc `id/action/nextLoc`; Unity mới có thể đọc thêm để debug.

### 4.6. Feature flag và rollback

Không hard-code thay thế planner ngay. Thêm một trong các cách bật:

- env var: `PIBT_TCP_PLANNER=epibt`
- command line server: `--planner epibt`
- hoặc field `plannerMode` trong `hello`.

Fallback:

- Nếu planner mode không rõ: dùng `DefaultPlanner`.
- Nếu EPIBT exception: log `[EPIBT] fallback reason=...`, dùng `DefaultPlanner` hoặc toàn bộ `W` tùy mức lỗi.
- `UnityStartKitAdapter::SanitizeAction()` vẫn chạy sau EPIBT như safety net.

## 5. Thiết kế Unity movement/rotation

### 5.1. Vấn đề rotation hiện tại

Hiện `CR/CCR` rotate bằng `LerpAngle` trong `Update`, ghi trực tiếp `Rigidbody2D.rotation`/transform, rồi snap body về committed facing. Cách này dễ tạo cảm giác giật vì:

- physics write không nằm trong `FixedUpdate`.
- rotate completion dùng `Time.deltaTime`, trong khi movement dùng Rigidbody ở `FixedUpdate`.
- snap rotation sau rotate có thể nhảy góc nếu body đã bị steer trong `FW`.
- `FW` world-direction tự xoay body về target, có thể làm visual heading lệch so với orientation server vừa lên kế hoạch.

### 5.2. Thêm strict EPIBT execution mode

Trong `GridEnemyAgentPIBTTcpCopy` thêm flag:

```csharp
public bool strictEpibtActionModel = true;
```

Khi bật:

- `CR/CCR`: chỉ rotate in-place, không auto-advance thành move.
- `W`: không tự `TryAutoAdvanceFromWait()` trừ khi explicit fallback mode.
- `FW`: đi đúng một cell theo `committedFacing`/server `nextLoc`, không tự đổi logical facing.
- Nếu server `FW.nextLoc` không kề đúng hướng `committedFacing`, reject và request plan mới; không sửa ngầm thành target khác.

Mục tiêu: state model của Unity khớp EPIBT paper và server.

### 5.3. Chuyển rotation sang physics-friendly executor

Nên đưa rotation target xuống `TankMoverCopy` hoặc một helper nhỏ để chạy trong `FixedUpdate`:

- `SetRotationTarget(float worldAngle, float maxDegreesPerSecond)`
- `IsRotationComplete(angleToleranceDeg)`
- dùng `Rigidbody2D.MoveRotation()` hoặc set `rb2d.rotation` trong `FixedUpdate`.

Các tham số đề xuất:

- `rotationMaxDegreesPerSecond = 540f` đến `720f`.
- `rotationToleranceDeg = 1.5f` đến `3f`.
- `maxRotationDuration = 0.25f`.
- chỉ snap cuối nếu `abs(deltaAngle) <= rotationToleranceDeg`; nếu lớn hơn thì log `[PIBT_TCP_ROT] snap_angle_large`.

`GridEnemyAgentPIBTTcpCopy.StartRotation()` chỉ set target:

- stop linear velocity.
- set target angle từ committed facing sau `CR/CCR`.
- state `isRotating=true`.

`ContinueRotation()` chỉ poll complete:

- không ghi transform trực tiếp mỗi frame.
- khi complete, update `committedFacing`, clear action, request plan/ready.

### 5.4. Làm rõ rotation trong `FW`

Có hai mode, chọn theo milestone:

**Mode A - strict grid, an toàn cho EPIBT**

- Trước `FW`, body đã được align bởi `CR/CCR`.
- `FW` move từ center cell hiện tại tới center target cell.
- Body rotation giữ theo `committedFacing`; không xoay theo vector target trong lúc đi.
- Nếu target vector lệch nhiều so với committed facing, reject action.

**Mode B - visual smoothing**

- Vẫn move tới target cell center.
- Body có thể rotate nhẹ về direction target nhưng không update committed facing cho đến arrive.
- Chỉ dùng nếu Mode A nhìn quá cứng.

Khuyến nghị triển khai Mode A trước để đúng thuật toán, sau đó tune visual smoothing nhỏ.

### 5.5. Giảm snap khi arrive

Hiện arrive gọi `SnapBodyToCurrentCellCenter()`. Giữ snap nhỏ, nhưng thêm ngưỡng:

- nếu `snapDistance <= 0.1`: snap im lặng.
- nếu `0.1 < snapDistance <= 0.35`: snap và log debug nếu bật trace.
- nếu `snapDistance > 0.35`: log warning, không coi là normal movement.

Sau khi EPIBT/rotation đúng, mục tiêu là `snapDistance` thường xuyên dưới `0.15`.

## 6. Protocol và diagnostics

### 6.1. Field optional mới

Mở rộng `ActionDto` Unity:

```csharp
public string planner;      // "DefaultPlanner" | "EPIBT3"
public string operation;    // "CR,FW,W"
public int    opIndex;      // action đầu đang execute, thường 0
public string debugReason;
```

Không bắt buộc ở milestone đầu, nhưng nên thêm sớm để debug.

### 6.2. Trace request/result

`MapScenarioBootstrapPIBTTcp.BuildRequestTrace()` nên thêm:

- planner hiện tại.
- số agent `rotating`.
- số agent `strictModeReject`.
- average/max snap distance nếu có.

Server `PlannerSession` nên log:

- `planner=EPIBT3`
- `computeMs`
- `revisitCount`
- `acceptedInherited`
- `fallbackInherited`
- `multiConflictSkipped`
- `operation` histogram nếu trace bật.

## 7. Milestones triển khai

### M0 - Baseline freeze và safety gates

Mục tiêu: có số liệu trước khi sửa tiếp.

Việc làm:

1. Chạy scene hiện tại 60 giây với server default.
2. Lưu log Unity và server vào `adds/output/` hoặc `adds/`.
3. Ghi lại:
   - số timeout.
   - số invalid/sanitize.
   - max `snap_distance_large`.
   - số `rotating`.
   - computeMs trung bình/max.
   - quan sát Game view về rotation.

Pass:

- Có log baseline rõ ràng để so sánh.
- Không có compile error.
- Không có `PlanStep exception`.

### M1 - Tối ưu rotation Unity trên copy route

Mục tiêu: sửa vấn đề hiện tại trước khi đổi thuật toán server.

Việc làm:

1. Thêm strict EPIBT execution flag nhưng mặc định có thể off trong lần đầu.
2. Đưa rotation target vào `TankMoverCopy.FixedUpdate()` hoặc helper tương đương.
3. `CR/CCR` không ghi Rigidbody/transform trực tiếp trong `Update`.
4. Thêm metric:
   - `rotateStartAngle`
   - `rotateTargetAngle`
   - `rotateSettleMs`
   - `snapAngle`
5. Disable/guard các auto-correction có thể làm sai strict model.

Pass:

- `CR/CCR` nhìn mượt hơn, không giật/snap lớn.
- `snap_angle_large` không xuất hiện thường xuyên.
- Enemy không drift khỏi cell center.
- Console không warning/error mới.

### M2 - Server EPIBT core, chưa LNS/GG

Mục tiêu: thay causal one-step PIBT bằng EPIBT(3) đúng paper ở mức core.

Việc làm:

1. Tạo `EpibtPlanner` namespace/module.
2. Build candidate operations `op_len=3`.
3. Implement `getPath`, reservation table, collision check vertex/edge.
4. Implement revisit limit `L=10`, hit guard, priority inheritance/backtracking.
5. Implement operation inheritance per agent.
6. Trả action đầu của operation qua protocol hiện tại.
7. Bật bằng feature flag `PIBT_TCP_PLANNER=epibt`.

Pass:

- Server build được `pibt_tcp_server`.
- Với map nhỏ giả lập, không có vertex/edge conflict trong horizon 3.
- ComputeMs cho 6 enemy dưới 90ms, kỳ vọng dưới 10ms.
- Nếu tắt feature flag, DefaultPlanner vẫn chạy như cũ.

### M3 - Unity protocol/diagnostics cho EPIBT

Mục tiêu: Unity thấy rõ server đang trả operation nào và execute strict.

Việc làm:

1. Thêm optional fields vào `ActionDto`.
2. Log operation trong `[PIBT_TCP_TRACE]`.
3. `GridEnemyAgentPIBTTcpCopy` strict mode:
   - `CR/CCR` rotate in-place.
   - `FW` chỉ move khi `nextLoc` đúng với committed facing.
   - reject action sai model thay vì tự sửa.
4. Hiển thị summary:
   - `planner=EPIBT3`
   - `rotating`
   - `fw_pending`
   - `shoot_eagle`
   - `fallbackInherited`

Pass:

- Unity parse result cũ và mới đều không lỗi.
- Log đủ để trace một agent từ operation server tới action Unity.
- Không còn tình trạng client tự chuyển `CR/CCR/W` thành move khi strict mode bật.

### M4 - Gameplay smoke test và tuning

Mục tiêu: xác nhận thuật toán mới và movement mới tốt hơn baseline.

Việc làm:

1. Chạy server:

```bash
cd /home/lenovo/projectY/Server-PIBT-TeamNoMan-sSky
PIBT_TCP_PLANNER=epibt ./build/pibt_tcp_server --host 127.0.0.1 --port 7777
```

2. Chạy Unity scene `MapF_TankTest_PIBT_TCP` 60 giây.
3. So sánh với baseline M0.

Pass bắt buộc:

- Không compile error.
- Không `PlanStep exception`.
- Không timeout spam.
- Server `computeMs < 90ms`, tốt nhất `< 10ms` với 6 enemy.
- Không có `FW` invalid/out-of-bounds/row-wrap/blocked thường xuyên.
- `snapDistance` phần lớn `< 0.15`, không có snap lớn lặp lại.
- Rotation settle ổn định, không giật 90 độ rõ bằng mắt.
- Enemy còn lại tiếp tục nhận plan hoặc đứng do `shoot_eagle` hợp lệ.

### M5 - LNS nhẹ sau EPIBT core

Chỉ làm sau khi M2-M4 pass.

Mục tiêu: tận dụng thời gian còn lại trong budget để cải thiện operation.

Việc làm:

1. Thêm loop LNS theo remaining time.
2. Chọn agent neighborhood nhỏ trước, ví dụ 1 agent hoặc cluster quanh bottleneck.
3. Metric:
   - `sum(priority * opWeight)`
   - hoặc tổng heuristic sau operation.
4. Giới hạn hard stop trước 80ms để chừa margin TCP/serialize.

Pass:

- Không làm computeMs vượt budget.
- Operation quality cải thiện so với EPIBT core trong log A/B.

### M6 - Graph Guidance nếu map còn bottleneck

Chỉ làm nếu EPIBT core + rotation vẫn còn tắc hành lang.

Mục tiêu: giảm congestion dài hạn.

Việc làm:

1. Tính edge guidance offline từ map hoặc dynamic traffic.
2. Dùng guided cost trong heuristic/sort operation.
3. Log heatmap/waiting counters theo cell.

Pass:

- Giảm số frame `W`/blocked ở bottleneck.
- Không làm agent đi vòng quá xa trong map nhỏ.

## 8. Thứ tự ưu tiên khuyến nghị

1. M0 baseline.
2. M1 rotation Unity, vì đây là vấn đề hiện tại còn lại.
3. M2 EPIBT server core, không LNS/GG.
4. M3 protocol diagnostics/strict execution.
5. M4 smoke test + tuning.
6. M5 LNS.
7. M6 Graph Guidance.

Không nên làm LNS/GG trước khi rotation và EPIBT core ổn, vì nếu client còn lệch rotation/state thì metric thuật toán sẽ nhiễu.

## 9. File dự kiến thay đổi

Unity:

- `Assets/Scripts/PibtTcp/PibtTcpProtocol.cs`
- `Assets/Scripts/PibtTcp/PibtTcpSessionState.cs`
- `Assets/Scripts/PibtTcp/GridEnemyAgentPIBTTcpCopy.cs`
- `Assets/Scripts/TankMoverCopy.cs`
- `Assets/Scripts/TankControllerCopy.cs` nếu cần API rotate.
- `Assets/Scripts/PibtTcp/MapScenarioBootstrapPIBTTcp.cs`
- `Assets/Data/TankData/EnemyTankMovementDataCopy.asset`

Server WSL:

- `src/PlannerSession.h`
- `src/PlannerSession.cpp`
- `src/PibtTcpServer.cpp`
- `src/UnityStartKitAdapter.h`
- `src/UnityStartKitAdapter.cpp`
- `default_planner/epibt.h`
- `default_planner/epibt.cpp`
- `CMakeLists.txt`

Docs/evidence:

- `adds/epibt_full.md`
- `adds/PLAN_EPIBT_ENEMY_MOVEMENT_IMPROVEMENT_2026-06-09.md`
- `adds/output/` cho log baseline/smoke nếu cần.

## 10. Rủi ro và cách giảm rủi ro

| Rủi ro | Giảm rủi ro |
|---|---|
| EPIBT server state inheritance lệch với Unity vì agent chưa tới cell đúng lúc | chỉ request plan khi all agents ready; reset inherited op nếu observed state lệch expected |
| Client auto-correction phá action model | strict mode tắt auto-advance/correct-turn khi chạy EPIBT |
| Rotation mượt nhưng làm chậm timestep | đặt max rotation duration và log settle time |
| Operation collision check sai edge conflict | viết test map nhỏ swap cạnh 2 agent |
| EPIBT global state làm hỏng DefaultPlanner | namespace/module riêng, feature flag, fallback |
| ComputeMs vượt budget khi thêm LNS | M2 chưa LNS; M5 hard stop theo remaining time |

## 11. Gate hoàn thành plan

Plan được coi là sẵn sàng để triển khai khi:

- Có baseline M0.
- M1 pass rotation visual/physics trên copy route.
- M2 server build và trả action đầu từ EPIBT operation.
- M4 smoke test chứng minh:
  - không regress các lỗi đã sửa.
  - rotation Enemy mượt hơn.
  - planner không timeout.
  - Enemy còn lại tiếp tục có kế hoạch hoặc đứng bắn hợp lệ.

