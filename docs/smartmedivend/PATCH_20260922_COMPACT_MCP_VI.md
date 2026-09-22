# Bản vá MCP gọn & phỏng vấn có kiểm soát — SmartMediVend (10)

> **TÀI LIỆU LỊCH SỬ:** tài liệu này chỉ ghi lại một bản vá MCP/audio cũ; các tuyên bố về relay và trạng thái phê duyệt không mô tả firmware hiện tại. Xem `STAGE1_INTEGRATION_AND_TEST.md` và `FAIL_CLOSED_VENDING_TEST.md`.

## Dấu vết từ log mới

- Ở lượt người dùng nói về thời gian đau đầu, STT xuất ra `Thu nhất đội trong một ngày.` (không thể kết luận chắc thời gian từ bản chép này). AI vẫn nói đã bắt đầu tư vấn.
- Ngay sau yêu cầu `self.medical.get_intake_schema`, kết nối TCP/TLS của MQTT bị peer reset (`errno=104`). Khi gửi MCP result, transport đã mất kết nối và QOS 0 bị mất. Bản ghi lỗi cũ in ra toàn bộ kết quả MCP: JSON schema 13.507 byte, cả bản tin MQTT JSON 14.625 byte.
- Không có bằng chứng từ log để xác định chắc peer reset do kích thước bản tin, mạng Wi-Fi, máy chủ, hay giới hạn MQTT trung gian. Giảm kích thước schema là thay đổi có cơ sở để loại bỏ bản tin quá lớn, **không đảm bảo giải quyết mọi reset**.

## Tệp đã sửa

- `main/medical/medical_advisor.cc/.h`: `get_intake_schema` giờ trả về contract rút gọn + nhãn triệu chứng, tối đa 3.200 byte, KHÔNG gửi cả bảng interview, hướng dẫn thuốc và flags; `evaluate_symptoms` vẫn đọc bộ luật đầy đủ ngay trên ESP32 để trả câu hỏi tiếp theo và đánh giá. Có thêm `get_symptom_guide` tùy chọn chỉ trả hướng dẫn một triệu chứng, giới hạn 2.400 byte. Không expose SKU/kênh relay.
- `main/boards/smartmedivend-s3/smartmedivend_board.cc`: mô tả MCP ngắn hơn; thêm công cụ tham khảo một nhóm triệu chứng khi cần phân biệt.
- `data/medical_rules.json`: bắt buộc hỏi xác nhận mức **nhẹ** cho `mild_headache` trước khi có lựa chọn tham khảo; tăng version luật (approval vẫn khóa). Không mở rộng chỉ định thuốc.
- `main/protocols/mqtt_protocol.cc`: không ghi payload y tế vào log khi MQTT publish hay parse thất bại; ghi độ dài để chẩn đoán.
- `docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md`: AI chỉ đọc `next_question_vi` một câu, không thêm dẫn dài; STT sai phải hỏi lại; hướng dẫn dùng guide theo yêu cầu và xử lý lỗi mạng.
- `main/medical/tests/test_medical_advisor.cc`: kiểm thử schema/guide kích thước, 15 profile, và bắt buộc làm rõ mức đau đầu.

## Áp dụng

Giải nén patch vào gốc source bản (10), giữ nguyên cấu trúc, ghi đè các tệp trùng tên. Cần **cập nhật vai trò AI trên đúng tài khoản/server Xiaozhi đang dùng**, vì firmware không tự tải prompt này lên máy chủ. Vì đã sửa `data/medical_rules.json` được CMake nhúng lúc configure, chạy `idf.py reconfigure build` (hoặc `idf.py fullclean build` nếu build cache không đồng bộ), sau đó `idf.py -p COMx flash monitor`.

## Test host / test thiết bị

- Build unit host: `g++ -std=c++17 -O2 -Wall -Wextra -Werror -Imain/medical/tests/host_include -Imain/medical main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc /usr/lib/x86_64-linux-gnu/libcjson.so.1 -o /tmp/test_med && /tmp/test_med data/medical_rules.json data/medicines.json data/pharmacist_review.json` (Linux với libcjson.so.1).
- `python -m unittest discover -s scripts/tests -p 'test_*.py' -q`.
- ESP-IDF và phần cứng KHÔNG có trong môi trường tạo bản vá: build/flash/cloud thực tế = NOT RUN. Log sau khi flash cần in được: CONNECTED, MCP request type, kích thước response mới, trạng thái mạng, `next_question_id`, và trạng thái `vend_allowed=false`. **Không in nội dung thông tin bệnh/sức khỏe cá nhân**.
- Kiểm thử thực tế: hỏi đau đầu chưa rõ mức, nói câu về thời gian bị STT nhận sai, đưa câu trả lời không liên quan khi đang hỏi tuổi/dị ứng, và mô phỏng mất MQTT trong lúc gọi MCP. Không tự giới thiệu lựa chọn thuốc nếu MCP chưa trả về kết quả xác nhận.

## Giới hạn an toàn

Trong bản lịch sử này, `pharmacist_review.json` vẫn ở trạng thái chưa phê duyệt. Thuốc trong danh mục chỉ phục vụ phân loại tham khảo chưa xác minh độc lập; không có cấp thuốc thật. Việc AI tuân thủ 100% một-câu-hỏi/lượt không thể ép chỉ bằng firmware khi câu nói được sinh trên máy chủ Xiaozhi; cần kiểm thử prompt hoặc một cơ chế kiểm soát lời thoại phía máy chủ. Gói này không thay đổi âm thanh, phiên MQTT/UDP chung, VAD, màn hình, GPIO hay relay.
