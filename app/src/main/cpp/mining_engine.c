/**
 * Mining engine — multi-threaded kHeavyHash mining for Android.
 *
 * Each thread gets its own KHeavyHashState and nonce range to avoid
 * contention. Jobs are updated atomically via mutex.
 *
 * Copyright (c) 2026 Proof of Prints
 */

#include "mining_engine.h"
#include "kheavyhash.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

#define TAG "KASMiner"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Maximum mining threads (practical limit for phones) */
#define MAX_THREADS 8

/* ===== Shared mining state (protected by mutex) ===== */

static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t current_header[32];
static uint8_t current_target[32];
static char current_job_id[65];
static bool has_job = false;

/* ===== Mining control ===== */

static volatile bool mining_running = false;
static pthread_t threads[MAX_THREADS];
static int active_thread_count = 0;

/* ===== Statistics (atomic updates) ===== */

static volatile uint64_t total_hashes = 0;
static volatile uint32_t shares_found = 0;
static volatile uint64_t hash_count_for_rate = 0;
static struct timespec rate_start_time;

/* ===== Callback ===== */

static share_found_callback on_share_found = NULL;

/* ===== Worker thread ===== */

typedef struct {
    int thread_id;
    uint64_t nonce_start;
} WorkerArgs;

static void *mining_worker(void *arg) {
    WorkerArgs *args = (WorkerArgs *)arg;
    int thread_id = args->thread_id;
    uint64_t nonce = args->nonce_start;
    free(args);

    KHeavyHashState state;
    kheavyhash_init(&state);

    uint8_t work_buffer[80];
    uint8_t hash_output[32];
    uint8_t local_target[32];
    char local_job_id[65];

    LOGI("Mining thread %d started, nonce offset: %llu", thread_id, (unsigned long long)nonce);

    uint32_t batch_count = 0;
    const uint32_t JOB_CHECK_INTERVAL = 1000;

    while (mining_running) {
        /* Periodically sync job data */
        if (batch_count % JOB_CHECK_INTERVAL == 0) {
            pthread_mutex_lock(&job_mutex);
            if (!has_job) {
                pthread_mutex_unlock(&job_mutex);
                /* No job yet — sleep briefly and retry */
                struct timespec ts = {0, 100000000}; /* 100ms */
                nanosleep(&ts, NULL);
                continue;
            }
            memset(work_buffer, 0, 80);
            memcpy(work_buffer, current_header, 32);
            memcpy(local_target, current_target, 32);
            strncpy(local_job_id, current_job_id, 64);
            local_job_id[64] = '\0';
            pthread_mutex_unlock(&job_mutex);
        }

        /* Write nonce at offset 32 (matching KASDeck layout) */
        *((uint64_t *)(work_buffer + 32)) = nonce;

        /* Compute kHeavyHash */
        kheavyhash_compute(&state, work_buffer, hash_output);

        /* Check difficulty */
        if (check_difficulty(hash_output, local_target)) {
            LOGI("Thread %d found share! Nonce: %llu", thread_id, (unsigned long long)nonce);
            __sync_fetch_and_add(&shares_found, 1);

            if (on_share_found) {
                on_share_found(local_job_id, nonce);
            }
        }

        __sync_fetch_and_add(&total_hashes, 1);
        __sync_fetch_and_add(&hash_count_for_rate, 1);

        /* Each thread increments by MAX_THREADS to avoid overlap */
        nonce += active_thread_count;
        batch_count++;
    }

    LOGI("Mining thread %d stopped", thread_id);
    return NULL;
}

/* ===== Public API ===== */

void mining_start(int num_threads) {
    if (mining_running) return;

    if (num_threads < 1) num_threads = 1;
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    active_thread_count = num_threads;
    total_hashes = 0;
    shares_found = 0;
    hash_count_for_rate = 0;
    mining_running = true;

    clock_gettime(CLOCK_MONOTONIC, &rate_start_time);

    LOGI("Starting mining with %d threads", num_threads);

    /* Seed nonce from random source */
    srand((unsigned int)time(NULL));

    for (int i = 0; i < num_threads; i++) {
        WorkerArgs *args = malloc(sizeof(WorkerArgs));
        args->thread_id = i;
        /* Each thread gets a different starting nonce with wide spacing */
        args->nonce_start = ((uint64_t)rand() << 32) | rand();
        args->nonce_start += i; /* Offset so threads don't overlap */

        pthread_create(&threads[i], NULL, mining_worker, args);
    }
}

void mining_stop(void) {
    if (!mining_running) return;

    LOGI("Stopping mining...");
    mining_running = false;

    for (int i = 0; i < active_thread_count; i++) {
        pthread_join(threads[i], NULL);
    }

    active_thread_count = 0;
    LOGI("Mining stopped. Total hashes: %llu", (unsigned long long)total_hashes);
}

bool mining_is_running(void) {
    return mining_running;
}

void mining_set_job(const uint8_t *header_hash, const char *job_id, const uint8_t *target) {
    pthread_mutex_lock(&job_mutex);
    memcpy(current_header, header_hash, 32);
    memcpy(current_target, target, 32);
    strncpy(current_job_id, job_id, 64);
    current_job_id[64] = '\0';
    has_job = true;
    pthread_mutex_unlock(&job_mutex);

    LOGI("New job: %s, target: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
         job_id,
         target[0], target[1], target[2], target[3],
         target[4], target[5], target[6], target[7],
         target[8], target[9], target[10], target[11]);
}

double mining_get_hashrate(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (now.tv_sec - rate_start_time.tv_sec) +
                     (now.tv_nsec - rate_start_time.tv_nsec) / 1e9;

    if (elapsed <= 0.0) return 0.0;

    uint64_t count = hash_count_for_rate;
    return (double)count / elapsed;
}

uint64_t mining_get_total_hashes(void) {
    return total_hashes;
}

uint32_t mining_get_shares_found(void) {
    return shares_found;
}

void mining_set_share_callback(share_found_callback cb) {
    on_share_found = cb;
}
