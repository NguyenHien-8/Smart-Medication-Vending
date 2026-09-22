# SmartMediVend — kiểm tra sàng lọc và định tuyến kênh (chỉ mô phỏng)

## Đọc hiện trạng

- `main/boards/smartmedivend-s3/smartmedivend_board.cc` đăng ký 3 công cụ MCP cho hội thoại; không có công cụ cấp thuốc, không có GPIO kích relay. Không thay đổi phần I2S/TFT/Xiaozhi đang hoạt động.
- `main/medical/medical_advisor.cc` xác nhận các trường hồ sơ và câu trả lời cho từng câu hỏi trong `data/medical_rules.json`. AI có thể cung cấp dữ liệu không chính xác; sàng lọc từ JSON **không xác thực** việc người dùng thực sự đã trả lời.
- `data/pharmacist_review.json`: `approved=false`. `initial_stock` trong `data/medicines.json` là số liệu ban đầu, **không phải tồn kho được cảm biến/nhân viên xác minh**. Không bật khả năng bán thuốc từ dữ liệu này.
- `BoardPins.h`: `MUX_S0..S3 = 39..42`, `MUX_SIG=17`; danh mục gán 16 kênh 0..15. Kênh 13 là dự phòng kênh 0; 14 dự phòng 3; 15 dự phòng 7. Không có dữ liệu xác thực thực tế về đấu dây, kiểu mạch MUX, relay, tính toàn vẹn của vỉ thuốc, cảm biến rơi hàng và tình trạng an toàn điện.

## Phần thay đổi

1. Trong `medical_advisor.cc`, kiểm tra đầy đủ **mọi** `global_checks.expected/on_mismatch`: trước đây một giá trị `false` có thể được chấp nhận sai nếu sau này luật đổi `expected=true`. Giữ đường `REFER` khi có dấu hiệu nguy hiểm.
2. Trong `medical_advisor.cc`, kiểm tra `selected` khác null trước khi đọc `initial_stock`. Nếu có hơn một lựa chọn thuốc hợp lệ, trả `REFER/MULTIPLE_OPTIONS_REQUIRE_HUMAN_REVIEW` thay vì tự lấy thuốc đầu tiên theo thứ tự JSON; không thêm chức năng kích relay.
3. `main/medical/tests/local_route_simulator.{h,cc}`: ánh xạ **chỉ cho thử nghiệm host** từ một kết quả cố vấn cục bộ, đánh giá phiên bản luật/danh mục được duyệt, tồn kho *giả lập được xác minh*, duy nhất kênh 0..15, SKU chính/dự phòng đồng nhất tên-hàm lượng-hoạt chất. Chỉ trả `SIMULATION_READY`, không có API cấp phát thật.
4. `RelayPulseSimulation`: logic HIGH (nghỉ) → LOW (bắt đầu) → HIGH khi `Tick()` đủ 500 ms; không dùng `sleep`, chặn xung chồng lấp, xử lý bộ đếm uint32 tràn và hủy về HIGH. Chỉ là trạng thái phần mềm; độ dài xung thực tế vẫn phụ thuộc tần suất gọi `Tick()`, không bảo đảm 500 ms ngoài phần cứng.
5. Tất cả bộ mô phỏng nằm dưới `main/medical/tests/` và **không được thêm vào** firmware CMake/MCP: không có mã mới nào kích GPIO hay trừ kho thực.

**Bất nhất dữ liệu:** kênh 7 có `strength="ví dụ 200 mg + 200 mg"`, kênh dự phòng 15 ghi `strength="cùng SKU channel 7"`. Phần mô phỏng chặn cả hai kênh Antacid với `BACKUP_SKU_MISMATCH`. Chỉ test ánh xạ 7→15 trên bản catalog được sửa trong bộ nhớ test, không sửa danh mục chính thức. Cần xác nhận hàm lượng thật và nhãn thuốc; từ “ví dụ” không đủ để xác thực hàng hóa.

## Cách chạy test trên máy có g++ và libcjson.so.1

```sh
g++ -std=c++17 -Wall -Wextra -Werror \
 -Imain/medical/tests/host_include -Imain/medical \
 main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc \
 -Wl,-l:libcjson.so.1 -o /tmp/smv_medical_test
/tmp/smv_medical_test data/medical_rules.json data/medicines.json data/pharmacist_review.json

g++ -std=c++17 -Wall -Wextra -Werror \
 -Imain/medical/tests/host_include -Imain/medical \
 main/medical/medical_advisor.cc \
 main/medical/tests/local_route_simulator.cc \
 main/medical/tests/test_local_route_simulator.cc \
 -Wl,-l:libcjson.so.1 -o /tmp/smv_route_test
/tmp/smv_route_test data/medical_rules.json data/medicines.json data/pharmacist_review.json
python3 -m unittest discover -s scripts/tests -v
```

`host_include/cJSON.h` chỉ là khai báo cho unit test dùng thư viện libcjson hệ thống. Không thêm header giả lập này vào đường include của ESP-IDF.

## Chưa hoàn thành — không cấp thuốc trên thiết bị thật

- Chưa có sự chấp thuận theo từng phiên bản từ dược sĩ/đơn vị đủ thẩm quyền, kiểm tra tương tác và quy định cấp phát thuốc tại nơi triển khai. So khớp nhãn phiên bản không thay thế chữ ký số, hash hay hồ sơ duyệt nội dung.
- Chưa có nguồn kiểm kê đáng tin cậy xác nhận SKU, lô, hạn dùng, tồn kho thực theo kênh và cơ chế ghi giảm kho **sau** khi cảm biến xác nhận chính xác thuốc đã rơi ra; `initial_stock` không thể thay thế.
- Chưa có xác nhận độc lập từ người dùng về thông tin đã nhận dạng qua giọng nói, sai số STT, danh tính phiên hoặc phương án khi mạng/mất điện/reset.
- Chưa xác minh điện áp/loại board MUX/relay, mạch chống relay tự kích khi reset, trình tự nhả SIG trước khi đổi S0..S3, watchdog ngoài và cảm biến kẹt hàng. Không thể kết luận đúng vị trí relay chỉ từ số kênh JSON.
- `Tick()` chỉ thử thuật toán trạng thái; để bảo đảm độ dài xung vật lý cần timer phần cứng và kiểm chứng bằng logic analyzer/oscilloscope, cùng mạch ngắt năng lượng độc lập nếu firmware treo.
- Chưa compile firmware ESP-IDF hoặc chạy ESP32-S3/cảm biến/relay thật trong môi trường kiểm thử này. Không thay đổi `pharmacist_review.json` thành `approved=true` chỉ để vượt rào.
