/**
 * Mining engine — manages multi-threaded hash computation.
 *
 * Copyright (c) 2026 Proof of Prints
 */

#ifndef MINING_ENGINE_H
#define MINING_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start mining with the given number of threads.
 * @param num_threads  Number of CPU threads to use
 */
void mining_start(int num_threads);

/**
 * Stop all mining threads.
 */
void mining_stop(void);

/**
 * Check if mining is currently active.
 */
bool mining_is_running(void);

/**
 * Update the current mining job.
 *
 * @param header_hash  32-byte header hash from pool
 * @param job_id       Null-terminated job ID string
 * @param target       32-byte target for difficulty check
 */
void mining_set_job(const uint8_t *header_hash, const char *job_id, const uint8_t *target, uint64_t timestamp);

/**
 * Get current hashrate in hashes/second.
 */
double mining_get_hashrate(void);

/**
 * Get total hashes computed since start.
 */
uint64_t mining_get_total_hashes(void);

/**
 * Get number of shares found.
 */
uint32_t mining_get_shares_found(void);

/**
 * Callback type for when a share is found.
 * Called from mining thread — must be thread-safe.
 */
typedef void (*share_found_callback)(const char *job_id, uint64_t nonce);

/**
 * Set the callback for share submission.
 */
uint32_t mining_get_shares_rejected(void);
void mining_increment_rejected(void);
void mining_set_share_callback(share_found_callback cb);

/**
 * Set extranonce from bridge.
 * @param prefix     Extranonce value shifted to upper nonce bits
 * @param en2_bits   Number of bits available for miner (lower bits)
 */
void mining_set_extranonce(uint64_t prefix, int en2_bits);

#ifdef __cplusplus
}
#endif

#endif /* MINING_ENGINE_H */
