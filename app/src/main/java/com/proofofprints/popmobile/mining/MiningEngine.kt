/**
 * Kotlin wrapper around the native C mining engine.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.mining

import android.util.Log

class MiningEngine {

    companion object {
        private const val TAG = "MiningEngine"
        /** Sentinel string in every log line from this class. Bump whenever we
         *  push new diagnostic code so we can `adb logcat | grep BUILD_MARKER`
         *  and be sure the device has the latest native+kotlin. */
        const val BUILD_MARKER = "BUILD_MARKER_2026-04-22-socfix"

        init {
            System.loadLibrary("popmobile")
            Log.i(TAG, "Native library loaded [$BUILD_MARKER]")
        }
    }

    /** Guard newly-added JNI methods so an out-of-date .so (partial rebuild)
     *  degrades to a zero reading instead of killing whoever called us with
     *  an UnsatisfiedLinkError. We log the first occurrence only per method
     *  so the Kotlin/native mismatch is visible without spamming logcat. */
    private val linkageWarned = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()

    private inline fun safeCallDouble(name: String, call: () -> Double): Double =
        try { call() } catch (e: LinkageError) { warnLinkage(name, e); 0.0 }

    private inline fun safeCallLong(name: String, call: () -> Long): Long =
        try { call() } catch (e: LinkageError) { warnLinkage(name, e); 0L }

    private inline fun safeCallInt(name: String, call: () -> Int): Int =
        try { call() } catch (e: LinkageError) { warnLinkage(name, e); 0 }

    private fun warnLinkage(name: String, e: LinkageError) {
        if (linkageWarned.add(name)) {
            Log.e(TAG, "JNI $name missing — stale libpopmobile.so? Rebuild native. (${e.message})")
        }
    }

    interface ShareCallback {
        fun onShareFound(jobId: String, nonce: Long)
    }

    fun start(numThreads: Int) {
        Log.i(TAG, "Starting mining with $numThreads threads")
        nativeStart(numThreads)
    }

    fun stop() {
        Log.i(TAG, "Stopping mining")
        nativeStop()
    }

    val isRunning: Boolean
        get() = nativeIsRunning()

    fun setJob(headerHash: ByteArray, jobId: String, target: ByteArray, timestamp: Long = 0L) {
        nativeSetJob(headerHash, jobId, target, timestamp)
    }

    /** Session-average hashrate (hashes since start / elapsed seconds).
     *  Smooths out real variation — use [hashrateWindow] for live display. */
    val hashrate: Double
        get() = safeCallDouble("nativeGetHashrate") { nativeGetHashrate() }

    /** Hashrate over the most recent [seconds] of mining (sliding window).
     *  Prefer 10s for live UI, 60s for telemetry, 900s for steady-state. */
    fun hashrateWindow(seconds: Double): Double =
        safeCallDouble("nativeGetHashrateWindow") { nativeGetHashrateWindow(seconds) }

    /** 10-second live hashrate — what the device is hashing at right now. */
    val hashrate10s: Double get() = hashrateWindow(10.0)

    /** 60-second hashrate — smoothed for telemetry. */
    val hashrate60s: Double get() = hashrateWindow(60.0)

    /** 15-minute hashrate — thermal-steady-state. */
    val hashrate15min: Double get() = hashrateWindow(900.0)

    /** Raw per-thread hash counter, for spotting a stuck worker. */
    fun threadHashes(threadIdx: Int): Long =
        safeCallLong("nativeGetThreadHashes") { nativeGetThreadHashes(threadIdx) }

    val activeThreads: Int
        get() = safeCallInt("nativeGetActiveThreads") { nativeGetActiveThreads() }

    val totalHashes: Long
        get() = nativeGetTotalHashes()

    val sharesFound: Int
        get() = nativeGetSharesFound()

    val sharesRejected: Int
        get() = nativeGetSharesRejected()

    fun incrementRejected() = nativeIncrementRejected()

    fun setShareCallback(callback: ShareCallback?) {
        nativeSetShareCallback(callback)
    }

    fun setTargetFromDifficulty(difficulty: Double): ByteArray {
        val target = ByteArray(32)
        nativeSetTargetFromDifficulty(difficulty, target)
        return target
    }

    fun setExtranonce(extranoncePrefix: Long, extranonce2Bits: Int) {
        Log.i(TAG, "Setting extranonce: prefix=0x${String.format("%016x", extranoncePrefix)}, en2bits=$extranonce2Bits")
        nativeSetExtranonce(extranoncePrefix, extranonce2Bits)
    }

    /* Native methods */
    private external fun nativeStart(numThreads: Int)
    private external fun nativeStop()
    private external fun nativeIsRunning(): Boolean
    private external fun nativeSetJob(headerHash: ByteArray, jobId: String, target: ByteArray, timestamp: Long)
    private external fun nativeGetHashrate(): Double
    private external fun nativeGetHashrateWindow(seconds: Double): Double
    private external fun nativeGetThreadHashes(threadIdx: Int): Long
    private external fun nativeGetActiveThreads(): Int
    private external fun nativeGetTotalHashes(): Long
    private external fun nativeGetSharesFound(): Int
    private external fun nativeGetSharesRejected(): Int
    private external fun nativeIncrementRejected()
    private external fun nativeSetShareCallback(callback: ShareCallback?)
    private external fun nativeSetTargetFromDifficulty(difficulty: Double, targetOut: ByteArray)
    private external fun nativeSetExtranonce(extranoncePrefix: Long, extranonce2Bits: Int)
}
