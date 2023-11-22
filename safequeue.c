#include "safequeue.h"

// Create a new priority queue
SafeQueue* create_queue(int capacity) {
    SafeQueue* queue = malloc(sizeof(SafeQueue));
    if (!queue) return NULL;
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    queue->capacity = capacity;
    return queue;
}

// Add work to the priority queue
int add_work(SafeQueue *queue, int *client_fd, int priority) {
    if (queue->size >= queue->capacity) {
        return -1;
    }

    node_t *newnode = malloc(sizeof(node_t));
    if (!newnode) return -1;
    newnode->client_fd = client_fd;
    newnode->priority = priority;
    newnode->next = NULL;

    if (!queue->head || queue->head->priority < priority) {
        newnode->next = queue->head;
        queue->head = newnode;
        if (!queue->tail) queue->tail = newnode;
    } else {
        node_t *current = queue->head;
        while (current->next && current->next->priority >= priority) {
            current = current->next;
        }
        newnode->next = current->next;
        current->next = newnode;
        if (current == queue->tail) queue->tail = newnode;
    }
    queue->size++;

    return 0;
}

// Get the job with the highest priority
int* get_work(SafeQueue *queue) {
    if (!queue->head) return NULL;

    node_t *temp = queue->head;
    int *result = temp->client_fd;
    queue->head = queue->head->next;
    if (!queue->head) queue->tail = NULL;
    free(temp);
    queue->size--;
    return result;
}

// Non-blocking version to get the highest priority job
int* get_work_nonblocking(SafeQueue *queue) {
    return get_work(queue); 
    // get_work itslef is non-blocking bcs cv is implementing in server code
}
