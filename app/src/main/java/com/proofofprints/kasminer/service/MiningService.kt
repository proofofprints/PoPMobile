/**
 * Android Foreground Service for mining.
 *
 * Keeps mining alive in the background with a persistent notification.
 * Manages the lifecycle of the native mining engine and stratum client.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.service

import android.app.*
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.net.wifi.WifiManager
import android.os.PowerManager
import android.util.Log
import com.proofofprints.kasminer.LogManager
import com.proofofprints.kasminer.mining.MiningEngine
import com.proofofprints.kasminer.popmanager.PoPManagerReporter
import com.proofofprints.kasminer.stratum.StratumClient
import com.proofofprints.kasminer.ui.MainActivity
import kotlinx.coroutines.*

class MiningService : Service(), StratumClient.StratumListener, MiningEngine.ShareCallback {

    companion object {
        private const val TAG = "MiningService"
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "kas_mining_channel"

        const val ACTION_START = "com.proofofprints.kasminer.START"
        const val ACTION_STOP = "com.proofofprints.kasminer.STOP"
        const val ACTION_ENSURE_RUNNING = "com.proofofprints.kasminer.ENSURE_RUNNING"
    }

    inner class MiningBinder : Binder() {
        fun getService(): MiningService = this@MiningService
    }

    private val binder = MiningBinder()
    private val miningEngine = MiningEngine()
    private val stratumClient = StratumClient(this)
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    lateinit var popManagerReporter: PoPManagerReporter
        private set
    private var miningStartedAt: Long = 0L
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private lateinit var thermalMonitor: ThermalMonitor
    @Volatile
    private var isStopping: Boolean = false

    // Mining config
    var poolHost: String = ""
    var poolPort: Int = 0
    var walletAddress: String = ""
    var workerName: String = "PoPMobile"
    var threadCount: Int = 2
    var currentDifficulty: Double = 0.0014

    // Thermal state exposed to UI
    var cpuTemp: Float = 0f
        private set
    var batteryPercent: Int = 100
        private set
    var isCharging: Boolean = false
        private set
    var thermalState: ThermalMonitor.ThermalState = ThermalMonitor.ThermalState.NORMAL
        private set
    var activeThreads: Int = 0
        private set
    private var thermalPaused: Boolean = false

    // Stats exposed to UI
    val hashrate: Double get() = miningEngine.hashrate
    val totalHashes: Long get() = miningEngine.totalHashes
    val sharesFound: Int get() = miningEngine.sharesFound
    val sharesRejected: Int get() = miningEngine.sharesRejected
    val isRunning: Boolean get() = miningEngine.isRunning
    val isPoolConnected: Boolean get() = stratumClient.isConnected

    enum class PoolState { DISCONNECTED, CONNECTING, CONNECTED, ERROR }

    /** Richer pool state for the UI — tracks whether we're mid-retry so the
     *  user can cancel a failing connection. */
    @Volatile var poolState: PoolState = PoolState.DISCONNECTED
        private set
    @Volatile var poolErrorReason: String? = null
        private set

    /** True when the user has tapped Start and we're actively trying to mine,
     *  even if the pool hasn't connected yet. Distinct from miningEngine.isRunning
     *  because the engine only starts after the first job arrives. */
    @Volatile var isSessionActive: Boolean = false
        private set

    // Listener for UI updates
    var onStatsUpdate: (() -> Unit)? = null
    var onShareSubmitted: (() -> Unit)? = null
    var onThermalWarning: ((ThermalMonitor.ThermalState, Float) -> Unit)? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        miningEngine.setShareCallback(this)
        thermalMonitor = ThermalMonitor(this)
        popManagerReporter = PoPManagerReporter(this, buildStatsProvider())
        popManagerReporter.setCommandExecutor(buildCommandExecutor())

        // Load saved mining config so telemetry has meaningful pool/worker values
        // even when the miner isn't actively running yet.
        val prefs = getSharedPreferences("kas_miner", Context.MODE_PRIVATE)
        val savedPoolUrl = prefs.getString("pool_url", "") ?: ""
        if (savedPoolUrl.isNotEmpty()) {
            val cleaned = savedPoolUrl.replace("stratum+tcp://", "")
            val parts = cleaned.split(":")
            if (parts.size == 2) {
                poolHost = parts[0]
                poolPort = parts[1].toIntOrNull() ?: 0
            }
        }
        walletAddress = prefs.getString("wallet", "") ?: ""
        workerName = prefs.getString("worker", "PoPMobile") ?: "PoPMobile"
        threadCount = prefs.getInt("threads", 2)

        // Start the reporter immediately — it runs independently of mining so
        // PoPManager sees the device even when stopped/paused.
        popManagerReporter.start()
    }

    private fun buildStatsProvider() = object : PoPManagerReporter.StatsProvider {
        override fun snapshot(): PoPManagerReporter.TelemetrySnapshot {
            // Always sample current thermal/battery state even when mining is
            // off — otherwise telemetry goes stale in idle mode.
            if (!miningEngine.isRunning && !thermalPaused) {
                try {
                    val s = thermalMonitor.getStatus(threadCount)
                    cpuTemp = s.cpuTemp
                    batteryPercent = s.batteryPercent
                    isCharging = s.isCharging
                    thermalState = s.thermalState
                } catch (e: Exception) { /* non-fatal */ }
            }
            val runtime = if (miningStartedAt > 0)
                (System.currentTimeMillis() - miningStartedAt) / 1000 else 0L
            val status = when {
                thermalPaused -> "paused"
                miningEngine.isRunning -> "mining"
                else -> "stopped"
            }
            return PoPManagerReporter.TelemetrySnapshot(
                coin = "KAS",
                pool = if (poolHost.isNotEmpty()) "stratum+tcp://$poolHost:$poolPort" else "",
                worker = if (walletAddress.isNotEmpty()) "$walletAddress.$workerName" else workerName,
                hashrate = miningEngine.hashrate,
                acceptedShares = miningEngine.sharesFound,
                rejectedShares = miningEngine.sharesRejected,
                difficulty = currentDifficulty,
                runtimeSeconds = runtime,
                cpuTemp = cpuTemp,
                throttleState = mapThrottleState(thermalState),
                batteryLevel = batteryPercent,
                batteryCharging = isCharging,
                threads = if (miningEngine.isRunning) activeThreads else threadCount,
                status = status
            )
        }
    }

    private fun buildCommandExecutor() = object : PoPManagerReporter.CommandExecutor {
        override fun execute(command: PoPManagerReporter.Command): String? {
            return try {
                when (command.type) {
                    "set_config" -> handleSetConfig(command.params)
                    "set_threads" -> handleSetThreads(command.params)
                    "start" -> handleRemoteStart()
                    "stop" -> handleRemoteStop()
                    "restart" -> handleRemoteRestart()
                    else -> "unknown command type: ${command.type}"
                }
            } catch (e: Exception) {
                Log.e(TAG, "Command ${command.type} threw: ${e.message}")
                e.message ?: "executor exception"
            }
        }
    }

    private fun handleSetConfig(params: com.google.gson.JsonObject): String? {
        val prefs = getSharedPreferences("kas_miner", Context.MODE_PRIVATE).edit()
        var changedPool = false
        var changedThreads = false

        params.get("poolUrl")?.takeIf { !it.isJsonNull }?.asString?.let { url ->
            val cleaned = url.replace("stratum+tcp://", "")
            val parts = cleaned.split(":")
            if (parts.size != 2) return "invalid poolUrl: $url"
            val newHost = parts[0]
            val newPort = parts[1].toIntOrNull() ?: return "invalid port in poolUrl: $url"
            if (newHost != poolHost || newPort != poolPort) {
                poolHost = newHost
                poolPort = newPort
                changedPool = true
            }
            prefs.putString("pool_url", url)
        }
        params.get("wallet")?.takeIf { !it.isJsonNull }?.asString?.let { wallet ->
            if (wallet != walletAddress) {
                walletAddress = wallet
                changedPool = true
            }
            prefs.putString("wallet", wallet)
        }
        params.get("worker")?.takeIf { !it.isJsonNull }?.asString?.let { w ->
            if (w != workerName) {
                workerName = w
                changedPool = true
            }
            prefs.putString("worker", w)
        }
        params.get("threads")?.takeIf { !it.isJsonNull }?.asInt?.let { t ->
            if (t < 1 || t > 16) return "threads out of range: $t"
            if (t != threadCount) {
                threadCount = t
                changedThreads = true
            }
            prefs.putInt("threads", t)
        }
        prefs.apply()

        // Notify UI to refresh fields
        onStatsUpdate?.invoke()

        // If currently mining and the pool-side config changed, restart the
        // stratum connection + worker threads to pick up the new values.
        if (miningEngine.isRunning && changedPool) {
            Log.i(TAG, "set_config: restarting mining to apply new pool/wallet/worker")
            restartMiningInternal()
        } else if (miningEngine.isRunning && changedThreads) {
            // Threads-only change: reuse thermal restart path
            Log.i(TAG, "set_config: adjusting thread count to $threadCount")
            miningEngine.stop()
            miningEngine.start(threadCount)
            activeThreads = threadCount
        }

        return null
    }

    private fun handleSetThreads(params: com.google.gson.JsonObject): String? {
        val threads = params.get("threads")?.takeIf { !it.isJsonNull }?.asInt
            ?: return "missing threads param"
        if (threads < 1 || threads > 16) return "threads out of range: $threads"

        threadCount = threads
        getSharedPreferences("kas_miner", Context.MODE_PRIVATE)
            .edit().putInt("threads", threads).apply()
        onStatsUpdate?.invoke()

        if (miningEngine.isRunning) {
            Log.i(TAG, "set_threads: restarting engine with $threads threads")
            miningEngine.stop()
            miningEngine.start(threads)
            activeThreads = threads
        }
        return null
    }

    private fun handleRemoteStart(): String? {
        if (miningEngine.isRunning) return null  // already running = success
        if (poolHost.isEmpty() || walletAddress.isEmpty()) {
            return "cannot start: pool or wallet not configured"
        }
        Log.i(TAG, "Remote start command")
        startMining()
        return null
    }

    private fun handleRemoteStop(): String? {
        if (!miningEngine.isRunning) return null  // already stopped = success
        Log.i(TAG, "Remote stop command")
        stopMining()  // keeps the reporter alive since serverUrl is set
        return null
    }

    private fun handleRemoteRestart(): String? {
        handleRemoteStop()
        Thread.sleep(500)
        return handleRemoteStart()
    }

    private fun restartMiningInternal() {
        serviceScope.launch {
            try {
                miningEngine.stop()
                stratumClient.disconnect()
                stratumClient.connect(poolHost, poolPort, walletAddress, workerName)
                // Mining engine will be restarted by the stratum job handler
            } catch (e: Exception) {
                Log.e(TAG, "restartMiningInternal failed: ${e.message}")
            }
        }
    }

    private fun mapThrottleState(state: ThermalMonitor.ThermalState): String = when (state) {
        ThermalMonitor.ThermalState.NORMAL -> "normal"
        ThermalMonitor.ThermalState.WARNING -> "light"
        ThermalMonitor.ThermalState.THROTTLE -> "moderate"
        ThermalMonitor.ThermalState.CRITICAL -> "critical"
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> startMining()
            ACTION_STOP -> stopMining()
            ACTION_ENSURE_RUNNING -> ensureRunningForReporting()
        }
        return START_STICKY
    }

    /** Called when the app launches and PoPManager is configured but mining
     *  isn't active. Promotes the service to foreground so the reporter stays
     *  alive when the app is backgrounded. */
    private fun ensureRunningForReporting() {
        if (miningEngine.isRunning) return  // already in foreground for mining
        Log.i(TAG, "ensureRunningForReporting: promoting to foreground")
        startForeground(NOTIFICATION_ID, createNotification("Idle — reporting to PoPManager"))
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        Log.i(TAG, "Service onDestroy — tearing down reporter and mining")
        miningEngine.stop()
        stratumClient.disconnect()
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        popManagerReporter.stop()
        serviceScope.cancel()
        super.onDestroy()
    }

    fun startMining() {
        Log.i(TAG, "Starting mining service...")
        LogManager.info("Mining service starting ($threadCount threads)")
        isStopping = false
        isSessionActive = true
        poolState = PoolState.CONNECTING
        poolErrorReason = null
        miningStartedAt = System.currentTimeMillis()
        // Reporter is already running from onCreate — just fire an immediate
        // status update so PoPManager sees the transition quickly.
        popManagerReporter.reportNow()

        // Acquire wake lock to prevent CPU sleep
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "KASMiner::MiningLock")
        wakeLock?.acquire(4 * 60 * 60 * 1000L) // 4 hour max

        // Acquire WiFi lock to prevent WiFi from being disabled during mining
        @Suppress("DEPRECATION")
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "KASMiner:WifiLock")
        wifiLock?.acquire()

        // Start foreground notification
        startForeground(NOTIFICATION_ID, createNotification("Connecting to pool..."))

        // Connect to pool then start mining
        serviceScope.launch {
            try {
                stratumClient.connect(poolHost, poolPort, walletAddress, workerName)

                // Start hashrate update loop
                launch { statsUpdateLoop() }
            } catch (e: Exception) {
                val reason = friendlyConnectError(e)
                Log.e(TAG, "Failed to start mining: $reason")
                LogManager.warn("Pool connect failed: $reason")
                poolState = PoolState.ERROR
                poolErrorReason = reason
                updateNotification("Pool error: $reason")
            }
        }
    }

    /** Convert a network exception to a short, user-friendly reason string. */
    private fun friendlyConnectError(e: Throwable): String {
        val msg = e.message.orEmpty()
        return when {
            e is java.net.ConnectException && msg.contains("refused", true) -> "Refused"
            e is java.net.SocketTimeoutException -> "Timed out"
            e is java.net.UnknownHostException -> "DNS failed"
            msg.contains("Network is unreachable", true) -> "No network"
            msg.contains("refused", true) -> "Refused"
            msg.contains("timed out", true) -> "Timed out"
            msg.contains("unreachable", true) -> "Unreachable"
            else -> msg.take(40).ifEmpty { e.javaClass.simpleName }
        }
    }

    fun stopMining() {
        Log.i(TAG, "Stopping mining...")
        isStopping = true
        isSessionActive = false
        poolState = PoolState.DISCONNECTED
        poolErrorReason = null
        miningStartedAt = 0L
        miningEngine.stop()
        stratumClient.disconnect()
        wakeLock?.let {
            if (it.isHeld) it.release()
        }
        wakeLock = null
        wifiLock?.let {
            if (it.isHeld) it.release()
        }
        wifiLock = null

        // Keep the service alive so the PoPManager reporter keeps running
        // and can receive remote commands. Update the foreground notification
        // to reflect the new state. If PoPManager is NOT configured, there's
        // no reason to stay resident — tear the whole thing down.
        if (popManagerReporter.serverUrl.isNotEmpty()) {
            popManagerReporter.reportNow()
            updateNotificationForIdle()
            Log.i(TAG, "Service staying resident for PoPManager reporting")
        } else {
            stopForeground(STOP_FOREGROUND_REMOVE)
            popManagerReporter.stop()
            stopSelf()
        }
    }

    /** Fully tear down the service — used on explicit user exit. */
    fun shutdown() {
        Log.i(TAG, "Shutting down mining service")
        stopMining()
        if (popManagerReporter.serverUrl.isNotEmpty()) {
            popManagerReporter.stop()
        }
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun updateNotificationForIdle() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as android.app.NotificationManager
        nm.notify(NOTIFICATION_ID, createNotification("Idle — reporting to PoPManager"))
    }

    private suspend fun statsUpdateLoop() {
        while (miningEngine.isRunning || thermalPaused) {
            // Thermal check every cycle
            val status = thermalMonitor.getStatus(threadCount)
            cpuTemp = status.cpuTemp
            batteryPercent = status.batteryPercent
            isCharging = status.isCharging
            thermalState = status.thermalState

            when (status.thermalState) {
                ThermalMonitor.ThermalState.CRITICAL -> {
                    if (miningEngine.isRunning) {
                        Log.w(TAG, "THERMAL CRITICAL (${cpuTemp}°C) — pausing mining!")
                        miningEngine.stop()
                        thermalPaused = true
                        activeThreads = 0
                        onThermalWarning?.invoke(status.thermalState, cpuTemp)
                    }
                }
                ThermalMonitor.ThermalState.THROTTLE -> {
                    val recommended = status.recommendedThreads
                    if (miningEngine.isRunning && recommended < activeThreads) {
                        Log.w(TAG, "THERMAL THROTTLE (${cpuTemp}°C) — reducing to $recommended threads")
                        miningEngine.stop()
                        miningEngine.start(recommended)
                        activeThreads = recommended
                        onThermalWarning?.invoke(status.thermalState, cpuTemp)
                    }
                }
                ThermalMonitor.ThermalState.NORMAL -> {
                    // Resume from thermal pause if cooled down
                    if (thermalPaused && !miningEngine.isRunning) {
                        Log.i(TAG, "Thermal normal (${cpuTemp}°C) — resuming mining")
                        miningEngine.start(threadCount)
                        activeThreads = threadCount
                        thermalPaused = false
                    }
                }
                ThermalMonitor.ThermalState.WARNING -> {
                    onThermalWarning?.invoke(status.thermalState, cpuTemp)
                }
            }

            updateNotification()
            onStatsUpdate?.invoke()
            delay(3000) // Check every 3 seconds
        }
    }

    // ===== StratumListener callbacks =====

    override fun onConnected() {
        Log.i(TAG, "Pool connected!")
        LogManager.info("Pool connected successfully")
        poolState = PoolState.CONNECTED
        poolErrorReason = null
        updateNotification("Connected, waiting for job...")
    }

    override fun onDisconnected(reason: String) {
        Log.w(TAG, "Pool disconnected: $reason")
        LogManager.warn("Pool disconnected: $reason")
        val friendly = friendlyConnectError(RuntimeException(reason))
        poolState = if (isStopping) PoolState.DISCONNECTED else PoolState.ERROR
        poolErrorReason = if (isStopping) null else friendly
        miningEngine.stop()
        updateNotification("Pool error: $friendly")

        // Attempt reconnect with exponential backoff
        serviceScope.launch {
            var delayMs = 5_000L
            val maxDelayMs = 60_000L
            val maxRetries = 10

            for (attempt in 1..maxRetries) {
                if (isStopping) {
                    Log.i(TAG, "Service stopping, aborting reconnect")
                    return@launch
                }
                if (stratumClient.isConnected) {
                    Log.i(TAG, "Already reconnected, aborting retry loop")
                    return@launch
                }

                Log.i(TAG, "Reconnect attempt $attempt/$maxRetries in ${delayMs / 1000}s...")
                poolState = PoolState.CONNECTING
                updateNotification("Reconnecting ($attempt/$maxRetries)...")
                delay(delayMs)

                if (isStopping) return@launch

                try {
                    stratumClient.connect(poolHost, poolPort, walletAddress, workerName)
                    if (stratumClient.isConnected) {
                        Log.i(TAG, "Reconnected on attempt $attempt")
                        return@launch
                    }
                } catch (e: Exception) {
                    val reason = friendlyConnectError(e)
                    Log.w(TAG, "Reconnect attempt $attempt failed: $reason")
                    poolState = PoolState.ERROR
                    poolErrorReason = reason
                }

                delayMs = minOf(delayMs * 2, maxDelayMs)
            }

            if (!isStopping) {
                Log.e(TAG, "All $maxRetries reconnect attempts failed")
                poolState = PoolState.ERROR
                if (poolErrorReason == null) poolErrorReason = "No response"
                updateNotification("Connection lost — all retries exhausted")
            }
        }
    }

    override fun onNewJob(jobId: String, headerHash: ByteArray, timestamp: Long) {
        LogManager.info("New job received: $jobId")
        val target = miningEngine.setTargetFromDifficulty(currentDifficulty)
        miningEngine.setJob(headerHash, jobId, target, timestamp)

        // Start mining threads if not already running
        if (!miningEngine.isRunning && !thermalPaused) {
            miningEngine.start(threadCount)
            activeThreads = threadCount
            updateNotification("Mining...")
        }
    }

    override fun onDifficultyChanged(difficulty: Double) {
        currentDifficulty = difficulty
        Log.i(TAG, "Difficulty updated: $difficulty")
        LogManager.info("Difficulty set to $difficulty")
    }

    override fun onShareAccepted() {
        Log.i(TAG, "Share accepted!")
        LogManager.info("Share accepted!")
        onShareSubmitted?.invoke()
    }

    override fun onShareRejected(reason: String) {
        Log.w(TAG, "Share rejected: $reason")
        LogManager.warn("Share rejected: $reason")
        miningEngine.incrementRejected()
    }

    override fun onExtranonceSet(extranonce: String, extranonceShifted: Long, extranonce2Bits: Int) {
        Log.i(TAG, "Extranonce set: $extranonce (prefix=0x${String.format("%016x", extranonceShifted)}, en2bits=$extranonce2Bits)")
        LogManager.info("Extranonce: $extranonce")
        miningEngine.setExtranonce(extranonceShifted, extranonce2Bits)
    }

    // ===== MiningEngine.ShareCallback =====

    override fun onShareFound(jobId: String, nonce: Long) {
        Log.i(TAG, "Share found! Job: $jobId, Nonce: $nonce")
        LogManager.info("Share found! Job: $jobId")
        stratumClient.submitShare(jobId, nonce)
    }

    // ===== Notification management =====

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "KAS Mining",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Shows mining status"
                setShowBadge(false)
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun createNotification(status: String): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val stopIntent = PendingIntent.getService(
            this, 1,
            Intent(this, MiningService::class.java).apply { action = ACTION_STOP },
            PendingIntent.FLAG_IMMUTABLE
        )

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("PoPMobile")
            .setContentText(status)
            .setSmallIcon(android.R.drawable.ic_menu_manage)
            .setContentIntent(pendingIntent)
            .addAction(Notification.Action.Builder(
                null, "Stop Mining", stopIntent
            ).build())
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(status: String? = null) {
        val thermalInfo = if (cpuTemp > 0) " | ${cpuTemp.toInt()}°C" else ""
        val text = status ?: String.format(
            "%.2f H/s | %d shares%s",
            hashrate, sharesFound, thermalInfo
        )
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIFICATION_ID, createNotification(text))
    }

    private fun formatHashes(count: Long): String = when {
        count >= 1_000_000 -> String.format("%.1fM", count / 1_000_000.0)
        count >= 1_000 -> String.format("%.1fK", count / 1_000.0)
        else -> count.toString()
    }
}
