/**
 * Main Activity — Jetpack Compose UI for KAS Mobile Miner.
 *
 * Dashboard shows: hashrate, shares, pool status, settings.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.ui

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.proofofprints.kasminer.LogLevel
import com.proofofprints.kasminer.LogManager
import com.proofofprints.kasminer.R
import com.proofofprints.kasminer.service.MiningService
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {

    private var miningService: MiningService? = null
    private var serviceBound = mutableStateOf(false)

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            miningService = (service as MiningService.MiningBinder).getService()
            serviceBound.value = true
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            miningService = null
            serviceBound.value = false
        }
    }

    private val notificationPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* granted or not, mining still works */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Request notification permission (Android 13+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
            ) {
                notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }

        setContent {
            KASMinerTheme {
                MinerDashboard()
            }
        }
    }

    override fun onStart() {
        super.onStart()
        Intent(this, MiningService::class.java).also { intent ->
            bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
        }
    }

    override fun onStop() {
        super.onStop()
        if (serviceBound.value) {
            unbindService(serviceConnection)
            serviceBound.value = false
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    fun MinerDashboard() {
        // Settings state
        val prefs = getSharedPreferences("kas_miner", Context.MODE_PRIVATE)
        var poolUrl by remember { mutableStateOf(prefs.getString("pool_url", "") ?: "") }
        var wallet by remember { mutableStateOf(prefs.getString("wallet", "") ?: "") }
        var worker by remember { mutableStateOf(prefs.getString("worker", "KASMobile") ?: "KASMobile") }
        var threads by remember { mutableIntStateOf(prefs.getInt("threads", 2)) }
        var showSettings by remember { mutableStateOf(false) }
        var showLogs by remember { mutableStateOf(false) }
        var showAbout by remember { mutableStateOf(false) }

        // Live stats
        var hashrate by remember { mutableDoubleStateOf(0.0) }
        var totalHashes by remember { mutableLongStateOf(0L) }
        var sharesFound by remember { mutableIntStateOf(0) }
        var sharesRejected by remember { mutableIntStateOf(0) }
        var difficulty by remember { mutableDoubleStateOf(0.0) }
        var isRunning by remember { mutableStateOf(false) }
        var poolConnected by remember { mutableStateOf(false) }
        var cpuTemp by remember { mutableFloatStateOf(0f) }
        var batteryPercent by remember { mutableIntStateOf(100) }
        var thermalState by remember { mutableStateOf("NORMAL") }
        var activeThreads by remember { mutableIntStateOf(0) }

        // Update stats periodically
        LaunchedEffect(serviceBound.value) {
            while (true) {
                miningService?.let {
                    hashrate = it.hashrate
                    totalHashes = it.totalHashes
                    sharesFound = it.sharesFound
                    isRunning = it.isRunning
                    poolConnected = it.isPoolConnected
                    cpuTemp = it.cpuTemp
                    batteryPercent = it.batteryPercent
                    thermalState = it.thermalState.name
                    activeThreads = it.activeThreads
                    difficulty = it.currentDifficulty
                    sharesRejected = it.sharesRejected
                }
                delay(1000)
            }
        }

        Scaffold(
            topBar = {
                TopAppBar(
                    title = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Image(
                                painter = painterResource(id = R.drawable.kaspa_logo),
                                contentDescription = "Kaspa Logo",
                                modifier = Modifier.size(32.dp)
                            )
                            Spacer(modifier = Modifier.width(8.dp))
                            Text(
                                "PoPMiner",
                                fontWeight = FontWeight.Bold,
                                fontFamily = FontFamily.Monospace
                            )
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = Color(0xFF1A1A2E),
                        titleContentColor = Color(0xFF49EACB)
                    ),
                    actions = {
                        // Info button with circle border
                        IconButton(onClick = {
                            if (showAbout) { showAbout = false }
                            else { showAbout = true; showSettings = false; showLogs = false }
                        }) {
                            Box(
                                modifier = Modifier
                                    .size(24.dp)
                                    .background(Color.Transparent, shape = androidx.compose.foundation.shape.CircleShape)
                                    .then(Modifier.padding(0.dp)),
                                contentAlignment = Alignment.Center
                            ) {
                                Text(
                                    "i",
                                    color = Color(0xFF49EACB),
                                    fontSize = 14.sp,
                                    fontWeight = FontWeight.Bold,
                                    modifier = Modifier
                                        .background(Color.Transparent, shape = androidx.compose.foundation.shape.CircleShape)
                                )
                                // Circle outline
                                Box(modifier = Modifier
                                    .size(24.dp)
                                    .background(Color.Transparent)
                                    .then(
                                        Modifier.drawBehind {
                                            drawCircle(
                                                color = androidx.compose.ui.graphics.Color(0xFF49EACB),
                                                radius = size.minDimension / 2,
                                                style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2f)
                                            )
                                        }
                                    )
                                )
                            }
                        }
                        // Logs / Dashboard toggle
                        TextButton(onClick = {
                            if (showLogs || showAbout || showSettings) { showLogs = false; showSettings = false; showAbout = false }
                            else { showLogs = true; showSettings = false; showAbout = false }
                        }) {
                            Text(
                                if (showLogs || showAbout || showSettings) "Dashboard" else "Logs",
                                color = Color(0xFF49EACB)
                            )
                        }
                        // Settings button
                        TextButton(onClick = {
                            if (showSettings) { showSettings = false }
                            else { showSettings = true; showLogs = false; showAbout = false }
                        }) {
                            Text("Settings", color = Color(0xFF49EACB))
                        }
                    }
                )
            }
        ) { padding ->
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color(0xFF0F0F23))
                    .padding(padding)
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                if (showAbout) {
                    AboutPanel()
                } else if (showLogs) {
                    LogsPanel()
                } else if (showSettings) {
                    SettingsPanel(
                        poolUrl = poolUrl,
                        wallet = wallet,
                        worker = worker,
                        threads = threads,
                        onPoolUrlChange = { poolUrl = it },
                        onWalletChange = { wallet = it },
                        onWorkerChange = { worker = it },
                        onThreadsChange = { threads = it },
                        onSave = {
                            prefs.edit()
                                .putString("pool_url", poolUrl)
                                .putString("wallet", wallet)
                                .putString("worker", worker)
                                .putInt("threads", threads)
                                .apply()
                        }
                    )
                } else {
                    // Status indicator
                    StatusCard(isRunning = isRunning, poolConnected = poolConnected, difficulty = difficulty)

                    // Hashrate card
                    StatCard(
                        label = "HASHRATE",
                        value = formatHashrate(hashrate),
                        color = Color(0xFF49EACB)
                    )

                    // Stats row
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        StatCard(
                            label = "SHARES",
                            value = if (sharesRejected > 0) "$sharesFound / ${sharesRejected}rej" else sharesFound.toString(),
                            color = Color(0xFFFFD700),
                            modifier = Modifier.weight(1f)
                        )
                        StatCard(
                            label = "HASHES",
                            value = formatHashes(totalHashes),
                            color = Color(0xFF8B5CF6),
                            modifier = Modifier.weight(1f)
                        )
                    }

                    // Threads & thermal row
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        StatCard(
                            label = "THREADS",
                            value = if (isRunning) "$activeThreads / $threads" else "$threads cores",
                            color = Color(0xFFFF6B6B),
                            modifier = Modifier.weight(1f)
                        )
                        StatCard(
                            label = "CPU TEMP",
                            value = if (cpuTemp > 0) "${cpuTemp.toInt()}°C" else "--",
                            color = when {
                                cpuTemp >= 55 -> Color(0xFFFF4444)
                                cpuTemp >= 50 -> Color(0xFFFF8C00)
                                cpuTemp >= 45 -> Color(0xFFFFD700)
                                else -> Color(0xFF49EACB)
                            },
                            modifier = Modifier.weight(1f)
                        )
                    }

                    // Battery & thermal state row
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        StatCard(
                            label = "BATTERY",
                            value = "$batteryPercent%",
                            color = when {
                                batteryPercent <= 10 -> Color(0xFFFF4444)
                                batteryPercent <= 20 -> Color(0xFFFFD700)
                                else -> Color(0xFF49EACB)
                            },
                            modifier = Modifier.weight(1f)
                        )
                        StatCard(
                            label = "THERMAL",
                            value = thermalState,
                            color = when (thermalState) {
                                "CRITICAL" -> Color(0xFFFF4444)
                                "THROTTLE" -> Color(0xFFFF8C00)
                                "WARNING" -> Color(0xFFFFD700)
                                else -> Color(0xFF49EACB)
                            },
                            modifier = Modifier.weight(1f)
                        )
                    }

                    Spacer(modifier = Modifier.height(8.dp))

                    // Start/Stop button
                    Button(
                        onClick = {
                            if (isRunning) {
                                stopMiningService()
                            } else {
                                startMiningService(poolUrl, wallet, worker, threads)
                            }
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(56.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (isRunning) Color(0xFFFF4444) else Color(0xFF49EACB)
                        ),
                        shape = RoundedCornerShape(12.dp),
                        enabled = wallet.isNotEmpty() && poolUrl.isNotEmpty()
                    ) {
                        Text(
                            if (isRunning) "STOP MINING" else "START MINING",
                            fontSize = 18.sp,
                            fontWeight = FontWeight.Bold,
                            fontFamily = FontFamily.Monospace,
                            color = if (isRunning) Color.White else Color.Black
                        )
                    }

                    // Disclaimer
                    Text(
                        "Lottery-style mining. Low hashrate expected on mobile.",
                        color = Color.Gray,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.padding(top = 8.dp)
                    )
                }
            }
        }
    }

    @Composable
    fun StatusCard(isRunning: Boolean, poolConnected: Boolean, difficulty: Double = 0.0) {
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("STATUS", color = Color.Gray, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                    Text(
                        when {
                            isRunning -> "MINING"
                            poolConnected -> "CONNECTED"
                            else -> "IDLE"
                        },
                        color = when {
                            isRunning -> Color(0xFF49EACB)
                            poolConnected -> Color(0xFFFFD700)
                            else -> Color.Gray
                        },
                        fontSize = 20.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )
                }
                Column(horizontalAlignment = Alignment.End) {
                    Text(
                        if (poolConnected) "POOL: ONLINE" else "POOL: OFFLINE",
                        color = if (poolConnected) Color(0xFF49EACB) else Color(0xFFFF4444),
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                    if (difficulty > 0) {
                        Text(
                            "DIFF: ${"%.4f".format(difficulty)}",
                            color = Color(0xFF8B5CF6),
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }
            }
        }
    }

    @Composable
    fun StatCard(
        label: String,
        value: String,
        color: Color,
        modifier: Modifier = Modifier
    ) {
        Card(
            modifier = modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(label, color = Color.Gray, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    value,
                    color = color,
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }

    @Composable
    fun SettingsPanel(
        poolUrl: String,
        wallet: String,
        worker: String,
        threads: Int,
        onPoolUrlChange: (String) -> Unit,
        onWalletChange: (String) -> Unit,
        onWorkerChange: (String) -> Unit,
        onThreadsChange: (Int) -> Unit,
        onSave: () -> Unit
    ) {
        val maxThreads = Runtime.getRuntime().availableProcessors()
        var showSaved by remember { mutableStateOf(false) }

        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text(
                    "MINER CONFIGURATION",
                    color = Color(0xFF49EACB),
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )

                MinerTextField("Pool URL (host:port)", poolUrl, onPoolUrlChange)
                MinerTextField("Wallet Address", wallet, onWalletChange)
                MinerTextField("Worker Name", worker, onWorkerChange)

                // Thread count slider
                Text("Threads: $threads / $maxThreads", color = Color.White, fontFamily = FontFamily.Monospace)
                Slider(
                    value = threads.toFloat(),
                    onValueChange = { onThreadsChange(it.toInt()) },
                    valueRange = 1f..maxThreads.toFloat(),
                    steps = maxThreads - 2,
                    colors = SliderDefaults.colors(
                        thumbColor = Color(0xFF49EACB),
                        activeTrackColor = Color(0xFF49EACB)
                    )
                )

                Text(
                    "Warning: Using more threads increases heat and battery drain.",
                    color = Color(0xFFFF6B6B),
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace
                )

                Button(
                    onClick = { onSave(); showSaved = true },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF49EACB)),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Text("SAVE", color = Color.Black, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace)
                }

                if (showSaved) {
                    LaunchedEffect(Unit) {
                        delay(2000)
                        showSaved = false
                    }
                    Text(
                        "Settings saved!",
                        color = Color(0xFF49EACB),
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.fillMaxWidth(),
                        textAlign = TextAlign.Center
                    )
                }
            }
        }
    }

    @Composable
    fun MinerTextField(label: String, value: String, onValueChange: (String) -> Unit) {
        OutlinedTextField(
            value = value,
            onValueChange = onValueChange,
            label = { Text(label, fontFamily = FontFamily.Monospace) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            colors = OutlinedTextFieldDefaults.colors(
                focusedTextColor = Color.White,
                unfocusedTextColor = Color.White,
                focusedBorderColor = Color(0xFF49EACB),
                unfocusedBorderColor = Color(0xFF333333),
                focusedLabelColor = Color(0xFF49EACB),
                unfocusedLabelColor = Color.Gray,
                cursorColor = Color(0xFF49EACB)
            ),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii)
        )
    }

    private fun startMiningService(poolUrl: String, wallet: String, worker: String, threads: Int) {
        // Parse host:port
        val parts = poolUrl.replace("stratum+tcp://", "").split(":")
        if (parts.size != 2) return

        val intent = Intent(this, MiningService::class.java).apply {
            action = MiningService.ACTION_START
        }

        // Configure service before starting
        Intent(this, MiningService::class.java).also { bindIntent ->
            bindService(bindIntent, object : ServiceConnection {
                override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
                    val svc = (service as MiningService.MiningBinder).getService()
                    svc.poolHost = parts[0]
                    svc.poolPort = parts[1].toIntOrNull() ?: 3333
                    svc.walletAddress = wallet
                    svc.workerName = worker
                    svc.threadCount = threads
                    unbindService(this)

                    // Now start as foreground service
                    ContextCompat.startForegroundService(this@MainActivity, intent)
                }

                override fun onServiceDisconnected(name: ComponentName?) {}
            }, Context.BIND_AUTO_CREATE)
        }
    }

    private fun stopMiningService() {
        val intent = Intent(this, MiningService::class.java).apply {
            action = MiningService.ACTION_STOP
        }
        startService(intent)
    }

    private fun formatHashrate(hr: Double): String = when {
        hr >= 1_000_000 -> String.format("%.2f MH/s", hr / 1_000_000)
        hr >= 1_000 -> String.format("%.2f KH/s", hr / 1_000)
        hr > 0 -> String.format("%.1f H/s", hr)
        else -> "0 H/s"
    }

    private fun formatHashes(count: Long): String = when {
        count >= 1_000_000_000 -> String.format("%.1fG", count / 1_000_000_000.0)
        count >= 1_000_000 -> String.format("%.1fM", count / 1_000_000.0)
        count >= 1_000 -> String.format("%.1fK", count / 1_000.0)
        else -> count.toString()
    }

    @Composable
    fun LogsPanel() {
        val logs = LogManager.entries
        val dateFormat = remember { java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.getDefault()) }
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        "MINING LOG (${logs.size})",
                        color = Color(0xFF49EACB),
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )
                    TextButton(onClick = { LogManager.clear() }) {
                        Text("Clear", color = Color.Gray)
                    }
                }
                Text(
                    "Logs are kept for up to 4 hours",
                    color = Color.Gray,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(modifier = Modifier.height(8.dp))
                logs.reversed().forEach { entry ->
                    val ts = dateFormat.format(java.util.Date(entry.timestamp))
                    val tag = when (entry.level) {
                        LogLevel.INFO -> "[INF]"
                        LogLevel.WARN -> "[WRN]"
                        LogLevel.ERROR -> "[ERR]"
                    }
                    Text(
                        "$ts $tag ${entry.message}",
                        color = when (entry.level) {
                            LogLevel.ERROR -> Color(0xFFFF4444)
                            LogLevel.WARN -> Color(0xFFFFD700)
                            LogLevel.INFO -> Color(0xFF49EACB)
                        },
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.padding(vertical = 2.dp)
                    )
                }
            }
        }
    }

    @Composable
    fun AboutPanel() {
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Column(
                modifier = Modifier.padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Image(
                    painter = painterResource(id = R.drawable.kaspa_logo_large),
                    contentDescription = "Kaspa Logo",
                    modifier = Modifier.size(120.dp)
                )
                Text(
                    "PoPMiner",
                    color = Color(0xFF49EACB),
                    fontSize = 24.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
                Image(
                    painter = painterResource(id = R.drawable.pop_logo),
                    contentDescription = "Proof of Prints Logo",
                    modifier = Modifier.size(80.dp)
                )
                Text(
                    "v1.0.0",
                    color = Color.Gray,
                    fontSize = 14.sp,
                    fontFamily = FontFamily.Monospace
                )
                Divider(
                    color = Color(0xFF333333),
                    thickness = 1.dp,
                    modifier = Modifier.padding(vertical = 8.dp)
                )
                Text(
                    "Developed by Proof of Prints",
                    color = Color.White,
                    fontSize = 16.sp,
                    fontFamily = FontFamily.Monospace
                )
                Text(
                    "support@proofofprints.com",
                    color = Color(0xFF49EACB),
                    fontSize = 14.sp,
                    fontFamily = FontFamily.Monospace
                )
                Text(
                    "https://www.proofofprints.com",
                    color = Color(0xFF49EACB),
                    fontSize = 14.sp,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(modifier = Modifier.height(16.dp))
                Text(
                    "Kaspa kHeavyHash mobile miner for Android.\nLottery-style mining — low hashrate expected.",
                    color = Color.Gray,
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    textAlign = TextAlign.Center
                )
            }
        }
    }
}
