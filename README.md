# Sensor Gateway – giai đoạn giữa kỳ (Step 0–7)

Gateway viết bằng C trên Linux: nhận dữ liệu từ nhiều sensor node qua TCP (4 giao thức TBP/HLP/EVP/SGP),
xử lý cảnh báo theo loại sensor, lưu SQLite, ghi log qua FIFO vào gateway.log và kết thúc có kiểm soát.
Chưa có giao diện web (Step 8–12 làm ở cuối kỳ).

## Cấu trúc thư mục

```
include/   sensor_types.h, sensor_config.h, protocol_adapter.h (cung cấp sẵn), gateway.h, sbuffer.h, logger.h
src/       main.c, sbuffer.c, logger.c, connmgr.c, adapters.c, datamgr.c, storagemgr.c
test/      sensor_node.c (mô phỏng sensor), test_parse.c (test các hàm parse)
web/       (để trống, dùng ở Step 9-10)
room_sensor.map   file room-sensor
```

## Build

Cần: gcc, make, libsqlite3-dev (`sudo apt install libsqlite3-dev`)

```
make            # build server, sensor_node, test_parse
make debug      # build với -DDEBUG để in thông tin chẩn đoán ra stderr
make test       # test parse() bằng các ví dụ ở mục 3.4 + vector CRC 0x29B1
make distclean  # xoá file build + gateway.log + sensor.db
```

## Chạy

```
./server 1234 8080
```
Gateway nghe cổng 1234 (TBP), 1235 (HLP), 1236 (EVP), 1237 (SGP). Cổng HTTP 8080 hiện chỉ được đọc và kiểm tra.

Chạy sensor ở terminal khác:
```
./sensor_node temperature 10 127.0.0.1 1234 1000
./sensor_node temperature 11 127.0.0.1 1234 1000 --fragment
./sensor_node humidity    21 127.0.0.1 1234 1000 --glue
./sensor_node motion      30 127.0.0.1 1234 2000
./sensor_node door        40 127.0.0.1 1234 2000 --corrupt
./sensor_node smoke_gas   50 127.0.0.1 1234 1000 --corrupt
./sensor_node temperature 99 127.0.0.1 1234 1000      # ID không có trong room_sensor.map
./sensor_node humidity    10 127.0.0.1 1234 1000      # ID có nhưng sai loại
```
Dừng gateway: Ctrl+C.

## Kiến trúc

- **Main process**: tạo FIFO + fork log process, nạp room-sensor, tạo 3 thread, chờ cờ stop rồi dọn dẹp.
- **Connection manager** (thread): 1 thread dùng `poll()` cho 4 socket lắng nghe và mọi kết nối.
  Mỗi kết nối có bộ đệm riêng, gọi `parse()` của adapter đến khi `PARSE_NEED_MORE`.
- **Data manager** (thread): bảng handler theo loại sensor, trung bình 5 mẫu cho nhiệt độ/độ ẩm,
  so ngưỡng tức thời cho khói/gas, motion/door ghi sự kiện.
- **Storage manager** (thread): ghi SQLite, mất kết nối thì thử lại 3 lần (cách 2 giây), vẫn lỗi thì dừng gateway.
- **sbuffer**: 1 linked list + mutex + condition variable, mỗi node có 2 cờ đã đọc, chỉ xoá khi cả 2 thread đã đọc.
- **Log process**: đọc FIFO `logFifo`, ghi `gateway.log` dạng `<seq> <thời điểm> <nguồn> <thông điệp>`.

## Một số lựa chọn

- **poll() thay vì 1 thread/kết nối**: chỉ 1 thread, không tốn context switch, không phải đồng bộ nhiều thread ghi.
  Đánh đổi: xử lý 1 kết nối chậm thì kết nối khác phải chờ (ở đây xử lý rất nhanh nên không sao).
- **Lỗi parse**: ghi log, bỏ byte hỏng theo `consumed`. Lỗi liên tiếp 50 lần thì đóng kết nối
  (để 50 vì EVP dò lại từng byte, 1 frame hỏng có thể sinh nhiều lỗi).
  Lỗi khi chưa có frame hợp lệ nào ghi `Connection attempt failed ... (malformed first frame)`, vẫn giữ kết nối.
- **reason của lỗi**: đề chỉ cho các giá trị cố định nên sai CRC ghi `bad CRC`, còn sai magic/độ dài/giá trị/format ghi `bad length`.
- **Frame SGP trùng (cùng id, ts)**: chọn **lưu cả hai** (ts tính bằng giây, sensor gửi nhanh thì phép đo khác nhau cũng có thể trùng ts).
- **Mất SQL**: phép đo đang ghi được giữ lại và ghi lại sau khi kết nối lại, các phép đo khác vẫn nằm trong sbuffer.
  Để test: đổi tên `sensor.db` khi đang chạy (reconnect không tạo file mới nên sẽ lỗi), đổi tên lại để khôi phục.
- **Kết thúc**: signal handler chỉ set cờ `stop`. Main gọi `sbuffer_shutdown()` để đánh thức 2 thread đọc,
  `pthread_join` 3 thread, đóng FIFO (log process đọc EOF rồi tự thoát), `waitpid` log process, giải phóng sbuffer.

## Kiểm chứng

```
make test
valgrind --leak-check=full ./server 1234 8080      # chạy vài sensor rồi Ctrl+C
valgrind --tool=helgrind ./server 1234 8080
make debug && ./server 1234 8080 2> debug.txt      # so đầu ra datamgr/storagemgr: mỗi phép đo đúng 1 lần
```
