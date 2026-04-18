/**
 * Stratum protocol client for Kaspa pool communication.
 *
 * Ported from KASDeck Arduino stratum logic to Kotlin.
 * Handles mining.subscribe, mining.authorize, mining.notify, mining.submit.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.stratum

import android.util.Log
import com.proofofprints.popmobile.LogManager
import com.google.gson.Gson
import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import kotlinx.coroutines.*
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.Socket

class StratumClient(
    private val listener: StratumListener
) {
    companion object {
        private const val TAG = "StratumClient"
    }

    interface StratumListener {
        fun onConnected()
        fun onDisconnected(reason: String)
        fun onNewJob(jobId: String, headerHash: ByteArray, timestamp: Long)
        fun onDifficultyChanged(difficulty: Double)
        fun onShareAccepted()
        fun onShareRejected(reason: String)
        fun onExtranonceSet(extranonce: String, extranonceShifted: Long, extranonce2Bits: Int)
    }

    private var socket: Socket? = null
    private var writer: PrintWriter? = null
    private var reader: BufferedReader? = null
    private var readJob: Job? = null
    private val gson = Gson()
    private var messageId = 1
    private var walletWorker: String = ""
    private val pendingShareIds = java.util.Collections.synchronizedSet(mutableSetOf<Int>())
    private var extranonce: String = ""  // Bridge-assigned extranonce prefix (hex chars)

    val isConnected: Boolean
        get() = socket?.isConnected == true && socket?.isClosed == false

    suspend fun connect(host: String, port: Int, wallet: String, worker: String) {
        withContext(Dispatchers.IO) {
            try {
                Log.i(TAG, "Connecting to $host:$port...")
                // Use explicit connect with 10s timeout so a refused/unreachable
                // pool surfaces a quick error instead of blocking for minutes.
                // Assigning the socket before connect() lets disconnect()
                // interrupt an in-progress connection attempt.
                val s = Socket()
                socket = s
                s.connect(java.net.InetSocketAddress(host, port), 10_000)
                s.keepAlive = true
                s.soTimeout = 300_000 // 5 minute read timeout
                writer = PrintWriter(s.getOutputStream(), true)
                reader = BufferedReader(InputStreamReader(s.getInputStream()))

                listener.onConnected()

                // Subscribe
                sendMessage("mining.subscribe", listOf(worker))
                delay(500)

                // Authorize
                walletWorker = "$wallet.$worker"
                sendMessage("mining.authorize", listOf(walletWorker))

                // Start reading messages
                startReading()

            } catch (e: Exception) {
                Log.e(TAG, "Connection failed: ${e.message}")
                listener.onDisconnected(e.message ?: "Unknown error")
            }
        }
    }

    fun disconnect() {
        readJob?.cancel()
        try {
            writer?.close()
            reader?.close()
            socket?.close()
        } catch (e: Exception) {
            Log.e(TAG, "Error closing connection: ${e.message}")
        }
        socket = null
        writer = null
        reader = null
    }

    fun submitShare(jobId: String, nonce: Long) {
        val fullNonceHex = String.format("%016x", nonce)

        // If bridge assigned an extranonce, send only the extranonce2 portion.
        // Bridge prepends extranonce1 to reconstruct the full nonce.
        val nonceHex = if (extranonce.isNotEmpty()) {
            val extranonce2Len = 16 - extranonce.length
            "0x" + fullNonceHex.takeLast(extranonce2Len)
        } else {
            "0x$fullNonceHex"
        }

        val submitId = messageId++
        pendingShareIds.add(submitId)
        // 3 params: wallet.worker, job_id, nonce_hex (with 0x prefix)
        sendRaw("""{"id": $submitId, "method": "mining.submit", "params": ["$walletWorker", "$jobId", "$nonceHex"]}""")
        Log.i(TAG, "Submitted share (id=$submitId) - Job: $jobId, Nonce: $nonceHex (full: 0x$fullNonceHex)")
        LogManager.info("Share submitted - Job: $jobId, Nonce: $nonceHex")
    }

    private fun sendMessage(method: String, params: List<String>) {
        val msg = JsonObject().apply {
            addProperty("id", messageId++)
            addProperty("method", method)
            add("params", JsonArray().apply {
                params.forEach { add(it) }
            })
        }
        sendRaw(gson.toJson(msg))
    }

    private fun sendRaw(message: String) {
        try {
            writer?.println(message)
            Log.i(TAG, ">>> $message")
        } catch (e: Exception) {
            Log.e(TAG, "Send failed: ${e.message}")
            listener.onDisconnected("Send failed: ${e.message}")
        }
    }

    private fun startReading() {
        readJob = CoroutineScope(Dispatchers.IO).launch {
            try {
                while (isActive && isConnected) {
                    val line = reader?.readLine() ?: break
                    if (line.isBlank()) continue
                    Log.i(TAG, "<<< $line")
                    handleMessage(line)
                }
            } catch (e: Exception) {
                if (isActive) {
                    Log.e(TAG, "Read error: ${e.message}")
                    listener.onDisconnected(e.message ?: "Read error")
                }
            }
        }
    }

    private fun handleMessage(json: String) {
        try {
            val doc = JsonParser.parseString(json).asJsonObject

            // Handle method calls from server (mining.notify, mining.set_difficulty)
            if (doc.has("method")) {
                val method = doc.get("method").asString

                when (method) {
                    "mining.notify" -> handleNotify(doc)
                    "mining.set_difficulty" -> handleSetDifficulty(doc)
                    "mining.set_extranonce" -> handleSetExtranonce(doc)
                    else -> Log.w(TAG, "Unknown/unhandled method: $method — raw: $json")
                }
                return
            }

            // Handle responses to our requests
            if (doc.has("id")) {
                val id = doc.get("id").asInt

                if (pendingShareIds.remove(id)) {
                    // Share submission response
                    if (doc.has("error") && !doc.get("error").isJsonNull) {
                        val error = doc.get("error").toString()
                        Log.w(TAG, "Share rejected (id=$id): $error")
                        listener.onShareRejected(error)
                    } else if (doc.has("result")) {
                        val result = doc.get("result")
                        if (result.isJsonPrimitive && result.asBoolean) {
                            Log.i(TAG, "Share accepted! (id=$id)")
                            listener.onShareAccepted()
                        }
                    }
                } else {
                    // Response to subscribe/authorize — just log it
                    Log.d(TAG, "Response for id=$id: ${doc}")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error parsing message: ${e.message}")
        }
    }

    private fun handleNotify(doc: JsonObject) {
        try {
            val params = doc.getAsJsonArray("params")
            val jobId = params[0].asString

            // Parse header hash — can be array of uint64 or hex string
            val headerHash = ByteArray(32)

            val headerElement = params[1]
            if (headerElement.isJsonArray) {
                // Array of uint64 numbers (4 x uint64 = 32 bytes)
                // CRITICAL: Values can exceed Long.MAX_VALUE (unsigned uint64).
                // Gson's asLong falls back to (long)Double.parseDouble() for large values,
                // which corrupts the result. Must parse as string and use parseUnsignedLong.
                val headerArray = headerElement.asJsonArray
                for (i in 0 until minOf(4, headerArray.size())) {
                    // CRITICAL: Gson may return scientific notation for large numbers.
                    // Use BigInteger to handle any format correctly (decimal, scientific).
                    val bigVal = java.math.BigInteger(headerArray[i].asBigDecimal.toPlainString().split(".")[0])
                    val value = bigVal.toLong() // Returns low 64 bits — correct for unsigned u64
                    for (j in 0 until 8) {
                        headerHash[i * 8 + j] = ((value shr (j * 8)) and 0xFF).toByte()
                    }
                }
            } else {
                // Hex string format
                val hexStr = headerElement.asString
                for (i in 0 until minOf(32, hexStr.length / 2)) {
                    headerHash[i] = hexStr.substring(i * 2, i * 2 + 2).toInt(16).toByte()
                }
            }

            val timestamp = if (params.size() > 2) params[2].asLong else 0L

            // Log full parsed header hash for debugging
            val hashHex = headerHash.joinToString("") { "%02x".format(it) }
            Log.i(TAG, "New job: $jobId, timestamp=$timestamp, headerHash=$hashHex")
            LogManager.info("Job: $jobId ts=$timestamp hash=${hashHex.take(16)}...")
            listener.onNewJob(jobId, headerHash, timestamp)

        } catch (e: Exception) {
            Log.e(TAG, "Error parsing mining.notify: ${e.message}")
        }
    }

    private fun handleSetDifficulty(doc: JsonObject) {
        try {
            val params = doc.getAsJsonArray("params")
            val difficulty = params[0].asDouble
            Log.i(TAG, "New difficulty: $difficulty")
            listener.onDifficultyChanged(difficulty)
        } catch (e: Exception) {
            Log.e(TAG, "Error parsing mining.set_difficulty: ${e.message}")
        }
    }

    private fun handleSetExtranonce(doc: JsonObject) {
        try {
            val params = doc.getAsJsonArray("params")
            extranonce = params[0].asString
            Log.i(TAG, "Extranonce set: '$extranonce' (${extranonce.length} hex chars = ${extranonce.length / 2} bytes)")
            LogManager.info("Extranonce: $extranonce")

            // Compute the extranonce value as upper nonce bytes for mining
            // e.g., extranonce "001e" → upper 2 bytes of every nonce must be 0x001e
            val extranonceValue = java.lang.Long.parseUnsignedLong(extranonce, 16)
            val extranonce2Bits = (16 - extranonce.length) * 4  // bits for miner portion
            val extranonceShifted = extranonceValue shl extranonce2Bits

            Log.i(TAG, "Extranonce nonce prefix: 0x${String.format("%016x", extranonceShifted)}, extranonce2 bits: $extranonce2Bits")
            listener.onExtranonceSet(extranonce, extranonceShifted, extranonce2Bits)
        } catch (e: Exception) {
            Log.e(TAG, "Error parsing mining.set_extranonce: ${e.message}")
        }
    }
}
