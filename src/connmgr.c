/* connmgr.c - Connection manager
 * Dùng 1 thread + poll() để phục vụ 4 socket lắng nghe và tất cả kết nối.
 * Lý do: đơn giản, không phải tạo 1 thread cho mỗi sensor, ít context switch.
 * Đánh đổi: nếu xử lý 1 kết nối chậm thì kết nối khác phải chờ (ở đây xử lý rất nhanh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "gateway.h"
#include "sbuffer.h"
#include "logger.h"

#define MAX_ERR_IN_ROW 50   /* lỗi liên tiếp quá nhiều thì đóng kết nối (EVP dò lại từng byte nên để lớn) */

static int open_listen(uint16_t port)
{
    struct sockaddr_in addr;
    int opt = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void close_conn(connmgr_t *cm, size_t idx)
{
    connection_t *c = &cm->conns[idx];
    if (c->identified)
        log_event(SRC_CONN, "The sensor node with %u has closed the connection", c->sensor_id);
    DBG("connmgr: dong ket noi fd=%d\n", c->fd);
    close(c->fd);
    /* đưa phần tử cuối lên thay chỗ */
    cm->conns[idx] = cm->conns[cm->n_conns - 1];
    cm->n_conns--;
}

static void accept_new(connmgr_t *cm, int lfd, const protocol_adapter_t *ad)
{
    connection_t *c;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) {
        log_event(SRC_CONN, "Connection attempt failed on %s port (accept error)", ad->name);
        return;
    }
    if (cm->n_conns >= MAX_CONN) {
        log_event(SRC_CONN, "Connection attempt failed on %s port (too many connections)", ad->name);
        close(fd);
        return;
    }
    c = &cm->conns[cm->n_conns++];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->adapter = ad;
    DBG("connmgr: ket noi moi tren cong %s fd=%d\n", ad->name, fd);
}

/* xử lý dữ liệu trong bộ đệm của 1 kết nối. trả -1 nếu cần đóng kết nối */
static int handle_rx(connection_t *c)
{
    const protocol_adapter_t *ad = c->adapter;

    while (c->rx_len > 0) {
        sensor_data_t d;
        size_t used = 0;
        uint8_t reply[4];
        size_t rlen, sz;
        parse_result_t r = ad->parse(c->rx, c->rx_len, &d, &used);

        if (r == PARSE_NEED_MORE)
            break;      /* chờ recv thêm */

        /* gửi ACK/NAK nếu giao thức có (SGP) */
        rlen = ad->build_reply(r, reply, sizeof(reply));
        if (rlen > 0)
            send(c->fd, reply, rlen, MSG_NOSIGNAL);

        if (r == PARSE_OK) {
            c->err_count = 0;
            if (!c->identified) {
                /* frame hợp lệ đầu tiên mới biết sensor ID */
                c->identified = true;
                c->sensor_id = d.id;
                log_event(SRC_CONN, "A sensor node with %u has opened a new connection", d.id);
            }
            /* Frame SGP bị gửi lại (cùng id + ts): em chọn LƯU CẢ HAI cho đơn giản và nhất quán.
             * Lý do: ts chỉ tính bằng giây, sensor gửi nhanh (< 1s) thì 2 phép đo khác nhau
             * cũng có thể trùng ts, nếu bỏ qua sẽ mất dữ liệu thật. */
            sz = sbuffer_insert(&g_gw.sbuf, &d);
            DBG("connmgr: nhan id=%u type=%d value=%.2f (sbuffer=%zu)\n", d.id, d.type, d.value, sz);
            (void)sz;
        } else {
            /* PARSE_ERROR */
            if (!c->identified)
                log_event(SRC_CONN, "Connection attempt failed on %s port (malformed first frame)", ad->name);
            else
                log_event(SRC_CONN, "Received malformed frame on %s port (%s)", ad->name, g_parse_reason);
            c->err_count++;
            if (c->err_count >= MAX_ERR_IN_ROW)
                return -1;
            if (used == 0)
                used = 1;   /* cho chắc, không bị lặp vô hạn */
        }

        /* bỏ các byte đã dùng ra khỏi bộ đệm */
        if (used > c->rx_len)
            used = c->rx_len;
        memmove(c->rx, c->rx + used, c->rx_len - used);
        c->rx_len -= used;
    }
    return 0;
}

void *connmgr_thread(void *arg)
{
    connmgr_t *cm = &g_gw.conn;
    struct pollfd pfd[NUM_ADAPTERS + MAX_CONN];
    size_t i;
    (void)arg;

    log_event(SRC_CONN, "Connection manager started");

    for (i = 0; i < NUM_ADAPTERS; i++) {
        cm->listen_fd[i] = open_listen(g_gw.base_port + ADAPTERS[i].port_offset);
        if (cm->listen_fd[i] < 0) {
            fprintf(stderr, "khong mo duoc cong %u\n", g_gw.base_port + ADAPTERS[i].port_offset);
            g_gw.stop_reason = "cannot open TCP port";
            g_gw.stop = 1;
        }
    }
    cm->n_conns = 0;

    while (!g_gw.stop) {
        size_t n = 0;
        int ret;

        /* 4 socket lắng nghe ở đầu, sau đó là các kết nối */
        for (i = 0; i < NUM_ADAPTERS; i++) {
            pfd[n].fd = cm->listen_fd[i];
            pfd[n].events = POLLIN;
            n++;
        }
        for (i = 0; i < cm->n_conns; i++) {
            pfd[n].fd = cm->conns[i].fd;
            pfd[n].events = POLLIN;
            n++;
        }

        /* timeout 500ms để còn kiểm tra cờ stop */
        ret = poll(pfd, n, 500);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (ret == 0)
            continue;

        /* duyệt ngược các kết nối để xoá phần tử không làm lệch index */
        for (i = n; i-- > NUM_ADAPTERS; ) {
            size_t ci = i - NUM_ADAPTERS;
            connection_t *c = &cm->conns[ci];
            ssize_t r;

            if (pfd[i].revents == 0)
                continue;

            r = recv(c->fd, c->rx + c->rx_len, RX_BUF_SIZE - c->rx_len, 0);
            if (r <= 0) {
                close_conn(cm, ci);    /* sensor đóng kết nối hoặc lỗi */
                continue;
            }
            c->rx_len += r;
            if (handle_rx(c) < 0)
                close_conn(cm, ci);
        }

        /* kết nối mới */
        for (i = 0; i < NUM_ADAPTERS; i++) {
            if (pfd[i].revents & POLLIN)
                accept_new(cm, cm->listen_fd[i], &ADAPTERS[i]);
        }
    }

    /* dọn dẹp: đóng hết socket */
    while (cm->n_conns > 0)
        close_conn(cm, cm->n_conns - 1);
    for (i = 0; i < NUM_ADAPTERS; i++)
        if (cm->listen_fd[i] >= 0)
            close(cm->listen_fd[i]);

    DBG("connmgr: thoat\n");
    return NULL;
}
