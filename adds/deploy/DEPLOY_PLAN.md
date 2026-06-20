# 🚀 Kế Hoạch Deploy: Server-PIBT → Hugging Face Space
> **Cập nhật**: 2026-06-20 v2.1 (bổ sung CORS + WebGL compatibility)

**Project**: Server-PIBT-TeamNoMan-sSky (MAPF C++ `pibt_tcp_server` + FastAPI HTTP Proxy)  
**Client**: Unity WebGL **TankMAPF** tại `https://luminx.io.vn`  
**Target**: Hugging Face Spaces – Docker SDK · CPU Basic Free  
**CI/CD**: GitHub Actions → tự động push lên HF Space khi merge vào `main`

---

## 🔐 Phân Tích Tương Thích Client ↔ Server

### Client là Unity WebGL (TankMAPF – luminx.io.vn)

Phát hiện từ DOM của `https://luminx.io.vn`: đây là **Unity WebGL build** chạy trong browser.  
WebGL browser sandbox có các ràng buộc bảo mật quan trọng:

| Ràng buộc | Ảnh hưởng | Giải pháp |
|-----------|-----------|-----------|
| **CORS Policy** | Browser block mọi cross-origin request nếu server không có `Access-Control-Allow-Origin` | Thêm `CORSMiddleware` vào `app.py` ✅ |
| **No raw TCP** | WebGL **không thể** mở TCP socket trực tiếp | Dùng REST API qua `app.py` proxy ✅ |
| **HTTPS required** | `luminx.io.vn` là HTTPS → không gọi được HTTP server (mixed content) | HF Space tự cấp HTTPS `.hf.space` ✅ |
| **Preflight OPTIONS** | Browser gửi OPTIONS trước POST | `CORSMiddleware` handle tự động ✅ |

### Luồng Request Đầy Đủ

```
Unity WebGL (luminx.io.vn HTTPS)
        │
        │  OPTIONS /plan  ← preflight CORS
        ▼
  HF Space HTTPS (xxx.hf.space)
  CORSMiddleware: trả Access-Control-Allow-Origin: https://luminx.io.vn
        │
        │  POST /plan  {"team_size":..., "map":..., "agents":[...]}
        ▼
  FastAPI app.py (asyncio.Lock → đợi turn)
        │
        │  JSON-over-TCP (nội bộ container)
        │  hello → plan_step → shutdown
        ▼
  pibt_tcp_server:7777 (sequential §7)
        │
        │  plan_result { "actions": ["FW","CR",...] }
        ▼
  HTTP 200 JSON → Unity WebGL
```

### CORS Origins Đã Cấu Hình Trong `app.py`

```python
ALLOWED_ORIGINS = [
    "https://luminx.io.vn",   # Production Unity WebGL
    "http://luminx.io.vn",    # Fallback
    "http://localhost",        # Unity Editor local test
    "http://localhost:3000",   # Web frontend dev
    "http://127.0.0.1",
    "null",                    # file:// (Unity WebGL build local)
]
# Mở rộng qua HF Secret: CORS_EXTRA_ORIGINS=https://other.domain.com
```

---

## 📁 Cấu Trúc File Deploy

```
projectY/
├── Dockerfile                  ← Copy từ adds/deploy/Dockerfile.hf (đặt ở ROOT)
├── app.py                      ← Copy từ adds/deploy/app.py (đặt ở ROOT)
├── .github/
│   └── workflows/
│       └── deploy-hf.yml       ← Copy từ adds/deploy/deploy-hf.yml
└── adds/deploy/
    ├── DEPLOY_PLAN.md          ← File này
    ├── Dockerfile.hf           ← Source Dockerfile
    ├── app.py                  ← Source FastAPI proxy
    └── deploy-hf.yml           ← Source GitHub Actions workflow
```

> **Lưu ý**: `Dockerfile` và `app.py` phải đặt ở **thư mục gốc** (root) của repo.

---

## 🏗️ Kiến Trúc Hệ Thống

```
[Client HTTP]
      │ POST /plan  (HTTP 7860)
      ▼
[FastAPI – app.py]          ← chạy trong container, port 7860
      │ JSON-over-TCP        ← giao thức theo unity_server_guide.md §6
      │ hello → plan_step → shutdown
      ▼
[pibt_tcp_server]           ← spawn lúc startup, TCP 7777 (nội bộ)
      │ DefaultPlanner
      ▼
[plan_result]               ← actions: FW / CR / CCR / W
```

**Tại sao không expose TCP trực tiếp?**  
HF Spaces chỉ cho phép **1 port duy nhất là 7860 (HTTP)**. Binary `pibt_tcp_server` dùng TCP 7777 nên phải có `app.py` làm proxy ở giữa.

---

## BƯỚC 1 – Tạo Hugging Face Space

1. Truy cập: **https://huggingface.co/new-space**
2. Điền thông tin:

| Trường | Giá trị |
|--------|---------|
| Space name | `server-pibt` (tên tùy chọn) |
| SDK | **Docker** ← ô giữa hàng trên |
| Docker template | **Blank** |
| Hardware | **CPU basic · Free** |
| Visibility | `Private` (khuyến nghị) |

3. Nhấn **Create Space**  
4. Ghi lại **Space ID**: `{username}/server-pibt`

---

## BƯỚC 2 – Lấy Hugging Face Access Token

1. Đăng nhập HF → avatar → **Settings**
2. Sidebar → **Access Tokens**: https://huggingface.co/settings/tokens
3. Nhấn **New token**:
   - Name: `github-actions-deploy`
   - **Type: Write** ← bắt buộc
4. Nhấn **Generate** → **Copy ngay** (chỉ hiện 1 lần)

```
Token format: hf_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

---

## BƯỚC 3 – Cấu Hình GitHub Secrets

**Vị trí**: GitHub repo → **Settings → Secrets and variables → Actions → New repository secret**

| Secret Name | Giá trị |
|-------------|---------|
| `HF_TOKEN` | Token HF vừa lấy (`hf_xxx...`) |
| `HF_SPACE_ID` | `{username}/server-pibt` |

> **Không cần** tạo GitHub Environment (đã bỏ khỏi workflow để tránh block).

---

## BƯỚC 4 – Cấu Hình HF Space Secrets (Runtime)

Nếu `app.py` cần biến môi trường lúc chạy:

1. HF Space → tab **Settings** → **Variables and secrets** → **New secret**

| Name | Value | Ghi chú |
|------|-------|---------|
| `PIBT_TCP_PORT` | `7777` | Port nội bộ TCP (mặc định đã là 7777) |
| `OPENAI_API_KEY` | `sk-...` | Chỉ cần nếu tích hợp LLM |

---

## BƯỚC 5 – Đặt File Vào Đúng Vị Trí & Push

```bash
# Từ thư mục gốc projectY:
cp adds/deploy/Dockerfile.hf Dockerfile
cp adds/deploy/app.py app.py
mkdir -p .github/workflows
cp adds/deploy/deploy-hf.yml .github/workflows/deploy-hf.yml

git add Dockerfile app.py .github/workflows/deploy-hf.yml
git commit -m "feat: deploy pibt_tcp_server to HF Space via GitHub Actions"
git push origin main
```

---

## BƯỚC 6 – Theo Dõi CI/CD

1. GitHub repo → tab **Actions** → workflow **"🚀 Deploy to Hugging Face Space"**
2. Xem 2 job:

```
validate (🔍 Validate files)          ~30s
  └── Kiểm tra Dockerfile, app.py tồn tại
  └── Kiểm tra EXPOSE 7860 trong Dockerfile
      ↓ pass
deploy (🤗 Push to HF Space)
  └── git push --force → HF Space
  └── Polling /health mỗi 30s (tối đa 20 phút)
  └── Ghi Deploy Summary
```

---

## BƯỚC 7 – Kiểm Tra Sau Deploy

### 7.1 Xem build logs trên HF Space

HF Space → tab **Logs** → **Build** (xem quá trình cmake + compile C++)

> ⚠️ Build C++ trên Free CPU mất **10–20 phút**. Bình thường.

### 7.2 Test API khi Space Running

```bash
SPACE="https://{username}-server-pibt.hf.space"

# Health check
curl $SPACE/health

# Full session: hello → plan_step → shutdown
curl -X POST $SPACE/plan \
  -H "Content-Type: application/json" \
  -d '{
    "team_size": 2,
    "map": {
      "width": 5,
      "height": 5,
      "symbols": "........................."
    },
    "agents": [
      {"id": 0, "loc": 0,  "orientation": 0, "goalLoc": 24},
      {"id": 1, "loc": 4,  "orientation": 2, "goalLoc": 20}
    ],
    "timestep": 0
  }'
```

**Response mong đợi** (từ `plan_result`):
```json
{
  "hello_ack":    { "type": "hello_ack",    "status": "ok" },
  "plan_result":  { "type": "plan_result",  "actions": ["FW","FW"], "computeMs": 12 },
  "shutdown_ack": { "type": "shutdown_ack", "status": "ok" }
}
```

### 7.3 Swagger UI

Truy cập `{SPACE_URL}/docs` để xem và test tất cả endpoint.

---

## 📝 Checklist Triển Khai

- [ ] Tạo HF Space (Docker SDK · Blank · CPU Free)
- [ ] Lấy HF Access Token **(Write)**
- [ ] Thêm GitHub Secrets: `HF_TOKEN`, `HF_SPACE_ID`
- [ ] Copy `Dockerfile.hf → Dockerfile` (root)
- [ ] Copy `app.py → app.py` (root)
- [ ] Copy `deploy-hf.yml → .github/workflows/`
- [ ] `git commit && git push origin main`
- [ ] Kiểm tra GitHub Actions (validate + deploy xanh)
- [ ] Chờ HF Space build xong (10–20 phút)
- [ ] Test `GET /health` → `"status": "ok"`
- [ ] Test `POST /plan` với map mẫu

---

## ⚠️ Những Điều Cần Nhớ

| # | Vấn đề | Giải pháp đã áp dụng |
|---|--------|----------------------|
| 1 | `compile.sh` build cả `lifelong` (quá nặng) | Dockerfile chỉ build target `pibt_tcp_server` |
| 2 | HF chỉ cho phép port 7860 HTTP | `app.py` làm HTTP proxy → TCP 7777 nội bộ |
| 3 | `libboost-log1.74.0` không tồn tại trong Ubuntu 22.04 | Dùng `libboost-all-dev` ở runtime |
| 4 | `environment: huggingface` cần tạo thủ công | Đã bỏ khỏi workflow |
| 5 | Health check 90s quá ngắn cho build C++ | Polling mỗi 30s, tối đa 20 phút |
| 6 | `git push --force` overwrite HF history | Không edit trực tiếp trên HF UI |

---

## 📚 Tham Khảo

| Tài nguyên | Link |
|------------|------|
| HF Spaces Docker Guide | https://huggingface.co/docs/hub/spaces-sdks-docker |
| HF Access Tokens | https://huggingface.co/settings/tokens |
| HF Space Variables & Secrets | https://huggingface.co/docs/hub/spaces-overview#managing-secrets |
| GitHub Encrypted Secrets | https://docs.github.com/en/actions/security-guides/using-secrets-in-github-actions |
| Protocol reference | `adds/unity_server_guide.md` §6 |
