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
#define NONCE_OFFSET 72

/* ===== Shared mining state (protected by mutex) ===== */

static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t current_header[32];
static uint8_t current_target[32];
static char current_job_id[65];
static uint64_t current_timestamp = 0;
static bool has_job = false;

/* ===== Mining control ===== */

static volatile bool mining_running = false;
static pthread_t threads[MAX_THREADS];
static int active_thread_count = 0;

/* ===== Statistics (atomic updates) ===== */

static volatile uint64_t total_hashes = 0;
static volatile uint32_t shares_found = 0;
static volatile uint32_t shares_rejected = 0;
static volatile uint64_t hash_count_for_rate = 0;
static struct timespec rate_start_time;

/* ===== Per-thread hash counters (cache-line aligned to avoid false sharing) ===== */

typedef struct {
    volatile uint64_t hashes;
    char _pad[64 - sizeof(uint64_t)];
} __attribute__((aligned(64))) ThreadStat;

static ThreadStat thread_stats[MAX_THREADS];

/* ===== Hashrate history ring buffer =====
 * Sampled every SAMPLE_INTERVAL_MS by a dedicated reporter thread. Reader walks
 * back from newest sample to compute a rate over any window. One mutex covers
 * both push and query; contention is negligible (2 writes/sec, reads on demand).
 */

#define SAMPLE_INTERVAL_MS 500
#define HISTORY_SLOTS 1800   /* 1800 * 500ms = 15 min of history */

typedef struct {
    uint64_t ts_ms;
    uint64_t total_hashes;
} HashrateSample;

static HashrateSample samples[HISTORY_SLOTS];
static uint64_t samples_written = 0;        /* total samples ever written (monotonic) */
static pthread_mutex_t samples_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t reporter_thread;
static volatile bool reporter_running = false;

/* ===== Extranonce ===== */

static volatile uint64_t extranonce_prefix = 0;   /* Upper bits of nonce (from bridge) */
static volatile int extranonce2_bits = 64;         /* Bits available for miner (default: all 64) */
static volatile uint64_t extranonce2_mask = 0xFFFFFFFFFFFFFFFFULL; /* Mask for miner portion */

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
    const uint32_t JOB_CHECK_INTERVAL = 100; /* Check for new jobs every 100 hashes */

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
            /* Build 80-byte work buffer:
             *   [0..31]  = pre_pow_hash (32 bytes)
             *   [32..39] = timestamp (8 bytes, LE)
             *   [40..71] = zero padding (32 bytes)
             *   [72..79] = nonce (written per-hash below) */
            memset(work_buffer, 0, 80);
            memcpy(work_buffer, current_header, 32);
            memcpy(work_buffer + 32, &current_timestamp, 8);
            memcpy(local_target, current_target, 32);
            strncpy(local_job_id, current_job_id, 64);
            local_job_id[64] = '\0';
            pthread_mutex_unlock(&job_mutex);
        }

        /* Write nonce at offset 72 (little-endian) */
        memcpy(work_buffer + NONCE_OFFSET, &nonce, 8);

        /* Compute kHeavyHash */
        kheavyhash_compute(&state, work_buffer, hash_output);

        /* Check difficulty */
        if (check_difficulty(hash_output, local_target)) {
            __sync_fetch_and_add(&shares_found, 1);

            /* Debug: log first share's work buffer for verification */
            static volatile int debug_count = 0;
            if (__sync_fetch_and_add(&debug_count, 1) < 3) {
                uint64_t ts_val;
                memcpy(&ts_val, work_buffer + 32, 8);
                LOGI("SHARE_DEBUG pre_pow[0:8]: %02x%02x%02x%02x%02x%02x%02x%02x",
                     work_buffer[0], work_buffer[1], work_buffer[2], work_buffer[3],
                     work_buffer[4], work_buffer[5], work_buffer[6], work_buffer[7]);
                LOGI("SHARE_DEBUG timestamp: %llu nonce: 0x%016llx",
                     (unsigned long long)ts_val, (unsigned long long)nonce);
                LOGI("SHARE_DEBUG hash[0:8]: %02x%02x%02x%02x%02x%02x%02x%02x",
                     hash_output[0], hash_output[1], hash_output[2], hash_output[3],
                     hash_output[4], hash_output[5], hash_output[6], hash_output[7]);
                LOGI("SHARE_DEBUG target[0:8]: %02x%02x%02x%02x%02x%02x%02x%02x",
                     local_target[0], local_target[1], local_target[2], local_target[3],
                     local_target[4], local_target[5], local_target[6], local_target[7]);
                LOGI("SHARE_DEBUG bytes[32-39](ts): %02x%02x%02x%02x%02x%02x%02x%02x",
                     work_buffer[32], work_buffer[33], work_buffer[34], work_buffer[35],
                     work_buffer[36], work_buffer[37], work_buffer[38], work_buffer[39]);
                LOGI("SHARE_DEBUG bytes[72-79](nonce): %02x%02x%02x%02x%02x%02x%02x%02x",
                     work_buffer[72], work_buffer[73], work_buffer[74], work_buffer[75],
                     work_buffer[76], work_buffer[77], work_buffer[78], work_buffer[79]);
            }

            if (on_share_found) {
                on_share_found(local_job_id, nonce);
            }
        }

        __sync_fetch_and_add(&total_hashes, 1);
        __sync_fetch_and_add(&hash_count_for_rate, 1);
        /* Per-thread counter has a single writer (this thread), but use atomic
         * RELAXED so the reporter thread sees up-to-date values without tearing. */
        __atomic_fetch_add(&thread_stats[thread_id].hashes, 1, __ATOMIC_RELAXED);

        /* Increment only the extranonce2 portion, preserving the extranonce prefix */
        uint64_t en2_part = (nonce & extranonce2_mask) + active_thread_count;
        nonce = extranonce_prefix | (en2_part & extranonce2_mask);
        batch_count++;
    }

    LOGI("Mining thread %d stopped", thread_id);
    return NULL;
}

/* ===== Self-test ===== */

static bool run_selftest(void) {
    /* Test vector: pre_pow=[0x2A]*32, ts=5435345234, nonce=432432432
     * Expected kHeavyHash first 8 bytes: 5a5bcd6e352eb8c8
     * (from verified Python + C cross-reference) */
    KHeavyHashState test_state;
    kheavyhash_init(&test_state);

    uint8_t test_input[80];
    uint8_t test_output[32];
    memset(test_input, 0, 80);

    /* pre_pow_hash = [0x2A]*32 */
    memset(test_input, 0x2A, 32);

    /* timestamp = 5435345234 (LE at offset 32) */
    uint64_t test_ts = 5435345234ULL;
    memcpy(test_input + 32, &test_ts, 8);

    /* zeros at [40..71] — already zero */

    /* nonce = 432432432 (LE at offset 72) */
    uint64_t test_nonce = 432432432ULL;
    memcpy(test_input + 72, &test_nonce, 8);

    kheavyhash_compute(&test_state, test_input, test_output);

    /* Log full hash */
    LOGI("SELFTEST input[0:8]: %02x%02x%02x%02x%02x%02x%02x%02x",
         test_input[0], test_input[1], test_input[2], test_input[3],
         test_input[4], test_input[5], test_input[6], test_input[7]);
    LOGI("SELFTEST ts_bytes[32:40]: %02x%02x%02x%02x%02x%02x%02x%02x",
         test_input[32], test_input[33], test_input[34], test_input[35],
         test_input[36], test_input[37], test_input[38], test_input[39]);
    LOGI("SELFTEST nonce_bytes[72:80]: %02x%02x%02x%02x%02x%02x%02x%02x",
         test_input[72], test_input[73], test_input[74], test_input[75],
         test_input[76], test_input[77], test_input[78], test_input[79]);
    LOGI("SELFTEST hash[0:16]: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
         test_output[0], test_output[1], test_output[2], test_output[3],
         test_output[4], test_output[5], test_output[6], test_output[7],
         test_output[8], test_output[9], test_output[10], test_output[11],
         test_output[12], test_output[13], test_output[14], test_output[15]);
    LOGI("SELFTEST hash[16:32]: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
         test_output[16], test_output[17], test_output[18], test_output[19],
         test_output[20], test_output[21], test_output[22], test_output[23],
         test_output[24], test_output[25], test_output[26], test_output[27],
         test_output[28], test_output[29], test_output[30], test_output[31]);

    /* Check expected: first 8 bytes should be 5a5bcd6e352eb8c8 */
    const uint8_t expected[8] = {0x5a, 0x5b, 0xcd, 0x6e, 0x35, 0x2e, 0xb8, 0xc8};
    bool pass = (memcmp(test_output, expected, 8) == 0);
    LOGI("SELFTEST %s — expected 5a5bcd6e352eb8c8, got %02x%02x%02x%02x%02x%02x%02x%02x",
         pass ? "PASSED" : "FAILED",
         test_output[0], test_output[1], test_output[2], test_output[3],
         test_output[4], test_output[5], test_output[6], test_output[7]);

    return pass;
}

/* ===== Hashrate sampler ===== */

static uint64_t now_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static uint64_t sum_thread_hashes(void) {
    uint64_t sum = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        sum += __atomic_load_n(&thread_stats[i].hashes, __ATOMIC_RELAXED);
    }
    return sum;
}

static void *reporter_loop(void *arg) {
    (void)arg;
    LOGI("Hashrate reporter thread started");
    while (reporter_running) {
        uint64_t ts = now_monotonic_ms();
        uint64_t total = sum_thread_hashes();

        pthread_mutex_lock(&samples_mutex);
        HashrateSample *slot = &samples[samples_written % HISTORY_SLOTS];
        slot->ts_ms = ts;
        slot->total_hashes = total;
        samples_written++;
        pthread_mutex_unlock(&samples_mutex);

        struct timespec sleep_ts = {0, SAMPLE_INTERVAL_MS * 1000000L};
        nanosleep(&sleep_ts, NULL);
    }
    LOGI("Hashrate reporter thread stopped");
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

    /* Reset per-thread counters and hashrate history */
    for (int i = 0; i < MAX_THREADS; i++) {
        __atomic_store_n(&thread_stats[i].hashes, 0, __ATOMIC_RELAXED);
    }
    pthread_mutex_lock(&samples_mutex);
    samples_written = 0;
    memset(samples, 0, sizeof(samples));
    pthread_mutex_unlock(&samples_mutex);

    clock_gettime(CLOCK_MONOTONIC, &rate_start_time);

    LOGI("Starting mining with %d threads", num_threads);

    /* Launch reporter thread for windowed hashrate sampling */
    reporter_running = true;
    if (pthread_create(&reporter_thread, NULL, reporter_loop, NULL) != 0) {
        LOGE("Failed to start hashrate reporter thread");
        reporter_running = false;
    }

    /* Run self-test to verify compiled kHeavyHash is correct */
    run_selftest();

    /* Seed nonce from random source */
    srand((unsigned int)time(NULL));

    for (int i = 0; i < num_threads; i++) {
        WorkerArgs *args = malloc(sizeof(WorkerArgs));
        args->thread_id = i;

        /* Generate random nonce, masked to extranonce2 portion, OR'd with extranonce prefix */
        uint64_t random_part = ((uint64_t)rand() << 32) | rand();
        random_part = (random_part & extranonce2_mask) + i;
        args->nonce_start = extranonce_prefix | random_part;

        LOGI("Thread %d nonce_start: 0x%016llx (prefix=0x%016llx, mask=0x%016llx)",
             i, (unsigned long long)args->nonce_start,
             (unsigned long long)extranonce_prefix,
             (unsigned long long)extranonce2_mask);

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

    /* Stop reporter thread after workers so final sample captures their last hashes */
    if (reporter_running) {
        reporter_running = false;
        pthread_join(reporter_thread, NULL);
    }

    active_thread_count = 0;
    LOGI("Mining stopped. Total hashes: %llu", (unsigned long long)total_hashes);
}

bool mining_is_running(void) {
    return mining_running;
}

void mining_set_job(const uint8_t *header_hash, const char *job_id, const uint8_t *target, uint64_t timestamp) {
    pthread_mutex_lock(&job_mutex);
    memcpy(current_header, header_hash, 32);
    memcpy(current_target, target, 32);
    strncpy(current_job_id, job_id, 64);
    current_job_id[64] = '\0';
    current_timestamp = timestamp;
    has_job = true;
    pthread_mutex_unlock(&job_mutex);

    LOGI("JOB SET: id=%s timestamp=%llu (0x%llx)", job_id,
         (unsigned long long)timestamp, (unsigned long long)timestamp);
    LOGI("  target: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
         target[0], target[1], target[2], target[3],
         target[4], target[5], target[6], target[7],
         target[8], target[9], target[10], target[11]);
}

double mining_get_hashrate(void) {
    if (!mining_running) return 0.0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (now.tv_sec - rate_start_time.tv_sec) +
                     (now.tv_nsec - rate_start_time.tv_nsec) / 1e9;

    if (elapsed <= 0.0) return 0.0;

    uint64_t count = hash_count_for_rate;
    return (double)count / elapsed;
}

double mining_get_hashrate_window(double seconds) {
    if (seconds <= 0.0) return 0.0;

    pthread_mutex_lock(&samples_mutex);
    uint64_t written = samples_written;
    if (written < 2) {
        pthread_mutex_unlock(&samples_mutex);
        return 0.0;
    }

    uint64_t newest_slot = (written - 1) % HISTORY_SLOTS;
    uint64_t newest_ts = samples[newest_slot].ts_ms;
    uint64_t newest_h = samples[newest_slot].total_hashes;

    uint64_t window_ms = (uint64_t)(seconds * 1000.0);
    uint64_t target_ts = (newest_ts > window_ms) ? (newest_ts - window_ms) : 0;

    /* Walk back to find the oldest sample whose timestamp is within the window,
     * bounded by history capacity. Use the sample just past target_ts so the
     * window covers at least `seconds`. */
    uint64_t max_back = (written < HISTORY_SLOTS) ? written : HISTORY_SLOTS;
    uint64_t oldest_ts = newest_ts;
    uint64_t oldest_h = newest_h;

    for (uint64_t back = 1; back < max_back; back++) {
        uint64_t slot = (written - 1 - back) % HISTORY_SLOTS;
        oldest_ts = samples[slot].ts_ms;
        oldest_h = samples[slot].total_hashes;
        if (oldest_ts <= target_ts) break;
    }
    pthread_mutex_unlock(&samples_mutex);

    if (newest_ts <= oldest_ts) return 0.0;
    double elapsed_s = (double)(newest_ts - oldest_ts) / 1000.0;
    uint64_t delta = newest_h - oldest_h;
    return (double)delta / elapsed_s;
}

uint64_t mining_get_thread_hashes(int thread_idx) {
    if (thread_idx < 0 || thread_idx >= MAX_THREADS) return 0;
    return __atomic_load_n(&thread_stats[thread_idx].hashes, __ATOMIC_RELAXED);
}

int mining_get_active_threads(void) {
    return active_thread_count;
}

uint64_t mining_get_total_hashes(void) {
    return total_hashes;
}

uint32_t mining_get_shares_found(void) {
    return shares_found;
}

uint32_t mining_get_shares_rejected(void) {
    return shares_rejected;
}

void mining_increment_rejected(void) {
    __sync_fetch_and_add(&shares_rejected, 1);
}

void mining_set_share_callback(share_found_callback cb) {
    on_share_found = cb;
}

void mining_set_extranonce(uint64_t prefix, int en2_bits) {
    extranonce_prefix = prefix;
    extranonce2_bits = en2_bits;
    if (en2_bits >= 64) {
        extranonce2_mask = 0xFFFFFFFFFFFFFFFFULL;
    } else {
        extranonce2_mask = (1ULL << en2_bits) - 1;
    }
    LOGI("Extranonce set: prefix=0x%016llx, en2_bits=%d, mask=0x%016llx",
         (unsigned long long)prefix, en2_bits, (unsigned long long)extranonce2_mask);
}
