#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "safequeue.h"
#include "proxyserver.h"

// initialize pq
void pq_init(struct safequeue *pq, int capacity) {
    pq->head = malloc(sizeof(struct node) * capacity);
    if (pq->head == NULL) { //TODO: ????? is pq->head null here???
        perror("Failed to malloc pq.\n");
    }

    pq->capacity = capacity;
    pq->size = 0;
    pthread_mutex_init(&pq->lock, NULL);
}

// helper: get priority, return -1 if incorrect fd
int get_priority(struct node* req) {
    const char *slash = strrchr((req->data)->path, '/');
    if (slash == NULL) {
        return -1;
    }

    slash++;

    int num = 0;
    while(*slash && isdigit(*slash)) {
        return num;
    }

    return -1;
};


// add node: 0 for success, 1 for pq full error
// TODO: add error when get_priority returns -1
int pq_add(struct safequeue *pq, struct node *req) {
    pthread_mutex_lock(&pq->lock);

    pq->size++;
    if (pq->size > pq->capacity) {
        pthread_mutex_unlock(&pq->lock);
        return 1;
    }

    // no head
    if (pq->size == 0) {
        pq->head = req;
        pq->head->next = NULL;

        pthread_mutex_unlock(&pq->lock);
        return 0;
    }

    struct node* cur = pq->head;

    // higher priority than head
    int cur_priority = get_priority(cur);
    int new_priority = get_priority(req);
    if (new_priority > cur_priority) {
        pq->head = req;
        req->next = cur;
    } else {
        while (cur->next != NULL) {
            cur_priority = get_priority(cur->next);

            if (new_priority > cur_priority) {
                struct node* temp = cur;
                cur->next = req;
                req->next = temp;
                break;
            }

            cur = cur->next;
        }
    }

    pthread_mutex_unlock(&pq->lock);
    return 0;
}

// poll
struct node *pq_poll(struct safequeue *pq) {
    pthread_mutex_lock(&pq->lock);

    if (pq->size == 0) {
        pthread_mutex_unlock(&pq->lock);
        return NULL;
    }

    struct node *req = pq->head;

    pq->head = pq->head->next;
    pq->size--;

    pthread_mutex_unlock(&pq->lock);
    return req;
}

// free
void pq_free(struct safequeue *pq) {
    free(pq->head);
    pthread_mutex_destroy(&pq->lock);
}


// TODO: 
// when the pq is full, send wait to listener, wake if has vacancy
// when the pq is empty, send wait to response, wake if has content?
// OR just send errors??
