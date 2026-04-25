/**
 * PoPManager reporter — pushes miner telemetry to a PoPManager instance AND
 * executes commands returned in the report response.
 *
 * Key design points:
 *   - Report loop runs CONTINUOUSLY while a server URL is configured, regardless
 *     of whether mining is active. Status field reflects mining state
 *     (mining | paused | stopped | error).
 *   - Commands in the /report response are executed via an injected
 *     CommandExecutor, then acknowledged on the NEXT /report cycle.
 *   - Pending acks are persisted to SharedPreferences so they survive an app
 *     restart between executing a command and acking it.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.popmanager

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.util.Log
import com.google.gson.Gson
import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.proofofprints.popmobile.LogManager
import kotlinx.coroutines.*
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.util.UUID

class PoPManagerReporter(
    private val context: Context,
    private val statsProvider: StatsProvider
) {
    companion object {
        private const val TAG = "MobileMiner"
        private const val PREFS = "popmanager"
        private const val KEY_DEVICE_ID = "device_id"
        private const val KEY_API_KEY = "api_key"
        private const val KEY_REPORT_URL = "report_url"
        private const val KEY_DEVICE_NAME = "device_name"
        private const val KEY_SERVER_URL = "server_url"
        private const val KEY_PENDING_ACKS = "pending_acks"  // JSON array
        private const val KEY_APPLIED_IDS = "applied_command_ids"  // JSON array (for dedup)
        private const val DEFAULT_INTERVAL_SECONDS = 30
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val READ_TIMEOUT_MS = 8000
        private const val MAX_APPLIED_HISTORY = 200
    }

    /** Supplies live stats to the reporter on each tick. */
    interface StatsProvider {
        fun snapshot(): TelemetrySnapshot
    }

    /** Executes commands received from PoPManager. Runs on the reporter's IO scope. */
    interface CommandExecutor {
        /** Apply a command. Return null on success, or an error string on failure. */
        fun execute(command: Command): String?
    }

    data class TelemetrySnapshot(
        val coin: String,
        val pool: String,
        val worker: String,
        val hashrate: Double,
        val acceptedShares: Int,
        val rejectedShares: Int,
        val difficulty: Double,
        val runtimeSeconds: Long,
        val cpuTemp: Float,
        val throttleState: String,
        val batteryLevel: Int,
        val batteryCharging: Boolean,
        val threads: Int,
        val status: String,         // mining | paused | stopped | error
        val errorMessage: String? = null
    )

    data class Command(
        val id: String,
        val type: String,           // set_config | set_threads | start | stop | restart
        val params: JsonObject      // arbitrary per-type params
    )

    private data class PendingAck(
        val id: String,
        val status: String,         // applied | failed
        val error: String? = null
    )

    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val gson = Gson()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var reportJob: Job? = null
    private var intervalSeconds: Int = DEFAULT_INTERVAL_SECONDS

    @Volatile private var commandExecutor: CommandExecutor? = null

    /** Observable state for UI. */
    @Volatile var lastStatus: String = "idle"   // idle | registering | reporting | error | disabled | pairing_required
        private set
    @Volatile var lastError: String? = null
        private set
    @Volatile var lastReportAt: Long = 0L
        private set
    /** True when the app has a server URL but no valid apiKey — the user must
     *  enter a pairing code from PoPManager before reporting can start. */
    @Volatile var pairingRequired: Boolean = false
        private set

    val deviceId: String
        get() {
            var id = prefs.getString(KEY_DEVICE_ID, null)
            if (id == null) {
                id = UUID.randomUUID().toString()
                prefs.edit().putString(KEY_DEVICE_ID, id).apply()
            }
            return id
        }

    var serverUrl: String
        get() = prefs.getString(KEY_SERVER_URL, "") ?: ""
        set(value) { prefs.edit().putString(KEY_SERVER_URL, value.trimEnd('/')).apply() }

    var deviceName: String
        get() = prefs.getString(KEY_DEVICE_NAME, defaultDeviceName()) ?: defaultDeviceName()
        set(value) { prefs.edit().putString(KEY_DEVICE_NAME, value).apply() }

    private val apiKey: String?
        get() = prefs.getString(KEY_API_KEY, null)

    private val reportUrl: String?
        get() = prefs.getString(KEY_REPORT_URL, null)

    private fun defaultDeviceName(): String = "${Build.MANUFACTURER} ${Build.MODEL}"

    private fun appVersion(): String = try {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "1.0.0"
    } catch (e: Exception) { "1.0.0" }

    /** Inject the command executor. Usually called once by MiningService. */
    fun setCommandExecutor(executor: CommandExecutor?) {
        commandExecutor = executor
    }

    // ===== Pending acks persistence =====

    private fun loadPendingAcks(): MutableList<PendingAck> {
        val raw = prefs.getString(KEY_PENDING_ACKS, null) ?: return mutableListOf()
        return try {
            val arr = gson.fromJson(raw, JsonArray::class.java)
            arr.mapTo(mutableListOf()) { el ->
                val o = el.asJsonObject
                PendingAck(
                    id = o.get("id").asString,
                    status = o.get("status").asString,
                    error = o.get("error")?.takeIf { !it.isJsonNull }?.asString
                )
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to load pending acks: ${e.message}")
            mutableListOf()
        }
    }

    private fun savePendingAcks(acks: List<PendingAck>) {
        val arr = JsonArray()
        for (ack in acks) {
            val o = JsonObject()
            o.addProperty("id", ack.id)
            o.addProperty("status", ack.status)
            if (ack.error != null) o.addProperty("error", ack.error)
            arr.add(o)
        }
        prefs.edit().putString(KEY_PENDING_ACKS, gson.toJson(arr)).apply()
    }

    private fun queueAck(id: String, status: String, error: String? = null) {
        Log.d(TAG, "Queued ack: $id -> $status${if (error != null) " ($error)" else ""}")
        val acks = loadPendingAcks()
        // Replace any existing ack for same id (idempotent)
        acks.removeAll { it.id == id }
        acks.add(PendingAck(id, status, error))
        savePendingAcks(acks)
    }

    // ===== Applied command dedup =====

    private fun loadAppliedIds(): MutableSet<String> {
        val raw = prefs.getString(KEY_APPLIED_IDS, null) ?: return mutableSetOf()
        return try {
            val arr = gson.fromJson(raw, JsonArray::class.java)
            arr.mapTo(mutableSetOf()) { it.asString }
        } catch (e: Exception) {
            mutableSetOf()
        }
    }

    private fun markApplied(id: String) {
        val ids = loadAppliedIds()
        ids.add(id)
        // Cap the history so prefs don't grow unbounded
        val trimmed = if (ids.size > MAX_APPLIED_HISTORY) {
            ids.toList().takeLast(MAX_APPLIED_HISTORY).toMutableSet()
        } else ids
        val arr = JsonArray()
        trimmed.forEach { arr.add(it) }
        prefs.edit().putString(KEY_APPLIED_IDS, gson.toJson(arr)).apply()
    }

    // ===== Lifecycle =====

    /** Start the periodic report loop. Safe to call repeatedly. Loop runs forever
     *  until [stop] is called, regardless of mining state. */
    fun start() {
        if (serverUrl.isEmpty()) {
            lastStatus = "disabled"
            pairingRequired = false
            Log.d(TAG, "Reporter disabled (no server URL)")
            return
        }
        if (reportJob?.isActive == true) {
            Log.d(TAG, "Reporter already running")
            return
        }

        // If no apiKey is stored, we need the user to pair via the setup
        // screen before we can talk to the server at all.
        if (apiKey == null) {
            pairingRequired = true
            lastStatus = "pairing_required"
            Log.i(TAG, "No apiKey — pairing required")
            LogManager.info("PoPManager: pairing code required")
        }

        Log.i(TAG, "Reporter starting (server=$serverUrl, deviceId=$deviceId)")
        LogManager.info("PoPManager reporter starting")

        reportJob = scope.launch {
            // First attempt: if we have an apiKey, reconnect to refresh
            // reportUrl/interval. If we don't, just enter the idle loop.
            if (apiKey != null && reportUrl == null) {
                reconnectWithApiKey()
            }

            while (isActive) {
                try {
                    if (pairingRequired || apiKey == null) {
                        // Nothing to do until the user pairs — spin at a
                        // gentle interval so we pick up a fresh apiKey
                        // immediately once [pairWithCode] stores one.
                        delay(5_000L)
                        continue
                    }
                    sendReport()
                } catch (e: Exception) {
                    Log.w(TAG, "Report cycle error: ${e.message}")
                }
                delay(intervalSeconds * 1_000L)
            }
        }
    }

    fun stop() {
        Log.i(TAG, "Reporter stopping")
        reportJob?.cancel()
        reportJob = null
        lastStatus = "idle"
    }

    /** Fire a single out-of-band report immediately (e.g. on mining stop so
     *  PoPManager sees the status change without waiting for the next tick). */
    fun reportNow() {
        scope.launch {
            try { sendReport() } catch (e: Exception) {
                Log.w(TAG, "reportNow error: ${e.message}")
            }
        }
    }

    /** Clear stored credentials — forces re-registration on next report. */
    fun clearCredentials() {
        prefs.edit()
            .remove(KEY_API_KEY)
            .remove(KEY_REPORT_URL)
            .apply()
        Log.i(TAG, "Credentials cleared")
    }

    // ===== Registration =====

    /** Build the registration request body (same for pair and reconnect). */
    private fun buildRegisterBody(): String {
        val body = JsonObject().apply {
            addProperty("deviceId", deviceId)
            addProperty("name", deviceName)
            addProperty("deviceModel", "${Build.MANUFACTURER} ${Build.MODEL}")
            addProperty("osVersion", "Android ${Build.VERSION.RELEASE}")
            addProperty("appVersion", appVersion())
        }
        return body.toString()
    }

    /** Persist the apiKey/reportUrl/interval returned by a successful /register. */
    private fun persistRegistration(json: JsonObject) {
        val key = json.get("apiKey").asString
        val url = json.get("reportUrl").asString
        intervalSeconds = json.get("reportIntervalSeconds")?.asInt ?: DEFAULT_INTERVAL_SECONDS
        prefs.edit()
            .putString(KEY_API_KEY, key)
            .putString(KEY_REPORT_URL, url)
            .apply()
    }

    /** Background reconnect path — uses the stored apiKey, never sends a
     *  pairing code. Returns true on success. On 401 (key invalid, device
     *  removed server-side) clears credentials and surfaces pairing_required. */
    private fun reconnectWithApiKey(): Boolean {
        val base = serverUrl
        val existingKey = apiKey
        if (base.isEmpty() || existingKey == null) return false

        lastStatus = "registering"

        return try {
            val response = httpPost(
                "$base/api/miners/mobile/register",
                buildRegisterBody(),
                apiKey = existingKey
            )
            val json = gson.fromJson(response.body, JsonObject::class.java)
            if (response.code == 200 && json?.get("ok")?.asBoolean == true) {
                persistRegistration(json)
                Log.i(TAG, "Reconnected: reportUrl=${json.get("reportUrl").asString}")
                LogManager.info("PoPManager reconnected")
                lastStatus = "reporting"
                lastError = null
                pairingRequired = false
                true
            } else if (response.code == 401) {
                // Stored key is no longer valid — device was removed or server
                // was wiped. Force the user to re-pair.
                Log.w(TAG, "Reconnect 401 — clearing credentials, re-pair required")
                LogManager.warn("PoPManager: stored key rejected, re-pair required")
                clearCredentials()
                pairingRequired = true
                lastStatus = "pairing_required"
                lastError = "Device removed from PoPManager — re-pair required"
                false
            } else {
                val err = json?.get("error")?.asString ?: "HTTP ${response.code}"
                Log.w(TAG, "Reconnect failed: $err")
                LogManager.warn("PoPManager reconnect failed: $err")
                lastStatus = "error"
                lastError = err
                false
            }
        } catch (e: Exception) {
            Log.e(TAG, "Reconnect error: ${e.message}")
            LogManager.warn("PoPManager unreachable: ${e.message}")
            lastStatus = "error"
            lastError = e.message
            false
        }
    }

    /** User-initiated pairing call. Sends X-Auth-Code with the supplied 6-digit
     *  code. On success, stores the returned apiKey and clears pairing_required.
     *  Returns Result.success(msg) or Result.failure(error) so the UI can show
     *  the outcome directly. */
    suspend fun pairWithCode(rawCode: String): Result<String> = withContext(Dispatchers.IO) {
        val base = serverUrl
        if (base.isEmpty()) {
            return@withContext Result.failure(Exception("Server URL not set"))
        }

        // Strip whitespace, validate shape (6 digits)
        val code = rawCode.filter { !it.isWhitespace() }
        if (code.length != 6 || !code.all { it.isDigit() }) {
            return@withContext Result.failure(Exception("Pairing code must be 6 digits"))
        }

        lastStatus = "registering"

        try {
            val response = httpPost(
                "$base/api/miners/mobile/register",
                buildRegisterBody(),
                authCode = code
            )
            val json = gson.fromJson(response.body, JsonObject::class.java)
            if (response.code == 200 && json?.get("ok")?.asBoolean == true) {
                persistRegistration(json)
                Log.i(TAG, "Paired: reportUrl=${json.get("reportUrl").asString}")
                LogManager.info("PoPManager paired successfully")
                pairingRequired = false
                lastStatus = "reporting"
                lastError = null
                // Wake up the report loop so the first real report fires now
                reportNow()
                Result.success("Paired with PoPManager")
            } else if (response.code == 401) {
                // Spec (PoPManager v1.0.2): any 401 from the register endpoint
                // means the pairing code has been consumed or has rotated. The
                // user-visible string is fixed so PoPManager and PoPMobile
                // agree on the wording.
                val serverErr = json?.get("error")?.asString
                Log.w(TAG, "Pair 401: ${serverErr ?: "expired"}")
                LogManager.warn("PoPManager pairing failed: pairing code expired" +
                    (serverErr?.let { " (server: $it)" } ?: ""))
                pairingRequired = true
                lastStatus = "pairing_required"
                val userMsg = "Pairing code expired. Get a fresh code from PoPManager."
                lastError = userMsg
                Result.failure(Exception(userMsg))
            } else {
                val err = json?.get("error")?.asString ?: "HTTP ${response.code}"
                Log.w(TAG, "Pair failed: $err")
                LogManager.warn("PoPManager pairing failed: $err")
                lastStatus = "error"
                lastError = err
                Result.failure(Exception(err))
            }
        } catch (e: Exception) {
            Log.e(TAG, "Pair error: ${e.message}")
            LogManager.warn("PoPManager unreachable: ${e.message}")
            lastStatus = "error"
            lastError = e.message
            Result.failure(e)
        }
    }

    // ===== Report =====

    private fun sendReport(): Boolean {
        // If we have no credentials at all, we need the user to pair — the
        // caller (start() loop) should have skipped to the idle branch, but
        // guard here too.
        if (apiKey == null) {
            pairingRequired = true
            lastStatus = "pairing_required"
            return false
        }
        // If we have an apiKey but no reportUrl (e.g. server restart), try to
        // reconnect with the stored key first.
        if (reportUrl == null) {
            if (!reconnectWithApiKey()) return false
        }
        val url = reportUrl ?: return false
        val key = apiKey ?: return false

        val snap = statsProvider.snapshot()
        val pendingAcks = loadPendingAcks()

        val body = JsonObject().apply {
            addProperty("deviceId", deviceId)
            addProperty("name", deviceName)
            addProperty("deviceModel", "${Build.MANUFACTURER} ${Build.MODEL}")
            addProperty("osVersion", "Android ${Build.VERSION.RELEASE}")
            addProperty("appVersion", appVersion())
            addProperty("coin", snap.coin)
            addProperty("manufacturer", "Proof of Prints")
            addProperty("model", "Mobile")
            addProperty("pool", snap.pool)
            addProperty("worker", snap.worker)
            addProperty("hashrate", snap.hashrate)
            addProperty("acceptedShares", snap.acceptedShares)
            addProperty("rejectedShares", snap.rejectedShares)
            addProperty("difficulty", snap.difficulty)
            addProperty("runtime", snap.runtimeSeconds)
            addProperty("cpuTemp", snap.cpuTemp)
            addProperty("throttleState", snap.throttleState)
            addProperty("batteryLevel", snap.batteryLevel)
            addProperty("batteryCharging", snap.batteryCharging)
            addProperty("threads", snap.threads)
            addProperty("status", snap.status)
            if (snap.errorMessage != null) addProperty("errorMessage", snap.errorMessage)
            addProperty("timestamp", System.currentTimeMillis())

            // Include any pending acks from previous cycles
            if (pendingAcks.isNotEmpty()) {
                val acksArr = JsonArray()
                for (ack in pendingAcks) {
                    val o = JsonObject()
                    o.addProperty("id", ack.id)
                    o.addProperty("status", ack.status)
                    if (ack.error != null) o.addProperty("error", ack.error)
                    acksArr.add(o)
                }
                add("ackCommands", acksArr)
                Log.d(TAG, "Sending ${pendingAcks.size} acks: ${pendingAcks.map { it.id }}")
            }
        }

        return try {
            val response = httpPost(url, body.toString(), key)
            when (response.code) {
                200 -> {
                    // Acks were accepted (if any) — clear the queue.
                    if (pendingAcks.isNotEmpty()) {
                        savePendingAcks(emptyList())
                        Log.d(TAG, "Cleared ${pendingAcks.size} acks after successful report")
                    }

                    val json = gson.fromJson(response.body, JsonObject::class.java)
                    intervalSeconds = json?.get("nextReportIn")?.asInt ?: intervalSeconds
                    lastReportAt = System.currentTimeMillis()
                    lastStatus = "reporting"
                    lastError = null

                    // Process any commands in the response
                    val commandsEl = json?.get("commands")
                    if (commandsEl != null && commandsEl.isJsonArray) {
                        val commandsArr = commandsEl.asJsonArray
                        Log.d(TAG, "Got commands: $commandsArr")
                        if (commandsArr.size() > 0) {
                            processCommands(commandsArr)
                        }
                    } else {
                        Log.d(TAG, "Got commands: []")
                    }
                    true
                }
                404 -> {
                    // Device record was removed on the server — we can no
                    // longer report under the old key. Force the user to
                    // re-pair with a fresh pairing code.
                    Log.w(TAG, "Device not registered (404) — re-pair required")
                    LogManager.warn("PoPManager: device unknown, re-pair required")
                    clearCredentials()
                    pairingRequired = true
                    lastStatus = "pairing_required"
                    lastError = "Device removed from PoPManager — re-pair required"
                    false
                }
                401 -> {
                    // API key no longer valid — force re-pair.
                    Log.w(TAG, "Bad API key (401) — re-pair required")
                    LogManager.warn("PoPManager: invalid API key, re-pair required")
                    clearCredentials()
                    pairingRequired = true
                    lastStatus = "pairing_required"
                    lastError = "API key rejected — re-pair required"
                    false
                }
                else -> {
                    Log.w(TAG, "Report failed: HTTP ${response.code}")
                    lastStatus = "error"
                    lastError = "HTTP ${response.code}"
                    false
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Report error: ${e.message}")
            lastStatus = "error"
            lastError = e.message
            false
        }
    }

    // ===== Command processing =====

    private fun processCommands(commandsArr: JsonArray) {
        val executor = commandExecutor
        val applied = loadAppliedIds()

        for (el in commandsArr) {
            val obj = try { el.asJsonObject } catch (e: Exception) { continue }
            val id = obj.get("id")?.asString ?: continue
            val type = obj.get("type")?.asString ?: continue
            val params = obj.get("params")?.takeIf { it.isJsonObject }?.asJsonObject ?: JsonObject()

            // Dedup: if we've already applied this command, re-queue the ack
            // (server may have missed our previous ack) and move on.
            if (applied.contains(id)) {
                Log.d(TAG, "Command $id already applied — re-queuing ack")
                queueAck(id, "applied")
                continue
            }

            val cmd = Command(id = id, type = type, params = params)
            Log.d(TAG, "Executing: ${cmd.type} ${cmd.params}")
            LogManager.info("PoPManager command: ${cmd.type}")

            if (executor == null) {
                Log.w(TAG, "No CommandExecutor registered — failing command $id")
                queueAck(id, "failed", "no executor registered")
                continue
            }

            try {
                val error = executor.execute(cmd)
                if (error == null) {
                    markApplied(id)
                    queueAck(id, "applied")
                    LogManager.info("PoPManager command applied: ${cmd.type}")
                } else {
                    queueAck(id, "failed", error)
                    LogManager.warn("PoPManager command failed: ${cmd.type} — $error")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Command executor threw: ${e.message}")
                queueAck(id, "failed", e.message ?: "executor exception")
                LogManager.warn("PoPManager command exception: ${cmd.type} — ${e.message}")
            }
        }
    }

    // ===== HTTP =====

    private data class HttpResponse(val code: Int, val body: String)

    private fun httpPost(
        urlStr: String,
        json: String,
        apiKey: String? = null,
        authCode: String? = null
    ): HttpResponse {
        val url = URL(urlStr)
        val conn = url.openConnection() as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.connectTimeout = CONNECT_TIMEOUT_MS
            conn.readTimeout = READ_TIMEOUT_MS
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/json")
            conn.setRequestProperty("X-Device-ID", deviceId)
            if (apiKey != null) conn.setRequestProperty("X-API-Key", apiKey)
            if (authCode != null) conn.setRequestProperty("X-Auth-Code", authCode)

            OutputStreamWriter(conn.outputStream, Charsets.UTF_8).use { it.write(json) }

            val stream = if (conn.responseCode in 200..299) conn.inputStream else conn.errorStream
            val body = stream?.let {
                BufferedReader(InputStreamReader(it, Charsets.UTF_8)).use { r -> r.readText() }
            } ?: ""
            return HttpResponse(conn.responseCode, body)
        } finally {
            conn.disconnect()
        }
    }

    /** GET /health — one-shot reachability test for the settings screen. */
    suspend fun testConnection(baseUrl: String): Result<String> = withContext(Dispatchers.IO) {
        try {
            val url = URL("${baseUrl.trimEnd('/')}/api/miners/mobile/health")
            val conn = url.openConnection() as HttpURLConnection
            conn.requestMethod = "GET"
            conn.connectTimeout = CONNECT_TIMEOUT_MS
            conn.readTimeout = READ_TIMEOUT_MS
            try {
                if (conn.responseCode == 200) {
                    val body = BufferedReader(InputStreamReader(conn.inputStream, Charsets.UTF_8))
                        .use { it.readText() }
                    val json = gson.fromJson(body, JsonObject::class.java)
                    val version = json?.get("version")?.asString ?: "unknown"
                    Result.success("Connected (v$version)")
                } else {
                    Result.failure(Exception("HTTP ${conn.responseCode}"))
                }
            } finally {
                conn.disconnect()
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
}
