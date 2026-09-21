# Bản vá SmartMediVend – giai đoạn 1: thu thập triệu chứng qua Xiaozhi

1. Giải nén bản vá vào **thư mục gốc** của project Smart-Medication-Vending mới nhất, giữ nguyên cấu trúc `main/` và `docs/`. File ZIP chỉ chứa các file thay đổi hoặc mới, không chứa toàn bộ project.
2. **Giữ nguyên** ba file `data/medical_rules.json`, `data/medicines.json`, `data/pharmacist_review.json` hiện tại. CMake đọc trực tiếp ba file này và tạo header dữ liệu khi build. Không sửa `medical_data_generated.h.in` thành dữ liệu JSON thủ công.
3. Trong môi trường ESP-IDF v6.1 (đúng board SmartMediVend-S3), chạy `idf.py build` rồi `idf.py -p COMx flash monitor`; thay COMx bằng cổng đúng. Chưa cần `erase-flash` và không cần sửa phần audio/VAD.
4. Cập nhật phần vai trò / lời nhắc hệ thống của thiết bị ở dịch vụ Xiaozhi bằng `docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md`. File này **không** tự được nạp lên cloud khi build firmware.
5. Kiểm tra log: `MCP: Add tool: self.medical.get_intake_schema` và `MCP: Add tool: self.medical.evaluate_symptoms`. Thử yêu cầu mô tả triệu chứng; xác minh trên **cloud** rằng AI đã gọi công cụ MCP và sử dụng kết quả trả về thay vì tự nói tên thuốc.
6. Kết quả giai đoạn 1 chỉ có trạng thái `NEED_MORE_INFO`, `REFER`, `DENY` hoặc `PROVISIONAL_OPTIONS` (mang tính thử nghiệm). `PROVISIONAL_OPTIONS` không xác nhận còn hàng thực tế, không phải quyết định dùng thuốc hay lệnh cấp thuốc. **Không kích relay và không phát thuốc cho người dùng**.

Chi tiết kỹ thuật và kiểm thử: `docs/smartmedivend/STAGE1_INTEGRATION_AND_TEST.md`.
