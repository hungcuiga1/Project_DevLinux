/* adapters.c - 4 protocol adapter: TBP, HLP, EVP, SGP
 * Mỗi adapter giải mã byte -> sensor_data_t.
 * Đọc từng byte, không ép con trỏ về struct (tránh padding / endian).
 * Lưu ý: đề chỉ cho các reason cố định (bad CRC, bad length...) nên lỗi
 * sai magic / sai giá trị / sai format em cũng ghi chung là "bad length".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol_adapter.h"

const char *g_parse_reason = "";

/* ---------- hàm phụ đọc số ---------- */
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t rd_le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}
static uint16_t rd_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF */
uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc = crc << 1;
        }
    }
    return crc;
}

/* ================= TBP: 18 byte, little-endian ================= */
#define TBP_LEN 18
static parse_result_t tbp_parse(const uint8_t *buf, size_t len, sensor_data_t *out, size_t *consumed)
{
    uint64_t raw;
    double val;

    if (len < TBP_LEN)
        return PARSE_NEED_MORE;

    raw = rd_le64(buf + 2);
    memcpy(&val, &raw, sizeof(val));   /* 8 byte -> double */

    *consumed = TBP_LEN;
    /* giá trị ngoài dải -40..85 coi như frame hỏng */
    if (!(val >= -40.0 && val <= 85.0)) {
        g_parse_reason = "bad length";
        return PARSE_ERROR;
    }
    out->id = rd_le16(buf);
    out->type = SENSOR_TEMPERATURE;
    out->value = val;
    out->ts = (time_t)(int64_t)rd_le64(buf + 10);
    return PARSE_OK;
}

/* ================= HLP: "HUM <id> <value> <ts>\n" ================= */
#define HLP_MAX 64
static parse_result_t hlp_parse(const uint8_t *buf, size_t len, sensor_data_t *out, size_t *consumed)
{
    char line[HLP_MAX + 1];
    size_t i, n = len < HLP_MAX ? len : HLP_MAX;
    unsigned int id;
    double val;
    long long ts;
    int pos = 0;

    /* tìm ký tự LF */
    for (i = 0; i < n; i++)
        if (buf[i] == '\n')
            break;

    if (i == n) {
        if (len >= HLP_MAX) {
            /* 64 byte mà vẫn chưa thấy LF -> lỗi, bỏ 64 byte */
            *consumed = HLP_MAX;
            g_parse_reason = "bad length";
            return PARSE_ERROR;
        }
        return PARSE_NEED_MORE;
    }

    *consumed = i + 1;
    memcpy(line, buf, i);
    line[i] = '\0';

    /* %n để chắc chắn đọc hết cả dòng, không có rác ở cuối */
    if (sscanf(line, "HUM %u %lf %lld%n", &id, &val, &ts, &pos) != 3 || line[pos] != '\0'
        || id > 0xFFFF || val < 0.0 || val > 100.0) {
        g_parse_reason = "bad length";
        return PARSE_ERROR;
    }
    out->id = (sensor_id_t)id;
    out->type = SENSOR_HUMIDITY;
    out->value = val;
    out->ts = (time_t)ts;
    return PARSE_OK;
}

/* ================= EVP: TLV, big-endian ================= */
static parse_result_t evp_parse(const uint8_t *buf, size_t len, sensor_data_t *out, size_t *consumed)
{
    uint8_t type, l;

    if (len < 2)
        return PARSE_NEED_MORE;
    type = buf[0];
    l = buf[1];

    /* type/length không hợp lệ -> bỏ 1 byte để dò lại */
    if (!((type == 0x01 && l == 6) || (type == 0x02 && l == 7))) {
        *consumed = 1;
        g_parse_reason = "bad length";
        return PARSE_ERROR;
    }
    if (len < (size_t)(2 + l))
        return PARSE_NEED_MORE;

    *consumed = 2 + l;
    out->id = rd_be16(buf + 2);
    if (type == 0x01) {
        out->type = SENSOR_MOTION;
        out->value = 1;
        out->ts = rd_be32(buf + 4);
    } else {
        if (buf[4] > 1) {          /* state chỉ 0 hoặc 1 */
            g_parse_reason = "bad length";
            return PARSE_ERROR;
        }
        out->type = SENSOR_DOOR;
        out->value = buf[4];
        out->ts = rd_be32(buf + 5);
    }
    return PARSE_OK;
}

/* ================= SGP: 11 byte, magic 0x7E, CRC16 ================= */
#define SGP_LEN   11
#define SGP_MAGIC 0x7E
#define SGP_ACK   0x06
#define SGP_NAK   0x15
static parse_result_t sgp_parse(const uint8_t *buf, size_t len, sensor_data_t *out, size_t *consumed)
{
    uint16_t crc, ppm;

    if (len < 1)
        return PARSE_NEED_MORE;

    if (buf[0] != SGP_MAGIC) {
        /* sai magic: bỏ hết các byte cho tới 0x7E tiếp theo */
        size_t i = 0;
        while (i < len && buf[i] != SGP_MAGIC)
            i++;
        *consumed = i;
        g_parse_reason = "bad length";
        return PARSE_ERROR;
    }
    if (len < SGP_LEN)
        return PARSE_NEED_MORE;

    *consumed = SGP_LEN;
    crc = crc16_ccitt(buf + 1, 8);
    if (crc != rd_be16(buf + 9)) {
        g_parse_reason = "bad CRC";
        return PARSE_ERROR;
    }
    ppm = rd_be16(buf + 3);
    if (ppm > 10000) {
        g_parse_reason = "bad length";
        return PARSE_ERROR;
    }
    out->id = rd_be16(buf + 1);
    out->type = SENSOR_SMOKE_GAS;
    out->value = ppm;
    out->ts = rd_be32(buf + 5);
    return PARSE_OK;
}

static size_t sgp_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    if (cap < 1 || result == PARSE_NEED_MORE)
        return 0;
    reply[0] = (result == PARSE_OK) ? SGP_ACK : SGP_NAK;
    return 1;
}

/* các giao thức không phản hồi */
static size_t no_reply(parse_result_t result, uint8_t *reply, size_t cap)
{
    (void)result; (void)reply; (void)cap;
    return 0;
}

/* bảng adapter, index = port_offset. Thêm giao thức mới chỉ cần thêm 1 dòng ở đây */
const protocol_adapter_t ADAPTERS[NUM_ADAPTERS] = {
    { "TBP", 0, tbp_parse, no_reply  },
    { "HLP", 1, hlp_parse, no_reply  },
    { "EVP", 2, evp_parse, no_reply  },
    { "SGP", 3, sgp_parse, sgp_reply },
};
