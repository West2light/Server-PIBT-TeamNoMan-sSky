# 🎮 Hướng Dẫn Tích Hợp Client Unity WebGL với HF Space

Tài liệu này hướng dẫn cách cấu hình và viết lại phần kết nối mạng của client **TankMAPF (Unity WebGL)** để giao tiếp với server PIBT đang chạy trên Hugging Face Spaces.

---

## 🚨 Tại sao lỗi `Connect failed: Success` xảy ra?

Trong console của trình duyệt, bạn thấy lỗi:
```
[PIBTTcpClient] Connect failed: Success
[PIBT_TCP] Could not connect to PIBT server at 110.172.28.110:7777.
```
**Nguyên nhân:**
Client Unity hiện tại đang sử dụng class `PIBTTcpClient` để cố gắng mở một kết nối **TCP Socket thuần (Raw TCP)**. Tuy nhiên, khi build ra **WebGL** chạy trên trình duyệt, các chính sách bảo mật (Sandbox) của mọi trình duyệt (Chrome, Firefox, Safari...) **nghiêm cấm hoàn toàn việc sử dụng Raw TCP Sockets**. Trình duyệt sẽ tự động block kết nối này.

---

## 🏗️ Kiến Trúc Mới Cho WebGL

Để giải quyết vấn đề này, chúng ta đã dựng một **HTTP REST Proxy** bằng FastAPI trên Hugging Face.

Client WebGL thay vì mở TCP Socket, sẽ sử dụng **HTTPS (UnityWebRequest)** để gửi toàn bộ dữ liệu của 1 lượt (Timestep) đến endpoint API của Hugging Face. Server Hugging Face sẽ làm nhiệm vụ kết nối TCP nội bộ với PIBT, lấy kết quả và trả về cho Unity.

### 🌐 Thông số kết nối mới

- **URL Endpoint duy nhất:** `https://west2light-server-pibt.hf.space/plan`
- **Giao thức:** HTTPS (POST)
- **Content-Type:** `application/json`

---

## 🛠️ Hướng Dẫn Cập Nhật Script C# Trong Unity

Bạn cần thay thế script quản lý TCP cũ bằng script sử dụng `UnityWebRequest` dưới đây.

### 1. Chuẩn bị các class dữ liệu (Data Models)

Tạo các class Serialize để chuyển đổi sang JSON gửi lên server:

```csharp
using System;
using System.Collections.Generic;

[Serializable]
public class MapDef {
    public int width;
    public int height;
    public string symbols;
}

[Serializable]
public class AgentState {
    public int id;
    public int loc;
    public int orientation; // 0=East, 1=South, 2=West, 3=North
    public int goalLoc;
}

[Serializable]
public class PlanRequest {
    public int team_size;
    public MapDef map;
    public List<AgentState> agents;
    public int timestep;
}

// ---- Class dùng để hứng Response từ Server ----
[Serializable]
public class PlanResultData {
    public string type;
    public List<string> actions;
    public int computeMs;
}

[Serializable]
public class PlanResponse {
    public string session_id;
    public PlanResultData plan_result;
}
```

### 2. Viết lại hàm gọi Server bằng Coroutine (`UnityWebRequest`)

Thay vì dùng `PIBTTcpClient.Connect()`, bạn sử dụng Coroutine sau mỗi khi cần lập kế hoạch cho 1 timestep:

```csharp
using System.Collections;
using UnityEngine;
using UnityEngine.Networking;
using System.Text;

public class PIBTWebClient : MonoBehaviour
{
    // Địa chỉ mới của Hugging Face Space
    private string apiUrl = "https://west2light-server-pibt.hf.space/plan";

    public void RequestPlan(PlanRequest requestData)
    {
        StartCoroutine(PostPlanRequest(requestData));
    }

    private IEnumerator PostPlanRequest(PlanRequest requestData)
    {
        // 1. Chuyển đổi dữ liệu sang JSON
        string jsonData = JsonUtility.ToJson(requestData);
        byte[] bodyRaw = Encoding.UTF8.GetBytes(jsonData);

        // 2. Tạo UnityWebRequest
        using (UnityWebRequest request = new UnityWebRequest(apiUrl, "POST"))
        {
            request.uploadHandler = new UploadHandlerRaw(bodyRaw);
            request.downloadHandler = new DownloadHandlerBuffer();
            request.SetRequestHeader("Content-Type", "application/json");

            // 3. Chờ phản hồi từ Server
            yield return request.SendWebRequest();

            if (request.result == UnityWebRequest.Result.ConnectionError || 
                request.result == UnityWebRequest.Result.ProtocolError)
            {
                Debug.LogError($"[PIBT_HTTP] Lỗi kết nối: {request.error}");
                Debug.LogError($"Response Code: {request.responseCode}, Message: {request.downloadHandler.text}");
            }
            else
            {
                // 4. Xử lý dữ liệu trả về thành công
                string jsonResponse = request.downloadHandler.text;
                Debug.Log($"[PIBT_HTTP] Phản hồi: {jsonResponse}");

                PlanResponse response = JsonUtility.FromJson<PlanResponse>(jsonResponse);
                
                if (response != null && response.plan_result != null)
                {
                    List<string> actions = response.plan_result.actions;
                    // TODO: Truyền List<string> hành động này ("FW", "CR", "CCR", "W")
                    // vào logic di chuyển Tank của bạn
                    ProcessActions(actions);
                }
            }
        }
    }

    private void ProcessActions(List<string> actions)
    {
        for(int i = 0; i < actions.Count; i++)
        {
            Debug.Log($"Agent {i} thực hiện hành động: {actions[i]}");
        }
    }
}
```

### 3. Cách gọi hàm khi Game đang chạy

Ở mỗi vòng lặp `timestep` của Game, bạn gom thông tin bản đồ và tất cả các Tank lại rồi ném vào cấu trúc `PlanRequest`:

```csharp
PlanRequest req = new PlanRequest {
    team_size = 2,
    timestep = currentTimestep,
    map = new MapDef {
        width = 5,
        height = 5,
        symbols = "........................." // Bản đồ hiện tại
    },
    agents = new List<AgentState> {
        new AgentState { id = 0, loc = 0, orientation = 0, goalLoc = 24 },
        new AgentState { id = 1, loc = 4, orientation = 2, goalLoc = 20 }
    }
};

// Gọi Coroutine
GetComponent<PIBTWebClient>().RequestPlan(req);
```

---

## 🔒 Lưu ý về CORS (Đã được cấu hình)
Lỗi CORS Policy (Cross-Origin Resource Sharing) rất hay xảy ra trên WebGL khi gọi API khác tên miền. Tuy nhiên, **bạn không cần lo lắng** vì tôi đã chủ động cấu hình chặn trước điều này trên server Hugging Face của chúng ta.
Server đã được khai báo whitelist trực tiếp tên miền `https://luminx.io.vn` và cả `http://localhost`, nên trình duyệt sẽ tự động cho phép kết nối trót lọt!
