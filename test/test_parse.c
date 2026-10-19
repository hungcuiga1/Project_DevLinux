/* test_parse.c - kiểm thử riêng từng parse() bằng ví dụ trong đề (mục 3.4) */
#include <stdio.h>
#include <string.h>
#include "protocol_adapter.h"

static int fail = 0;
#define CHECK(cond, msg) do { if (cond) printf("[OK]   %s\n", msg); else { printf("[FAIL] %s\n", msg); fail++; } } while (0)

int main(void)
{
    sensor_data_t d;
    size_t used;
    parse_result_t r;

    /* CRC vector */
    CHECK(crc16_ccitt((const uint8_t *)"123456789", 9) == 0x29B1, "CRC16 '123456789' = 0x29B1");

    /* TBP */
    uint8_t tbp[] = { 0x0A,0x00, 0x00,0x00,0x00,0x00,0x00,0x80,0x35,0x40, 0x00,0xB9,0x55,0x69,0x00,0x00,0x00,0x00 };
    r = ADAPTERS[0].parse(tbp, sizeof(tbp), &d, &used);
    CHECK(r == PARSE_OK && d.id == 10 && d.value == 21.5 && d.ts == 1767225600 && used == 18, "TBP vi du");
    r = ADAPTERS[0].parse(tbp, 10, &d, &used);
    CHECK(r == PARSE_NEED_MORE, "TBP thieu byte -> NEED_MORE");

    /* HLP */
    const char *hlp = "HUM 21 54.3 1767225600\n";
    r = ADAPTERS[1].parse((const uint8_t *)hlp, strlen(hlp), &d, &used);
    CHECK(r == PARSE_OK && d.id == 21 && d.value > 54.29 && d.value < 54.31 && used == strlen(hlp), "HLP vi du");
    r = ADAPTERS[1].parse((const uint8_t *)"HUM 21 5", 8, &d, &used);
    CHECK(r == PARSE_NEED_MORE, "HLP chua co LF -> NEED_MORE");
    r = ADAPTERS[1].parse((const uint8_t *)"HUM xx\n", 7, &d, &used);
    CHECK(r == PARSE_ERROR && used == 7, "HLP sai format -> ERROR");

    /* EVP */
    uint8_t mo[] = { 0x01,0x06, 0x00,0x1E, 0x69,0x55,0xB9,0x00 };
    r = ADAPTERS[2].parse(mo, sizeof(mo), &d, &used);
    CHECK(r == PARSE_OK && d.id == 30 && d.type == SENSOR_MOTION && d.ts == 1767225600 && used == 8, "EVP motion vi du");
    uint8_t door[] = { 0x02,0x07, 0x00,0x28, 0x01, 0x69,0x55,0xB9,0x00 };
    r = ADAPTERS[2].parse(door, sizeof(door), &d, &used);
    CHECK(r == PARSE_OK && d.id == 40 && d.type == SENSOR_DOOR && d.value == 1 && used == 9, "EVP door vi du");
    uint8_t badevp[] = { 0x03,0x06 };
    r = ADAPTERS[2].parse(badevp, 2, &d, &used);
    CHECK(r == PARSE_ERROR && used == 1, "EVP type sai -> ERROR");

    /* SGP */
    uint8_t sgp[] = { 0x7E, 0x00,0x32, 0x00,0x78, 0x69,0x55,0xB9,0x00, 0xAC,0xF5 };
    r = ADAPTERS[3].parse(sgp, sizeof(sgp), &d, &used);
    CHECK(r == PARSE_OK && d.id == 50 && d.value == 120 && d.ts == 1767225600 && used == 11, "SGP vi du (CRC 0xACF5)");
    uint8_t rep;
    CHECK(ADAPTERS[3].build_reply(PARSE_OK, &rep, 1) == 1 && rep == 0x06, "SGP ACK");
    sgp[10] = 0x00;
    r = ADAPTERS[3].parse(sgp, sizeof(sgp), &d, &used);
    CHECK(r == PARSE_ERROR && strcmp(g_parse_reason, "bad CRC") == 0, "SGP sai CRC -> ERROR");
    CHECK(ADAPTERS[3].build_reply(r, &rep, 1) == 1 && rep == 0x15, "SGP NAK");

    printf("\n%s (%d loi)\n", fail ? "CO TEST FAIL" : "TAT CA OK", fail);
    return fail ? 1 : 0;
}
