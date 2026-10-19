/* gateway.h - các cấu trúc dữ liệu dùng chung (mục 3.9 của đề, phần Step 0-7) */
#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sqlite3.h>

#include "sensor_types.h"
#include "sensor_config.h"
#include "protocol_adapter.h"

/* in debug, chỉ bật khi build với -DDEBUG (make debug) */
#ifdef DEBUG
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) do { } while (0)
#endif

/* tên các file / tài nguyên */
#define LOG_FIFO_NAME   "logFifo"
#define LOG_FILE_NAME   "gateway.log"
#define DB_FILE_NAME    "sensor.db"
#define ROOM_FILE_NAME  "room_sensor.map"

/* ---- Hằng số và kiểu liệt kê dùng chung ---- */
#define RX_BUF_SIZE  256
#define MAX_CONN      64
#define MAX_SENSORS   64
typedef enum { STATE_COLLECTING, STATE_NORMAL, STATE_ALERT } sensor_state_t;
typedef enum { SQL_CONNECTED, SQL_LOST, SQL_DOWN } sql_state_t;

/* ---- Sensor ---- */
typedef struct {              /* một dòng của file room-sensor */
    uint16_t      room;
    sensor_id_t   id;
    sensor_type_t type;
} sensor_info_t;

/* ---- sbuffer ---- */
typedef struct sbuffer_node {
    sensor_data_t        data;
    bool                 read_by_data;      /* Data manager đã đọc    */
    bool                 read_by_storage;   /* Storage manager đã đọc */
    struct sbuffer_node *next;
} sbuffer_node_t;

typedef struct {
    sbuffer_node_t *head, *tail;
    size_t          size;
    bool            shutdown;               /* báo kết thúc           */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;              /* đánh thức thread đọc   */
} sbuffer_t;

/* ---- Connection manager ---- */
typedef struct {
    int                       fd;
    const protocol_adapter_t *adapter;
    uint8_t                   rx[RX_BUF_SIZE];  /* bộ đệm nhận riêng    */
    size_t                    rx_len;
    bool                      identified;       /* đã biết sensor ID    */
    sensor_id_t               sensor_id;
    int                       err_count;        /* tự thêm: số lỗi liên tiếp */
} connection_t;

typedef struct {
    int          listen_fd[4];                  /* cổng P .. P+3        */
    connection_t conns[MAX_CONN];
    size_t       n_conns;
} connmgr_t;

/* ---- Data manager ---- */
typedef struct {
    sensor_info_t  info;
    double         window[RUN_AVG_LENGTH];      /* vòng đệm giá trị trung binh */
    size_t         count, next;
    double         running_avg;
    sensor_state_t state;
    /* tự thêm */
    int            last_alert;   /* 0: không, 1: thấp (lạnh/khô), 2: cao (nóng/ẩm/khói) */
    int            door_state;   /* -1 = chưa biết */
} datamgr_sensor_t;

typedef struct { datamgr_sensor_t sensors[MAX_SENSORS]; size_t n; } datamgr_t;
typedef void (*sensor_handler_t)(datamgr_t *, datamgr_sensor_t *, const sensor_data_t *);

/* ---- Storage manager + SQLite ---- */
typedef struct {
    sqlite3      *db;
    sqlite3_stmt *insert;
    sql_state_t   state;
    int           attempts;
    const char   *db_path;
} storagemgr_t;

/* ---- Ngữ cảnh gốc của main process ---- */
typedef struct {
    uint16_t        base_port, http_port;
    sbuffer_t       sbuf;
    connmgr_t       conn;
    datamgr_t       data;
    storagemgr_t    stor;
    void           *status;      /* NULL cho tới Step 8 (giữa kỳ chưa làm) */
    pid_t           log_pid, web_pid;
    volatile sig_atomic_t stop;
    const char     *stop_reason; /* tự thêm: lý do kết thúc để ghi log */
} gateway_t;

/* biến toàn cục duy nhất, dùng cho signal handler */
extern gateway_t g_gw;

/* các hàm thread */
void *connmgr_thread(void *arg);
void *datamgr_thread(void *arg);
void *storagemgr_thread(void *arg);

/* data manager: nạp file room-sensor (gọi trước khi tạo thread) */
int datamgr_load_rooms(datamgr_t *dm, const char *path);

#endif
