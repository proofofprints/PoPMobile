/**
 * Singleton log manager for in-app mining event log.
 *
 * Stores up to MAX_ENTRIES log entries in a FIFO ring buffer.
 * Entries older than MAX_AGE_MS are pruned on each add.
 * UI observes [entries] as a Compose snapshot state list.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer

import androidx.compose.runtime.mutableStateListOf

enum class LogLevel { INFO, WARN, ERROR }

data class LogEntry(
    val timestamp: Long = System.currentTimeMillis(),
    val level: LogLevel,
    val message: String
)

object LogManager {
    private const val MAX_ENTRIES = 200
    private const val MAX_AGE_MS = 4 * 60 * 60 * 1000L // 4 hours

    val entries = mutableStateListOf<LogEntry>()

    fun info(message: String) = add(LogLevel.INFO, message)
    fun warn(message: String) = add(LogLevel.WARN, message)
    fun error(message: String) = add(LogLevel.ERROR, message)

    fun clear() { entries.clear() }

    private fun add(level: LogLevel, message: String) {
        synchronized(entries) {
            // Prune old entries (older than 4 hours)
            val cutoff = System.currentTimeMillis() - MAX_AGE_MS
            entries.removeAll { it.timestamp < cutoff }

            if (entries.size >= MAX_ENTRIES) {
                entries.removeAt(0)
            }
            entries.add(LogEntry(level = level, message = message))
        }
    }
}
