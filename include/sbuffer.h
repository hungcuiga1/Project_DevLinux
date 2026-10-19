#ifndef SBUFFER_H
#define SBUFFER_H

#include "gateway.h"

/* ai đang đọc sbuffer */
typedef enum { READER_DATA = 0, READER_STORAGE = 1 } sbuffer_reader_t;

int  sbuffer_init(sbuffer_t *sb);
void sbuffer_free(sbuffer_t *sb);

/* thêm 1 phép đo vào cuối, trả về số phần tử hiện tại */
size_t sbuffer_insert(sbuffer_t *sb, const sensor_data_t *data);

/* đọc phần tử tiếp theo mà reader này chưa đọc.
 * block nếu chưa có dữ liệu. trả 0 nếu ok, -1 nếu đã shutdown và hết dữ liệu.
 * *size_out = số phần tử còn lại sau khi đọc */
int sbuffer_read(sbuffer_t *sb, sbuffer_reader_t who, sensor_data_t *out, size_t *size_out);

/* báo cho các thread đọc biết là kết thúc */
void sbuffer_shutdown(sbuffer_t *sb);

#endif
