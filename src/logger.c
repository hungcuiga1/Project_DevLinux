/* logger.c - ghi log qua FIFO, log process ghi ra gateway.log */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include "logger.h"
#include "gateway.h"

static int log_fd = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int logger_make_fifo(void)
{
    /* nếu FIFO đã có rồi thì thôi */
    if (mkfifo(LOG_FIFO_NAME, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo");
        return -1;
    }
    return 0;
}

int logger_open_writer(void)
{
    /* open sẽ block cho đến khi log process mở đầu đọc */
    log_fd = open(LOG_FIFO_NAME, O_WRONLY);
    if (log_fd < 0) {
        perror("open fifo");
        return -1;
    }
    return 0;
}

void logger_close_writer(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_fd >= 0)
        close(log_fd);
    log_fd = -1;
    pthread_mutex_unlock(&log_lock);
}

void log_event(const char *src, const char *fmt, ...)
{
    char msg[256];
    char line[300];
    va_list ap;
    int len;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[200] = '\0';    /* thông điệp tối đa 200 byte */

    /* dạng gửi qua FIFO: "<nguồn> <thông điệp>\n" */
    len = snprintf(line, sizeof(line), "%s %s\n", src, msg);
    if (len > (int)sizeof(line) - 1)
        len = sizeof(line) - 1;

    DBG("[LOG] %s", line);

    /* 1 lần write, < PIPE_BUF nên là atomic.
     * mutex để các thread không dùng log_fd lúc đang bị close.
     * mutex này riêng, không lồng với lock của sbuffer */
    pthread_mutex_lock(&log_lock);
    if (log_fd >= 0) {
        if (write(log_fd, line, len) < 0)
            DBG("write log fifo loi: %s\n", strerror(errno));
    }
    pthread_mutex_unlock(&log_lock);
}

/* ===================== log process ===================== */
void log_process_run(void)
{
    FILE *fifo, *out;
    char buf[512];
    unsigned long seq = 1;

    /* Ctrl+C gửi SIGINT cho cả process group, log process bỏ qua
     * để còn ghi nốt log cuối cùng, nó sẽ thoát khi FIFO bị đóng hết (EOF) */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    logger_make_fifo();
    fifo = fopen(LOG_FIFO_NAME, "r");
    if (fifo == NULL) {
        perror("log process: open fifo");
        exit(1);
    }
    out = fopen(LOG_FILE_NAME, "w");
    if (out == NULL) {
        perror("log process: open gateway.log");
        fclose(fifo);
        exit(1);
    }

    /* fgets block khi chưa có dữ liệu, trả NULL khi tất cả bên ghi đã đóng */
    while (fgets(buf, sizeof(buf), fifo) != NULL) {
        char tbuf[32];
        time_t now = time(NULL);
        struct tm tm;

        localtime_r(&now, &tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

        /* buf đã có sẵn "\n" ở cuối */
        fprintf(out, "%lu %s %s", seq++, tbuf, buf);
        fflush(out);
    }

    fclose(out);
    fclose(fifo);
    exit(0);
}
