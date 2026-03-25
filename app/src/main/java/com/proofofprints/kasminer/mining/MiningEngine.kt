/**
 * Kotlin wrapper around the native C mining engine.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.mining

import android.util.Log

class MiningEngine {

    companion object {
        private const val TAG = "MiningEngine"

        init {
            System.loadLibrary("kasminer")
            Log.i(TAG, "Native library loaded")
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

    fun setJob(headerHash: ByteArray, jobId: String, target: ByteArray) {
        nativeSetJob(headerHash, jobId, target)
    }

    val hashrate: Double
        get() = nativeGetHashrate()

    val totalHashes: Long
        get() = nativeGetTotalHashes()

    val sharesFound: Int
        get() = nativeGetSharesFound()

    fun setShareCallback(callback: ShareCallback?) {
        nativeSetShareCallback(callback)
    }

    fun setTargetFromDifficulty(difficulty: Double): ByteArray {
        val target = ByteArray(32)
        nativeSetTargetFromDifficulty(difficulty, target)
        return target
    }

    /* Native methods */
    private external fun nativeStart(numThreads: Int)
    private external fun nativeStop()
    private external fun nativeIsRunning(): Boolean
    private external fun nativeSetJob(headerHash: ByteArray, jobId: String, target: ByteArray)
    private external fun nativeGetHashrate(): Double
    private external fun nativeGetTotalHashes(): Long
    private external fun nativeGetSharesFound(): Int
    private external fun nativeSetShareCallback(callback: ShareCallback?)
    private external fun nativeSetTargetFromDifficulty(difficulty: Double, targetOut: ByteArray)
}
