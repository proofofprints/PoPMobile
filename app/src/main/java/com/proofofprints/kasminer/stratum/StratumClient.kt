/**
 * Stratum protocol client for Kaspa pool communication.
 *
 * Ported from KASDeck Arduino stratum logic to Kotlin.
 * Handles mining.subscribe, mining.authorize, mining.notify, mining.submit.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.stratum

import android.util.Log
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
    }

    private var socket: Socket? = null
    private var writer: PrintWriter? = null
    private var reader: BufferedReader? = null
    private var readJob: Job? = null
    private val gson = Gson()
    private var messageId = 1
    private var workerIdentity = ""  // "wallet.worker" for share submissions

    val isConnected: Boolean
        get() = socket?.isConnected == true && socket?.isClosed == false

    suspend fun connect(host: String, port: Int, wallet: String, worker: String) {
        withContext(Dispatchers.IO) {
            try {
                Log.i(TAG, "Connecting to $host:$port...")
                socket = Socket(host, port)
                writer = PrintWriter(socket!!.getOutputStream(), true)
                reader = BufferedReader(InputStreamReader(socket!!.getInputStream()))

                listener.onConnected()
                workerIdentity = "$wallet.$worker"

                // Subscribe
                sendMessage("mining.subscribe", listOf(worker))
                delay(500)

                // Authorize
                sendMessage("mining.authorize", listOf(workerIdentity))

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
        val nonceHex = String.format("%016x", nonce)
        // Bridge expects 3 params: [worker, jobId, nonce] — matches KASDeck format
        sendRaw("""{"id": ${messageId++}, "method": "mining.submit", "params": ["$workerIdentity", "$jobId", "$nonceHex"]}""")
        Log.i(TAG, "Submitted share - Worker: $workerIdentity, Job: $jobId, Nonce: $nonceHex")
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
            Log.d(TAG, ">>> $message")
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
                    Log.d(TAG, "<<< $line")
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
                    else -> Log.d(TAG, "Unknown method: $method")
                }
                return
            }

            // Handle responses to our requests
            if (doc.has("id") && doc.has("result")) {
                val id = doc.get("id").asInt
                val result = doc.get("result")

                if (id == 4 || doc.has("error")) {
                    // Share submission response
                    if (doc.has("error") && !doc.get("error").isJsonNull) {
                        val error = doc.get("error").toString()
                        Log.w(TAG, "Share rejected: $error")
                        listener.onShareRejected(error)
                    } else if (result.isJsonPrimitive && result.asBoolean) {
                        Log.i(TAG, "Share accepted!")
                        listener.onShareAccepted()
                    }
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
                // Array of uint64 numbers (4 x uint64 = 32 bytes) — matches KASDeck format
                val headerArray = headerElement.asJsonArray
                for (i in 0 until minOf(4, headerArray.size())) {
                    val value = headerArray[i].asLong
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

            Log.i(TAG, "New job: $jobId")
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
}
