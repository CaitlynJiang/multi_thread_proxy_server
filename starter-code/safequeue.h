#include <stdlib.h>
#include <pthread.h>

typedef struct node
{
    struct http_request *data;

    int priority;

    struct node* next;
};

// priority determined by the directory name (file 10 > file 1)
typedef struct safequeue
{
    struct node* head;

    int capacity; // max size of queue specified using the -q flag

    int size;

    pthread_mutex_t lock;
};

void pq_init(struct safequeue *pq, int capacity);
int get_priority(struct node* req);
int pq_add(struct safequeue *pq, struct node *req);
struct node *pq_poll(struct safequeue *pq);
void pq_free(struct safequeue *pq);
