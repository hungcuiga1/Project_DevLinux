/* storagemgr.c - Storage manager: đọc sbuffer, ghi vào SQLite
 * Khi mất kết nối DB: thử lại tối đa 3 lần (mỗi lần cách nhau RETRY_DELAY giây),
 * vẫn lỗi thì cho gateway dừng. Trong lúc đó dữ liệu vẫn nằm trong sbuffer.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gateway.h"
#include "sbuffer.h"
#include "logger.h"

#define MAX_RETRY    3
#define RETRY_DELAY  2   /* giây */
#define TABLE_NAME   "SensorData"

static void db_close(storagemgr_t *sm)
{
    if (sm->insert)
        sqlite3_finalize(sm->insert);
    if (sm->db)
        sqlite3_close(sm->db);
    sm->insert = NULL;
    sm->db = NULL;
}

/* mở DB, tạo bảng nếu chưa có. first = lần đầu (được phép tạo file mới) */
static int db_connect(storagemgr_t *sm, bool first)
{
    sqlite3_stmt *chk;
    bool exists = false;
    int flags = SQLITE_OPEN_READWRITE;

    /* lần đầu thì tạo file, các lần reconnect thì không (để phát hiện file bị đổi tên/xoá) */
    if (first)
        flags |= SQLITE_OPEN_CREATE;

    if (sqlite3_open_v2(sm->db_path, &sm->db, flags, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_busy_timeout(sm->db, 1000);

    /* kiểm tra bảng đã có chưa */
    if (sqlite3_prepare_v2(sm->db, "SELECT name FROM sqlite_master WHERE type='table' AND name='" TABLE_NAME "';",
                           -1, &chk, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_step(chk) == SQLITE_ROW)
        exists = true;
    sqlite3_finalize(chk);

    if (!exists) {
        const char *sql = "CREATE TABLE " TABLE_NAME " ("
                          "id INTEGER NOT NULL, type INTEGER NOT NULL, "
                          "value REAL NOT NULL, ts INTEGER NOT NULL);";
        if (sqlite3_exec(sm->db, sql, NULL, NULL, NULL) != SQLITE_OK)
            goto fail;
    }

    if (sqlite3_prepare_v2(sm->db, "INSERT INTO " TABLE_NAME " (id, type, value, ts) VALUES (?, ?, ?, ?);",
                           -1, &sm->insert, NULL) != SQLITE_OK)
        goto fail;

    log_event(SRC_STOR, "Connection to SQL server established.");
    if (!exists)
        log_event(SRC_STOR, "New table " TABLE_NAME " created.");
    return 0;

fail:
    DBG("storagemgr: loi db: %s\n", sm->db ? sqlite3_errmsg(sm->db) : "?");
    db_close(sm);
    return -1;
}

/* thử kết nối tối đa MAX_RETRY lần. trả -1 nếu vẫn lỗi */
static int db_reconnect(storagemgr_t *sm, bool first)
{
    int i;
    for (sm->attempts = 1; sm->attempts <= MAX_RETRY; sm->attempts++) {
        if (db_connect(sm, first) == 0) {
            sm->state = SQL_CONNECTED;
            return 0;
        }
        DBG("storagemgr: ket noi that bai lan %d\n", sm->attempts);
        /* chờ rồi thử lại, chia nhỏ để còn thoát nhanh khi Ctrl+C */
        for (i = 0; i < RETRY_DELAY * 10 && !g_gw.stop; i++)
            usleep(100000);
        if (g_gw.stop)
            break;
    }
    sm->state = SQL_DOWN;
    log_event(SRC_STOR, "Unable to connect to SQL server.");
    return -1;
}

static int db_insert(storagemgr_t *sm, const sensor_data_t *d)
{
    int rc;
    /* file DB bị xoá / đổi tên thì coi như mất kết nối */
    if (access(sm->db_path, F_OK) != 0)
        return -1;
    sqlite3_reset(sm->insert);
    sqlite3_bind_int(sm->insert, 1, d->id);
    sqlite3_bind_int(sm->insert, 2, d->type);
    sqlite3_bind_double(sm->insert, 3, d->value);
    sqlite3_bind_int64(sm->insert, 4, (sqlite3_int64)d->ts);
    rc = sqlite3_step(sm->insert);
    return rc == SQLITE_DONE ? 0 : -1;
}

void *storagemgr_thread(void *arg)
{
    storagemgr_t *sm = &g_gw.stor;
    sensor_data_t d;
    size_t sz;
    (void)arg;

    log_event(SRC_STOR, "Storage manager started");
    sm->db_path = DB_FILE_NAME;
    sm->db = NULL;
    sm->insert = NULL;

    if (db_reconnect(sm, true) < 0) {
        g_gw.stop_reason = "unable to connect to SQL server";
        g_gw.stop = 1;
        return NULL;
    }

    while (sbuffer_read(&g_gw.sbuf, READER_STORAGE, &d, &sz) == 0) {
        DBG("storagemgr: doc id=%u type=%d value=%.2f ts=%ld\n", d.id, d.type, d.value, (long)d.ts);

        /* insert lỗi -> mất kết nối -> reconnect rồi ghi lại chính phép đo này */
        while (db_insert(sm, &d) < 0) {
            log_event(SRC_STOR, "Connection to SQL server lost.");
            db_close(sm);
            sm->state = SQL_LOST;
            if (db_reconnect(sm, false) < 0) {
                g_gw.stop_reason = "unable to connect to SQL server";
                g_gw.stop = 1;
                return NULL;
            }
        }
        DBG("storagemgr: da luu id=%u (sbuffer=%zu)\n", d.id, sz);
    }

    db_close(sm);
    DBG("storagemgr: thoat\n");
    return NULL;
}
