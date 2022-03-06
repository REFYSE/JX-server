#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "threadpool.h"

// Queue functions
struct queue* init_queue() {
    struct queue* q = malloc(sizeof(struct queue));
    if(q == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    q->head = NULL;
    q->tail = NULL;
    return q;
}

void enqueue(struct queue* q, void(*func)(void*), void* arg) {
    if(q == NULL || func == NULL || arg == NULL) {
        return;
    }
    struct task* task = malloc(sizeof(struct task));
    if(task == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    task->func = func;
    task->arg = arg;
    task->next = NULL;

    // Case nothing in queue
    if(q->head == NULL) {
        q->head = task;
        q->tail = task;
    } else {
        // Case something already in queue
        q->tail->next = task;
        q->tail = task;
    }
}

struct task* dequeue(struct queue* q) {
    if(q == NULL || q->head == NULL) {
        return NULL;
    }
    struct task* task;
    // Case only 1 element
    if(q->head == q->tail) {
        task = q->head;
        q->head = NULL;
        q->tail = NULL;
        return task;
    } else {
        // Case multiple elements
        task = q->head;
        q->head = q->head->next;
        return task;
    }
}

/* USYD CODE CITATION ACKNOWLEDGEMENT
 * The design for this threadpool was inspired by the example at the below URL.
 * However the code itself is original and different to the example given
 *
 * Original URL
 * https://programmer.group/c-simple-thread-pool-based-on-pthread-implementation.html
 * Last access June, 2020
 */

// Threadpool functions
struct threadpool* init_threadpool(int n_threads) {
    if(n_threads == 0) {
        return NULL;
    }
    // Create the pool
    struct threadpool* pool = malloc(sizeof(struct threadpool));
    if(pool == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    // Create the array of threads
    pthread_t* threads = malloc(sizeof(pthread_t) * n_threads);
    if(threads == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    // Initialize mutex and condition
    int ret = pthread_mutex_init(&(pool->lock), NULL);
    if(ret != 0) {
        perror("mutex_init() failed");
        exit(1);
    }
    ret = pthread_cond_init(&(pool->sig), NULL);
    if(ret != 0) {
        perror("cond_init() failed");
        exit(1);
    }

    // Initialise the rest of the pool
    pool->queue = init_queue();
    pool->n_threads = n_threads;
    pool->shutdown = 0;
    pool->threads = threads;

    // Start all the worker threads
    for(int i = 0; i < n_threads; i++) {
        // Start all worker threads running the thread function
        ret = pthread_create(&(pool->threads[i]), NULL,
                                threadpool_wait, (void*)pool);
        if(ret != 0) {
            perror("pethread_create() failed");
            exit(1);
        }
    }

    return pool;
}

void* threadpool_wait(void* threadpool) {
    if(threadpool == NULL) {
        return NULL;
    }
    struct threadpool* pool = (struct threadpool*)threadpool;

    // Threads keep waiting for task until shutdown
    struct task* task;
    while(1) {
        // Acquire the lock
        pthread_mutex_lock(&(pool->lock));
        // Wait for a task to become available or the pool to shutdown
        while(pool->queue->head == NULL && !(pool->shutdown)) {
            pthread_cond_wait(&(pool->sig), &(pool->lock));
        }

        // Thread woken
        // Check if shutdown in progress
        if(pool->shutdown) {
            break;
        }

        // Otherwise get the task from the queue
        task = dequeue(pool->queue);

        // Release the lock
        pthread_mutex_unlock(&(pool->lock));

        // Execute the task
        task->func(task->arg);

        // Free the task structure as it will not longer be used
        free(task);
    }
    // Pool is shutting down
    pthread_mutex_unlock(&(pool->lock));
    return NULL;
}

void threadpool_add(struct threadpool* pool, void (*func)(void*), void* arg) {
    if(pool == NULL || func == NULL || arg == NULL) {
        return;
    }
    // Acquire lock before modifying task queue
    pthread_mutex_lock(&(pool->lock));

    // Add task to queue
    enqueue(pool->queue, func, arg);

    // Notify worker threads that there is a new task
    int ret = pthread_cond_signal(&(pool->sig));
    if(ret != 0) {
        perror("cond_signal() failed");
        exit(1);
    }

    // Release lock
    pthread_mutex_unlock(&(pool->lock));
}

void threadpool_cleanup(struct threadpool* pool) {
    if(pool == NULL) {
        return;
    }

    // Acquire lock first before destroying anything
    pthread_mutex_lock(&(pool->lock));

    // Notify all threads of shutdown
    pool->shutdown = 1;
    pthread_cond_broadcast(&(pool->sig));
    pthread_mutex_unlock(&(pool->lock));

    // Join all worker threads and wait for them to finish execution
    for(int i = 0; i < pool->n_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    // Free memory allocated for threadpool
    free(pool->threads);

    // Free tasks that have not yet been executed
    struct task* cur = dequeue(pool->queue);
    while(cur != NULL) {
        free(cur->arg);
        free(cur);
        cur = dequeue(pool->queue);
    }
    // Free the queue itself
    free(pool->queue);
    // Destroy lock and condition
    pthread_mutex_destroy(&(pool->lock));
    pthread_cond_destroy(&(pool->sig));

    free(pool);
}
