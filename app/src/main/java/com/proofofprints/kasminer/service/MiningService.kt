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
import android.os.PowerManager
import android.util.Log
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
    private lateinit var thermalMonitor: ThermalMonitor

    // Mining config
    var poolHost: String = ""
    var poolPort: Int = 0
    var walletAddress: String = ""
    var workerName: String = "KASMobile"
    var threadCount: Int = 2
    var currentDifficulty: Double = 1.0

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

        // Acquire wake lock to prevent CPU sleep
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "KASMiner::MiningLock")
        wakeLock?.acquire(4 * 60 * 60 * 1000L) // 4 hour max

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
        miningEngine.stop()
        stratumClient.disconnect()
        wakeLock?.let {
            if (it.isHeld) it.release()
        }
        wakeLock = null
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
        updateNotification("Connected, waiting for job...")
    }

    override fun onDisconnected(reason: String) {
        Log.w(TAG, "Pool disconnected: $reason")
        miningEngine.stop()
        updateNotification("Disconnected: $reason")

        // Attempt reconnect after delay
        serviceScope.launch {
            delay(5000)
            if (!stratumClient.isConnected) {
                Log.i(TAG, "Attempting reconnect...")
                stratumClient.connect(poolHost, poolPort, walletAddress, workerName)
            }
        }
    }

    override fun onNewJob(jobId: String, headerHash: ByteArray, timestamp: Long) {
        val target = miningEngine.setTargetFromDifficulty(currentDifficulty)
        miningEngine.setJob(headerHash, jobId, target)

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
    }

    override fun onShareAccepted() {
        Log.i(TAG, "Share accepted!")
        onShareSubmitted?.invoke()
    }

    override fun onShareRejected(reason: String) {
        Log.w(TAG, "Share rejected: $reason")
    }

    // ===== MiningEngine.ShareCallback =====

    override fun onShareFound(jobId: String, nonce: Long) {
        Log.i(TAG, "Share found! Job: $jobId, Nonce: $nonce")
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
