#include <stdlib.h>
#include <pthread.h>
#include "proxyserver.h"

typedef struct Node
{
    struct http_request *data;
    int priority;
} Node;

// priority determined by the directory name (file 10 > file 1)
typedef struct Safequeue
{
    struct Node* reqs;

    int capacity; // max size of queue specified using the -q flag
    int size;

    pthread_mutex_t lock;
    pthread_cond_t cond; 
} Safequeue;

Safequeue *create_queue(int capacity);
int get_priority(struct http_request* req);
int add_work(Safequeue *pq, struct http_request *req);
Node *get_work(Safequeue *pq);
Node *get_work_nonblocking(Safequeue *pq);
