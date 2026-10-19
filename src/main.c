/* main.c - Sensor gateway (giữa kỳ: Step 0-7)
 * Cách chạy: ./server <cổng gốc P> <cổng HTTP>   vd: ./server 1234 8080
 * (cổng HTTP chỉ đọc và kiểm tra, web process làm ở cuối kỳ)
 *
 * Thứ tự khởi động:
 *   1. tạo FIFO, fork log process
 *   2. nạp file room-sensor, init sbuffer
 *   3. tạo 3 thread: connmgr, datamgr, storagemgr
 *   4. chờ cờ stop (Ctrl+C / SIGTERM / mất SQL) rồi dọn dẹp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "gateway.h"
#include "sbuffer.h"
#include "logger.h"

gateway_t g_gw;

/* signal handler chỉ set cờ, việc dọn dẹp làm ở main */
static void on_signal(int sig)
{
    (void)sig;
    g_gw.stop = 1;
}

static int parse_port(const char *s, uint16_t *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v <= 0 || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

int main(int argc, char *argv[])
{
    pthread_t t_conn, t_data, t_stor;
    struct sigaction sa;

    if (argc != 3 || parse_port(argv[1], &g_gw.base_port) < 0 ||
        parse_port(argv[2], &g_gw.http_port) < 0 || g_gw.base_port > 65532) {
        fprintf(stderr, "Cach dung: %s <cong goc P> <cong HTTP>\n", argv[0]);
        fprintf(stderr, "  vd: %s 1234 8080\n", argv[0]);
        return 1;
    }
    g_gw.stop = 0;
    g_gw.stop_reason = "SIGINT/SIGTERM";
    g_gw.status = NULL;     /* Step 8 mới dùng */
    g_gw.web_pid = -1;      /* Step 9 mới dùng */

    /* bắt SIGINT, SIGTERM; bỏ qua SIGPIPE (sensor đóng kết nối khi đang gửi ACK) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- 1. log process ---- */
    if (logger_make_fifo() < 0)
        return 1;
    g_gw.log_pid = fork();
    if (g_gw.log_pid < 0) {
        perror("fork log");
        return 1;
    }
    if (g_gw.log_pid == 0)
        log_process_run();      /* không return */
    if (logger_open_writer() < 0)
        return 1;
    log_event(SRC_MAIN, "Gateway started (TCP base port %u, HTTP port %u)", g_gw.base_port, g_gw.http_port);

    /* ---- 2. room-sensor + sbuffer ---- */
    if (datamgr_load_rooms(&g_gw.data, ROOM_FILE_NAME) < 0)
        fprintf(stderr, "canh bao: khong doc duoc %s, moi sensor se bi coi la khong hop le\n", ROOM_FILE_NAME);
    sbuffer_init(&g_gw.sbuf);

    /* ---- 3. 3 thread ---- */
    pthread_create(&t_conn, NULL, connmgr_thread, NULL);
    pthread_create(&t_data, NULL, datamgr_thread, NULL);
    pthread_create(&t_stor, NULL, storagemgr_thread, NULL);
    DBG("main: da tao 3 thread\n");

    /* ---- 4. chờ cờ stop ---- */
    /* kiểm tra cờ mỗi 1 giây (cờ có thể do signal hoặc do storagemgr bật khi mất SQL) */
    while (!g_gw.stop)
        sleep(1);

    log_event(SRC_MAIN, "Gateway shutting down (%s)", g_gw.stop_reason);
    DBG("main: dang dung cac thread...\n");

    /* báo các thread dừng: connmgr tự thấy cờ stop, 2 thread đọc thì đánh thức qua sbuffer */
    sbuffer_shutdown(&g_gw.sbuf);
    pthread_join(t_conn, NULL);
    pthread_join(t_data, NULL);
    pthread_join(t_stor, NULL);

    /* đóng FIFO -> log process đọc được EOF và tự thoát, wait để không zombie */
    logger_close_writer();
    waitpid(g_gw.log_pid, NULL, 0);

    sbuffer_free(&g_gw.sbuf);
    unlink(LOG_FIFO_NAME);
    printf("Gateway da dung.\n");
    return 0;
}
