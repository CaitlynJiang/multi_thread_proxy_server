#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>

#include "safequeue.h"
#include "proxyserver.h"

// initialize pq
Safequeue *create_queue(int capacity) {
    Safequeue *pq = malloc(sizeof(Safequeue));
    if (pq == NULL) { 
        perror("Failed to malloc pq.\n");
        return NULL; // Early return if allocation fails
    }

    pq->reqs = malloc(sizeof(Node) * capacity);
    if (pq->reqs == NULL) { 
        perror("Failed to malloc nodes.\n");
        free(pq); // Free the previously allocated pq before returning
        return NULL;
    }

    pq->capacity = capacity;
    pq->size = 0;
    pthread_mutex_init(&pq->lock, NULL);
    pthread_cond_init(&pq->cond, NULL);
    return pq;
}


// helper: get priority, return -1 if incorrect fd
int get_priority(struct http_request* req) {
    const char *slash = strrchr(req->path, '/');
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


// add node: 0 for success, -1 for pq full error
int add_work(Safequeue *pq, struct http_request *req) {
    pthread_mutex_lock(&pq->lock);

    if (pq->size == pq->capacity) {
        pthread_mutex_unlock(&pq->lock);
        return -1;
    }

    // initialize node
    Node temp;
    temp.data = req;
    temp.priority =  get_priority(req);

    pq->reqs[pq->size] = temp;
    int ind = pq->size;
    pq->size++;

    // no head
    if (ind == 0) {
        pq->reqs[ind] = temp;

        pthread_mutex_unlock(&pq->lock);
        return 0;
    }

    // insert according to priority
    while (ind > 0) {
        int parentIndex = (ind - 1) / 2;
        if (pq->reqs[ind].priority > pq->reqs[parentIndex].priority) {
            // swap with parent
            Node temp = pq->reqs[ind];
            pq->reqs[ind] = pq->reqs[parentIndex];
            pq->reqs[parentIndex] = temp;

            ind = parentIndex; // Move up the heap
        } else {
            break; // The heap property is satisfied
        }
    }

    pthread_mutex_unlock(&pq->lock);
    return 0;
}

// reorder
void reorder(Safequeue *pq, int ind) {

    int largestInd = ind;
    int leftInd = 2 * ind + 1;  
    int rightInd = 2 * ind + 2; 

    if (leftInd < pq->size && pq->reqs[leftInd].priority > pq->reqs[largestInd].priority) {
        largestInd = leftInd;
    }

    if (rightInd < pq->size && pq->reqs[rightInd].priority > pq->reqs[largestInd].priority) {
        largestInd = rightInd;
    }

    // swap and continue if root is not largest
    if (largestInd != ind) {
        Node temp = pq->reqs[ind];
        pq->reqs[ind] = pq->reqs[largestInd];
        pq->reqs[largestInd] = temp;

        reorder(pq, largestInd);
    }
}

// poll
Node *get_work(Safequeue *pq) {
    pthread_mutex_lock(&pq->lock);

    while (pq->size == 0) {
       pthread_cond_wait(&pq->cond, &pq->lock);
    }

    // remove highest priority job
    Node *req = &pq->reqs[0];

    pq->reqs[0] = pq->reqs[pq->size - 1];
    pq->size--;

    reorder(pq, 0);

    pthread_mutex_unlock(&pq->lock);
    return req;
}

Node *get_work_nonblocking(Safequeue *pq) {
    pthread_mutex_lock(&pq->lock);
    if (pq->size == 0) {
        pthread_mutex_unlock(&pq->lock);
        return NULL; // no available job
    }

    // remove highest priority job
    Node *req = &pq->reqs[0];

    pq->reqs[0] = pq->reqs[pq->size - 1];
    pq->size--;

    reorder(pq, 0);

    pthread_mutex_unlock(&pq->lock);
    return req;
}