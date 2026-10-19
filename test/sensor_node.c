/* sensor_node.c - mô phỏng 1 sensor node
 * ./sensor_node <loại> <id> <host> <cổng gốc P> <chu kỳ ms> [--fragment|--glue|--corrupt]
 * loại: temperature | humidity | motion | door | smoke_gas
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "protocol_adapter.h"

static int sock;
static int opt_fragment, opt_glue, opt_corrupt;

static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static void wr_le(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (v >> (8 * i)) & 0xFF; }
static void wr_be(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (v >> (8 * (n - 1 - i))) & 0xFF; }

/* gửi 1 khối byte, có thể chia nhỏ (--fragment) */
static int send_bytes(const uint8_t *b, size_t len)
{
    if (!opt_fragment)
        return send(sock, b, len, MSG_NOSIGNAL) == (ssize_t)len ? 0 : -1;
    /* mỗi lần gửi 3 byte, nghỉ 20ms */
    for (size_t i = 0; i < len; i += 3) {
        size_t n = len - i < 3 ? len - i : 3;
        if (send(sock, b + i, n, MSG_NOSIGNAL) != (ssize_t)n)
            return -1;
        usleep(20000);
    }
    return 0;
}

/* tạo 1 frame theo loại sensor, trả về độ dài */
static size_t make_frame(const char *type, uint16_t id, double val, uint8_t *f)
{
    time_t ts = time(NULL);
    int bad = opt_corrupt && (rand() % 5 == 0);   /* 1/5 frame bị hỏng */

    if (strcmp(type, "temperature") == 0) {
        uint64_t raw;
        memcpy(&raw, &val, 8);
        wr_le(f, id, 2);
        wr_le(f + 2, raw, 8);
        wr_le(f + 10, (uint64_t)ts, 8);
        if (bad)
            wr_le(f + 2, 0x7FF8000000000000ULL, 8);   /* NaN -> ngoài dải */
        return 18;
    }
    if (strcmp(type, "humidity") == 0) {
        if (bad)
            return sprintf((char *)f, "HUM abc xyz\n");
        return sprintf((char *)f, "HUM %u %.1f %ld\n", id, val, (long)ts);
    }
    if (strcmp(type, "motion") == 0 || strcmp(type, "door") == 0) {
        int motion = type[0] == 'm';
        f[0] = motion ? 0x01 : 0x02;
        f[1] = motion ? 6 : 7;
        wr_be(f + 2, id, 2);
        if (motion) {
            wr_be(f + 4, (uint64_t)ts, 4);
        } else {
            f[4] = (uint8_t)val;
            wr_be(f + 5, (uint64_t)ts, 4);
        }
        if (bad)
            f[1] = 9;   /* sai length */
        return motion ? 8 : 9;
    }
    /* smoke_gas (làm hỏng frame ở send_sgp) */
    f[0] = 0x7E;
    wr_be(f + 1, id, 2);
    wr_be(f + 3, (uint16_t)val, 2);
    wr_be(f + 5, (uint64_t)ts, 4);
    wr_be(f + 9, crc16_ccitt(f + 1, 8), 2);
    return 11;
}

/* SGP: chờ ACK, timeout 1s, NAK/timeout thì gửi lại tối đa 3 lần */
static int send_sgp(const uint8_t *f, size_t len)
{
    for (int i = 0; i < 4; i++) {          /* 1 lần đầu + 3 lần gửi lại */
        uint8_t r, tmp[16];
        memcpy(tmp, f, len);
        /* --corrupt: chỉ làm hỏng lần gửi đầu, lần gửi lại là frame đúng (giống nhiễu đường truyền) */
        if (i == 0 && opt_corrupt && rand() % 5 == 0) {
            if (rand() % 2)
                tmp[10] ^= 0xFF;   /* sai CRC */
            else
                tmp[0] = 0x55;     /* sai magic */
        }
        if (send_bytes(tmp, len) < 0)
            return -1;
        ssize_t n = recv(sock, &r, 1, 0);
        if (n == 0)
            return -1;
        if (n == 1 && r == 0x06)
            return 0;
        printf("sensor: khong nhan duoc ACK, gui lai (%d)\n", i + 1);
    }
    return 0;
}

/* sinh giá trị kiểu random walk cho có vẻ thật */
static double next_value(const char *type, double cur)
{
    double step = (rand() % 200 - 100) / 100.0;   /* -1..1 */
    if (strcmp(type, "temperature") == 0) {
        cur += step * 0.8;
        if (cur < 10) cur = 10;
        if (cur > 26) cur = 26;
    } else if (strcmp(type, "humidity") == 0) {
        cur += step * 2;
        if (cur < 15) cur = 15;
        if (cur > 90) cur = 90;
    } else if (strcmp(type, "door") == 0) {
        cur = cur ? 0 : 1;   /* đổi trạng thái */
    } else if (strcmp(type, "smoke_gas") == 0) {
        cur += step * 60;
        if (cur < 0) cur = 0;
        if (cur > 600) cur = 600;
    } else {
        cur = 1;
    }
    return cur;
}

int main(int argc, char *argv[])
{
    const char *type;
    uint16_t id, base;
    int period, offset;
    struct sockaddr_in addr;
    char tb[32];
    double val;

    if (argc < 6) {
        fprintf(stderr, "Cach dung: %s <loai> <id> <host> <cong goc P> <chu ky ms> [--fragment|--glue|--corrupt]\n", argv[0]);
        return 1;
    }
    type = argv[1];
    id = atoi(argv[2]);
    base = atoi(argv[4]);
    period = atoi(argv[5]);
    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "--fragment") == 0) opt_fragment = 1;
        else if (strcmp(argv[i], "--glue") == 0) opt_glue = 1;
        else if (strcmp(argv[i], "--corrupt") == 0) opt_corrupt = 1;
    }

    if (strcmp(type, "temperature") == 0) { offset = 0; val = 18; }
    else if (strcmp(type, "humidity") == 0) { offset = 1; val = 50; }
    else if (strcmp(type, "motion") == 0) { offset = 2; val = 1; }
    else if (strcmp(type, "door") == 0) { offset = 2; val = 1; }
    else if (strcmp(type, "smoke_gas") == 0) { offset = 3; val = 100; }
    else { fprintf(stderr, "loai sensor khong dung\n"); return 1; }

    srand(time(NULL) ^ (getpid() << 8));

    sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(base + offset);
    inet_pton(AF_INET, argv[3], &addr.sin_addr);
    now_str(tb, sizeof(tb));
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("%s sensor %u %s connect failed (%s)\n", tb, id, type, strerror(errno));
        return 1;
    }
    printf("%s sensor %u %s connected to %s:%u\n", tb, id, type, argv[3], base + offset);
    fflush(stdout);

    /* timeout nhận ACK = 1s (cho SGP) */
    struct timeval tv = { 1, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        uint8_t buf[256];
        size_t len = 0;
        int nframes = opt_glue ? 3 : 1;    /* --glue: gộp 3 frame vào 1 lần send */
        int sleep_ms = period;
        int rc = 0;

        for (int k = 0; k < nframes; k++) {
            val = next_value(type, val);
            len += make_frame(type, id, val, buf + len);
        }

        if (offset == 3 && !opt_glue)
            rc = send_sgp(buf, len);
        else
            rc = send_bytes(buf, len);
        if (rc < 0) {
            printf("sensor %u: gateway dong ket noi\n", id);
            break;
        }

        /* SGP --glue: đọc bỏ các ACK/NAK */
        if (offset == 3 && opt_glue) {
            uint8_t r[8];
            recv(sock, r, sizeof(r), MSG_DONTWAIT);
        }

        /* motion / door: khoảng cách ngẫu nhiên quanh period */
        if (offset == 2)
            sleep_ms = period / 2 + rand() % (period + 1);
        usleep(sleep_ms * 1000);
    }
    close(sock);
    return 0;
}
