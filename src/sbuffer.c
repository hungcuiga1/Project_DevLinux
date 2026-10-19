/* sbuffer.c - hàng đợi dùng chung giữa 3 thread
 * - connmgr ghi vào cuối
 * - datamgr và storagemgr cùng đọc, mỗi node có 2 cờ đã đọc
 * - node chỉ bị xoá khi cả 2 cờ đều bật
 * Vì mỗi reader đọc theo thứ tự nên các node đã đọc đủ 2 cờ luôn nằm ở đầu list
 * => chỉ cần xoá từ head.
 */
#include <stdlib.h>
#include "sbuffer.h"

int sbuffer_init(sbuffer_t *sb)
{
    sb->head = sb->tail = NULL;
    sb->size = 0;
    sb->shutdown = false;
    if (pthread_mutex_init(&sb->lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&sb->not_empty, NULL) != 0)
        return -1;
    return 0;
}

void sbuffer_free(sbuffer_t *sb)
{
    sbuffer_node_t *n = sb->head;
    while (n) {
        sbuffer_node_t *next = n->next;
        free(n);
        n = next;
    }
    sb->head = sb->tail = NULL;
    sb->size = 0;
    pthread_mutex_destroy(&sb->lock);
    pthread_cond_destroy(&sb->not_empty);
}

size_t sbuffer_insert(sbuffer_t *sb, const sensor_data_t *data)
{
    size_t size;
    /* malloc ngoài lock cho vùng găng ngắn */
    sbuffer_node_t *n = malloc(sizeof(*n));
    if (n == NULL)
        return 0;
    n->data = *data;
    n->read_by_data = false;
    n->read_by_storage = false;
    n->next = NULL;

    pthread_mutex_lock(&sb->lock);
    if (sb->tail == NULL) {
        sb->head = sb->tail = n;
    } else {
        sb->tail->next = n;
        sb->tail = n;
    }
    sb->size++;
    size = sb->size;
    /* broadcast vì có 2 thread đọc đang chờ */
    pthread_cond_broadcast(&sb->not_empty);
    pthread_mutex_unlock(&sb->lock);
    return size;
}

/* tìm node đầu tiên mà reader chưa đọc (gọi khi đang giữ lock) */
static sbuffer_node_t *find_unread(sbuffer_t *sb, sbuffer_reader_t who)
{
    sbuffer_node_t *n = sb->head;
    while (n) {
        if (who == READER_DATA && !n->read_by_data)
            return n;
        if (who == READER_STORAGE && !n->read_by_storage)
            return n;
        n = n->next;
    }
    return NULL;
}

int sbuffer_read(sbuffer_t *sb, sbuffer_reader_t who, sensor_data_t *out, size_t *size_out)
{
    sbuffer_node_t *n;
    sbuffer_node_t *to_free = NULL;

    pthread_mutex_lock(&sb->lock);
    /* chờ đến khi có dữ liệu chưa đọc hoặc shutdown (không polling) */
    while ((n = find_unread(sb, who)) == NULL && !sb->shutdown)
        pthread_cond_wait(&sb->not_empty, &sb->lock);

    if (n == NULL) {
        /* shutdown và không còn gì để đọc */
        pthread_mutex_unlock(&sb->lock);
        return -1;
    }

    *out = n->data;
    if (who == READER_DATA)
        n->read_by_data = true;
    else
        n->read_by_storage = true;

    /* xoá các node ở đầu đã được cả 2 thread đọc */
    while (sb->head && sb->head->read_by_data && sb->head->read_by_storage) {
        sbuffer_node_t *h = sb->head;
        sb->head = h->next;
        if (sb->head == NULL)
            sb->tail = NULL;
        sb->size--;
        h->next = to_free;      /* gom lại, free sau khi nhả lock */
        to_free = h;
    }
    if (size_out)
        *size_out = sb->size;
    pthread_mutex_unlock(&sb->lock);

    while (to_free) {
        sbuffer_node_t *nx = to_free->next;
        free(to_free);
        to_free = nx;
    }
    return 0;
}

void sbuffer_shutdown(sbuffer_t *sb)
{
    pthread_mutex_lock(&sb->lock);
    sb->shutdown = true;
    pthread_cond_broadcast(&sb->not_empty);
    pthread_mutex_unlock(&sb->lock);
}
