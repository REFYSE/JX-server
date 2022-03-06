#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

struct task {
    void (*func)(void*);
    void* arg;
    struct task* next;
};

struct queue {
    struct task* head;
    struct task* tail;
};

// Queue functions
struct queue* init_queue();
void enqueue(struct queue* q, void(*func)(void*), void* arg);
struct task* dequeue(struct queue* q);


struct threadpool {
    pthread_cond_t sig;
    pthread_mutex_t lock;
    struct queue* queue;
    pthread_t* threads;
    int n_threads;
    int shutdown;
};

// Threadpool functions
struct threadpool* init_threadpool(int n_threads);
void* threadpool_wait(void* threadpool);
void threadpool_add(struct threadpool* pool, void (*func)(void*), void* arg);
void threadpool_cleanup(struct threadpool* pool);

#endif
