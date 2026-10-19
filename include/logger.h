#ifndef LOGGER_H
#define LOGGER_H

/* nguồn log */
#define SRC_MAIN "MAIN"
#define SRC_CONN "CONN"
#define SRC_DATA "DATA"
#define SRC_STOR "STOR"

/* tạo FIFO nếu chưa có */
int  logger_make_fifo(void);
/* phía ghi (main process) */
int  logger_open_writer(void);
void logger_close_writer(void);
void log_event(const char *src, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* thân của log process (process con) */
void log_process_run(void);

#endif
