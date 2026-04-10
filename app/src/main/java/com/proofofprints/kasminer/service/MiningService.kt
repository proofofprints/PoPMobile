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
    }

    inner class MiningBinder : Binder() {
        fun getService(): MiningService = this@MiningService
    }

    private val binder = MiningBinder()
    private val miningEngine = MiningEngine()
    private val stratumClient = StratumClient(this)
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private lateinit var thermalMonitor: ThermalMonitor
    @Volatile
    private var isStopping: Boolean = false

    // Mining config
    var poolHost: String = ""
    var poolPort: Int = 0
    var walletAddress: String = ""
    var workerName: String = "KASMobile"
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

    // Listener for UI updates
    var onStatsUpdate: (() -> Unit)? = null
    var onShareSubmitted: (() -> Unit)? = null
    var onThermalWarning: ((ThermalMonitor.ThermalState, Float) -> Unit)? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        miningEngine.setShareCallback(this)
        thermalMonitor = ThermalMonitor(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> startMining()
            ACTION_STOP -> stopMining()
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        stopMining()
        serviceScope.cancel()
        super.onDestroy()
    }

    fun startMining() {
        Log.i(TAG, "Starting mining service...")
        LogManager.info("Mining service starting ($threadCount threads)")
        isStopping = false

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
                Log.e(TAG, "Failed to start mining: ${e.message}")
            }
        }
    }

    fun stopMining() {
        Log.i(TAG, "Stopping mining service...")
        isStopping = true
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
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
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
        updateNotification("Connected, waiting for job...")
    }

    override fun onDisconnected(reason: String) {
        Log.w(TAG, "Pool disconnected: $reason")
        LogManager.warn("Pool disconnected: $reason")
        miningEngine.stop()
        updateNotification("Disconnected: $reason")

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
                    Log.w(TAG, "Reconnect attempt $attempt failed: ${e.message}")
                }

                delayMs = minOf(delayMs * 2, maxDelayMs)
            }

            if (!isStopping) {
                Log.e(TAG, "All $maxRetries reconnect attempts failed")
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
            .setContentTitle("KAS Miner")
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
