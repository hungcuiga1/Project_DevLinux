/* datamgr.c - Data manager: đọc sbuffer, kiểm tra sensor ID, xử lý theo loại sensor
 * Trạng thái trung bình là dữ liệu riêng của thread này nên không cần khoá.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gateway.h"
#include "sbuffer.h"
#include "logger.h"

/* đổi chuỗi loại sensor trong file sang enum */
static int type_from_str(const char *s)
{
    if (strcmp(s, "temperature") == 0) return SENSOR_TEMPERATURE;
    if (strcmp(s, "humidity") == 0)    return SENSOR_HUMIDITY;
    if (strcmp(s, "motion") == 0)      return SENSOR_MOTION;
    if (strcmp(s, "door") == 0)        return SENSOR_DOOR;
    if (strcmp(s, "smoke_gas") == 0)   return SENSOR_SMOKE_GAS;
    return -1;
}

int datamgr_load_rooms(datamgr_t *dm, const char *path)
{
    char line[128];
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    dm->n = 0;
    while (fgets(line, sizeof(line), f) && dm->n < MAX_SENSORS) {
        unsigned room, id;
        char tstr[32];
        int type;
        datamgr_sensor_t *s;

        if (line[0] == '#' || line[0] == '\n')
            continue;   /* bỏ dòng chú thích / dòng trống */
        if (sscanf(line, "%u %u %31s", &room, &id, tstr) != 3)
            continue;
        type = type_from_str(tstr);
        if (type < 0) {
            fprintf(stderr, "room file: loai sensor la '%s'\n", tstr);
            continue;
        }
        s = &dm->sensors[dm->n++];
        memset(s, 0, sizeof(*s));
        s->info.room = room;
        s->info.id = id;
        s->info.type = type;
        s->state = STATE_COLLECTING;
        s->door_state = -1;
    }
    fclose(f);
    DBG("datamgr: nap %zu sensor tu %s\n", dm->n, path);
    return 0;
}

static datamgr_sensor_t *find_sensor(datamgr_t *dm, sensor_id_t id)
{
    size_t i;
    for (i = 0; i < dm->n; i++)
        if (dm->sensors[i].info.id == id)
            return &dm->sensors[i];
    return NULL;
}

/* thêm giá trị vào vòng đệm, tính trung bình. trả false nếu chưa đủ 5 mẫu */
static bool push_avg(datamgr_sensor_t *s, double v)
{
    double sum = 0;
    size_t i;
    s->window[s->next] = v;
    s->next = (s->next + 1) % RUN_AVG_LENGTH;
    if (s->count < RUN_AVG_LENGTH)
        s->count++;
    if (s->count < RUN_AVG_LENGTH)
        return false;
    for (i = 0; i < RUN_AVG_LENGTH; i++)
        sum += s->window[i];
    s->running_avg = sum / RUN_AVG_LENGTH;
    return true;
}

/* ---------- các handler theo loại sensor ---------- */
static void handle_temperature(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *d)
{
    int alert = 0;
    (void)dm;
    if (!push_avg(s, d->value)) {
        s->state = STATE_COLLECTING;
        return;
    }
    if (s->running_avg < TEMP_MIN_C)
        alert = 1;
    else if (s->running_avg > TEMP_MAX_C)
        alert = 2;

    /* chỉ log khi chuyển sang trạng thái cảnh báo, không log lặp lại */
    if (alert != 0 && alert != s->last_alert) {
        log_event(SRC_DATA, "The sensor node with %u reports it's too %s (running avg temperature = %.1f)",
                  d->id, alert == 1 ? "cold" : "hot", s->running_avg);
    }
    s->last_alert = alert;
    s->state = alert ? STATE_ALERT : STATE_NORMAL;
}

static void handle_humidity(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *d)
{
    int alert = 0;
    (void)dm;
    if (!push_avg(s, d->value)) {
        s->state = STATE_COLLECTING;
        return;
    }
    if (s->running_avg < HUMIDITY_MIN)
        alert = 1;
    else if (s->running_avg > HUMIDITY_MAX)
        alert = 2;

    if (alert != 0 && alert != s->last_alert) {
        log_event(SRC_DATA, "The sensor node with %u reports it's too %s (running avg humidity = %.1f)",
                  d->id, alert == 1 ? "dry" : "humid", s->running_avg);
    }
    s->last_alert = alert;
    s->state = alert ? STATE_ALERT : STATE_NORMAL;
}

static void handle_motion(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *d)
{
    (void)dm;
    /* mỗi sự kiện đều ghi log */
    log_event(SRC_DATA, "The sensor node with %u detected motion", d->id);
    s->state = STATE_ALERT;
}

static void handle_door(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *d)
{
    int st = (int)d->value;
    (void)dm;
    /* chỉ log khi trạng thái cửa thay đổi */
    if (st != s->door_state) {
        log_event(SRC_DATA, "The sensor node with %u reports the door is %s",
                  d->id, st ? "open" : "closed");
        s->door_state = st;
    }
    s->state = st ? STATE_ALERT : STATE_NORMAL;
}

static void handle_smoke(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *d)
{
    (void)dm;
    /* so ngưỡng tức thời, không lấy trung bình vì cần phản ứng ngay */
    if (d->value >= GAS_ALARM_PPM) {
        if (s->last_alert == 0)
            log_event(SRC_DATA, "The sensor node with %u reports a smoke/gas alarm (concentration = %.0f ppm)",
                      d->id, d->value);
        s->last_alert = 2;
        s->state = STATE_ALERT;
    } else {
        s->last_alert = 0;
        s->state = STATE_NORMAL;
    }
}

/* bảng handler, index = sensor_type_t. Thêm loại mới chỉ thêm 1 dòng */
static const sensor_handler_t HANDLERS[] = {
    NULL,                   /* 0 không dùng */
    handle_temperature,     /* SENSOR_TEMPERATURE */
    handle_humidity,        /* SENSOR_HUMIDITY */
    handle_motion,          /* SENSOR_MOTION */
    handle_door,            /* SENSOR_DOOR */
    handle_smoke,           /* SENSOR_SMOKE_GAS */
};
#define NUM_HANDLERS (sizeof(HANDLERS) / sizeof(HANDLERS[0]))

void *datamgr_thread(void *arg)
{
    datamgr_t *dm = &g_gw.data;
    sensor_data_t d;
    size_t sz;
    (void)arg;

    log_event(SRC_DATA, "Data manager started");

    while (sbuffer_read(&g_gw.sbuf, READER_DATA, &d, &sz) == 0) {
        datamgr_sensor_t *s = find_sensor(dm, d.id);

        DBG("datamgr: doc id=%u type=%d value=%.2f ts=%ld\n", d.id, d.type, d.value, (long)d.ts);

        /* ID không có trong file hoặc loại sensor không khớp */
        if (s == NULL || s->info.type != d.type || (size_t)d.type >= NUM_HANDLERS) {
            log_event(SRC_DATA, "Received sensor data with invalid sensor node ID %u", d.id);
            continue;
        }
        HANDLERS[d.type](dm, s, &d);
        DBG("datamgr: sensor %u phong %u state=%d avg=%.2f (sbuffer=%zu)\n",
            d.id, s->info.room, s->state, s->running_avg, sz);
    }

    DBG("datamgr: thoat\n");
    return NULL;
}
