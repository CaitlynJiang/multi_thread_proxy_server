#ifndef SAFEQUEUE_H_
#define SAFEQUEUE_H_

#include <stdlib.h>

// Node structure
typedef struct node {
    struct node* next;
    int *client_fd;
    int priority;
} node_t;

// Priority queue structure
typedef struct {
    node_t* head;
    node_t* tail;
    int size;
    int capacity;
} SafeQueue;

// Function prototypes
SafeQueue* create_queue(int capacity);
int add_work(SafeQueue *queue, int *client_fd, int priority);
int* get_work(SafeQueue *queue);
int* get_work_nonblocking(SafeQueue *queue);

#endif // SAFEQUEUE_H_
