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
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    private var miningService: MiningService? = null
    private var serviceBound = mutableStateOf(false)

    // Callback registered when launchWalletQrScanner is called, invoked when
    // the ZXing scanner activity returns a result.
    private var pendingScanResult: ((String) -> Unit)? = null

    private val qrScanLauncher = registerForActivityResult(
        com.journeyapps.barcodescanner.ScanContract()
    ) { result ->
        val cb = pendingScanResult
        pendingScanResult = null
        val raw = result.contents
        if (raw == null || cb == null) return@registerForActivityResult
        handleScannedWalletAddress(raw, cb)
    }

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
        // If the user has configured PoPManager, start the service in the
        // foreground so the reporter keeps running when the app is backgrounded.
        val popPrefs = getSharedPreferences("popmanager", Context.MODE_PRIVATE)
        val serverUrl = popPrefs.getString("server_url", "") ?: ""
        if (serverUrl.isNotEmpty()) {
            val startIntent = Intent(this, MiningService::class.java).apply {
                action = MiningService.ACTION_ENSURE_RUNNING
            }
            ContextCompat.startForegroundService(this, startIntent)
        }
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
        var worker by remember { mutableStateOf(prefs.getString("worker", "PoPMobile") ?: "PoPMobile") }
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
        var poolState by remember { mutableStateOf(MiningService.PoolState.DISCONNECTED) }
        var poolErrorReason by remember { mutableStateOf<String?>(null) }
        var isSessionActive by remember { mutableStateOf(false) }
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
                    poolState = it.poolState
                    poolErrorReason = it.poolErrorReason
                    isSessionActive = it.isSessionActive
                    cpuTemp = it.cpuTemp
                    batteryPercent = it.batteryPercent
                    thermalState = it.thermalState.name
                    activeThreads = it.activeThreads
                    difficulty = it.currentDifficulty
                    sharesRejected = it.sharesRejected
                }

                // Re-read config from prefs so remote PoPManager commands
                // (set_config / set_threads) reflect in the UI within ~1s of
                // being applied. Only update when the stored value differs
                // from the current state to avoid fighting user input while
                // they're typing in the Settings panel.
                val freshPoolUrl = prefs.getString("pool_url", "") ?: ""
                if (freshPoolUrl != poolUrl && !showSettings) poolUrl = freshPoolUrl
                val freshWallet = prefs.getString("wallet", "") ?: ""
                if (freshWallet != wallet && !showSettings) wallet = freshWallet
                val freshWorker = prefs.getString("worker", "PoPMobile") ?: "PoPMobile"
                if (freshWorker != worker && !showSettings) worker = freshWorker
                val freshThreads = prefs.getInt("threads", 2)
                if (freshThreads != threads && !showSettings) threads = freshThreads

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
                                "PoPMobile",
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
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)
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
                    StatusCard(
                        isRunning = isRunning,
                        poolState = poolState,
                        poolErrorReason = poolErrorReason,
                        isSessionActive = isSessionActive,
                        difficulty = difficulty
                    )

                    // Hashrate card (hero)
                    StatCard(
                        label = "HASHRATE",
                        value = formatHashrate(hashrate),
                        color = Color(0xFF49EACB)
                    )

                    // 3-column compact stats row
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        StatCardCompact(
                            label = "SHARES",
                            value = if (sharesRejected > 0) "$sharesFound/${sharesRejected}r" else sharesFound.toString(),
                            color = Color(0xFFFFD700),
                            modifier = Modifier.weight(1f)
                        )
                        StatCardCompact(
                            label = "HASHES",
                            value = formatHashes(totalHashes),
                            color = Color(0xFF8B5CF6),
                            modifier = Modifier.weight(1f)
                        )
                        StatCardCompact(
                            label = "THREADS",
                            value = if (isRunning) "$activeThreads/$threads" else "$threads",
                            color = Color(0xFFFF6B6B),
                            modifier = Modifier.weight(1f)
                        )
                    }

                    // 3-column device health row
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        StatCardCompact(
                            label = "TEMP",
                            value = if (cpuTemp > 0) "${cpuTemp.toInt()}°C" else "--",
                            color = when {
                                cpuTemp >= 55 -> Color(0xFFFF4444)
                                cpuTemp >= 50 -> Color(0xFFFF8C00)
                                cpuTemp >= 45 -> Color(0xFFFFD700)
                                else -> Color(0xFF49EACB)
                            },
                            modifier = Modifier.weight(1f)
                        )
                        StatCardCompact(
                            label = "BATTERY",
                            value = "$batteryPercent%",
                            color = when {
                                batteryPercent <= 10 -> Color(0xFFFF4444)
                                batteryPercent <= 20 -> Color(0xFFFFD700)
                                else -> Color(0xFF49EACB)
                            },
                            modifier = Modifier.weight(1f)
                        )
                        StatCardCompact(
                            label = "THERMAL",
                            value = thermalState.take(6),
                            color = when (thermalState) {
                                "CRITICAL" -> Color(0xFFFF4444)
                                "THROTTLE" -> Color(0xFFFF8C00)
                                "WARNING" -> Color(0xFFFFD700)
                                else -> Color(0xFF49EACB)
                            },
                            modifier = Modifier.weight(1f)
                        )
                    }

                    // Start/Stop button — shows STOP whenever a session is
                    // active, even if the pool isn't connected yet, so the
                    // user can cancel a stuck connection attempt.
                    val sessionLive = isSessionActive || isRunning
                    Button(
                        onClick = {
                            if (sessionLive) {
                                stopMiningService()
                            } else {
                                startMiningService(poolUrl, wallet, worker, threads)
                            }
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(52.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (sessionLive) Color(0xFFFF4444) else Color(0xFF49EACB)
                        ),
                        shape = RoundedCornerShape(12.dp),
                        enabled = wallet.isNotEmpty() && poolUrl.isNotEmpty()
                    ) {
                        Text(
                            if (sessionLive) "STOP MINING" else "START MINING",
                            fontSize = 16.sp,
                            fontWeight = FontWeight.Bold,
                            fontFamily = FontFamily.Monospace,
                            color = if (sessionLive) Color.White else Color.Black
                        )
                    }

                    // Disclaimer
                    Text(
                        "Lottery-style mining. Low hashrate expected.",
                        color = Color.Gray,
                        fontSize = 10.sp,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.padding(top = 2.dp).fillMaxWidth(),
                        textAlign = TextAlign.Center
                    )
                }
            }
        }
    }

    @Composable
    fun StatusCard(
        isRunning: Boolean,
        poolState: MiningService.PoolState,
        poolErrorReason: String?,
        isSessionActive: Boolean,
        difficulty: Double = 0.0
    ) {
        // Left-side status label
        val statusText = when {
            isRunning -> "MINING"
            poolState == MiningService.PoolState.CONNECTING -> "CONNECTING"
            poolState == MiningService.PoolState.CONNECTED -> "CONNECTED"
            poolState == MiningService.PoolState.ERROR && isSessionActive -> "RETRYING"
            else -> "IDLE"
        }
        val statusColor = when {
            isRunning -> Color(0xFF49EACB)
            poolState == MiningService.PoolState.CONNECTING -> Color(0xFFFFD700)
            poolState == MiningService.PoolState.CONNECTED -> Color(0xFFFFD700)
            poolState == MiningService.PoolState.ERROR -> Color(0xFFFFD700)  // yellow, not red
            else -> Color.Gray
        }
        // Right-side pool line
        val (poolLabel, poolColor) = when (poolState) {
            MiningService.PoolState.CONNECTED ->
                "POOL: ONLINE" to Color(0xFF49EACB)
            MiningService.PoolState.CONNECTING ->
                "POOL: CONNECTING..." to Color(0xFFFFD700)
            MiningService.PoolState.ERROR ->
                "POOL: ${(poolErrorReason ?: "ERROR").uppercase()}" to Color(0xFFFFD700)
            MiningService.PoolState.DISCONNECTED ->
                "POOL: OFFLINE" to Color(0xFFFF4444)
        }

        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(12.dp)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 14.dp, vertical = 10.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("STATUS", color = Color.Gray, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                    Text(
                        statusText,
                        color = statusColor,
                        fontSize = 18.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )
                }
                Column(horizontalAlignment = Alignment.End) {
                    Text(
                        poolLabel,
                        color = poolColor,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace
                    )
                    if (difficulty > 0) {
                        Text(
                            "DIFF: ${"%.4f".format(difficulty)}",
                            color = Color(0xFF8B5CF6),
                            fontSize = 10.sp,
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
            Column(modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp)) {
                Text(label, color = Color.Gray, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                Spacer(modifier = Modifier.height(2.dp))
                Text(
                    value,
                    color = color,
                    fontSize = 24.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }

    /** Compact stat card for 3-column rows on small screens. */
    @Composable
    fun StatCardCompact(
        label: String,
        value: String,
        color: Color,
        modifier: Modifier = Modifier
    ) {
        Card(
            modifier = modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFF1A1A2E)),
            shape = RoundedCornerShape(10.dp)
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(label, color = Color.Gray, fontSize = 10.sp, fontFamily = FontFamily.Monospace)
                Spacer(modifier = Modifier.height(2.dp))
                Text(
                    value,
                    color = color,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    maxLines = 1
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

        // PoPManager settings (read/write directly against the popmanager prefs file
        // so the reporter in MiningService sees changes immediately)
        val popPrefs = getSharedPreferences("popmanager", Context.MODE_PRIVATE)
        val defaultName = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}"
        var popServerUrl by remember {
            mutableStateOf(popPrefs.getString("server_url", "") ?: "")
        }
        var popDeviceName by remember {
            mutableStateOf(popPrefs.getString("device_name", defaultName) ?: defaultName)
        }
        var popTestResult by remember { mutableStateOf<String?>(null) }
        var popTesting by remember { mutableStateOf(false) }
        var popPairingCode by remember { mutableStateOf("") }
        var popPairResult by remember { mutableStateOf<String?>(null) }
        var popPairing by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()

        // Live pairing status from the reporter (polled once per second via
        // the same LaunchedEffect that updates mining stats). We read directly
        // each recomposition because the flags are @Volatile on the reporter.
        val popPairingRequired = miningService?.popManagerReporter?.pairingRequired ?: false
        val popLastStatus = miningService?.popManagerReporter?.lastStatus ?: "idle"
        val popLastError = miningService?.popManagerReporter?.lastError

        Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {

            // ========= MINER CONFIGURATION =========
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

                    // Wallet address with a QR scan button
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Box(modifier = Modifier.weight(1f)) {
                            MinerTextField("Wallet Address", wallet, onWalletChange)
                        }
                        OutlinedButton(
                            onClick = { launchWalletQrScanner(onWalletChange) },
                            modifier = Modifier.height(56.dp)
                        ) {
                            Text(
                                "Scan QR",
                                color = Color(0xFF49EACB),
                                fontSize = 12.sp,
                                fontFamily = FontFamily.Monospace
                            )
                        }
                    }

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
                }
            }

            // ========= POPMANAGER INTEGRATION =========
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
                        "POPMANAGER INTEGRATION",
                        color = Color(0xFF49EACB),
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )
                    Text(
                        "Report this miner to a PoPManager instance on your network.",
                        color = Color.Gray,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace
                    )

                    // --- Pairing status banner ---
                    val hasStoredKey = popPrefs.getString("api_key", null) != null
                    val (bannerText, bannerColor) = when {
                        popServerUrl.isBlank() ->
                            "Not configured" to Color.Gray
                        popPairingRequired || !hasStoredKey ->
                            "Pairing required — enter code from PoPManager" to Color(0xFFFFD700)
                        popLastStatus == "reporting" ->
                            "Paired · reporting" to Color(0xFF49EACB)
                        popLastStatus == "error" ->
                            "Error: ${popLastError ?: "unknown"}" to Color(0xFFFF6B6B)
                        else ->
                            "Status: $popLastStatus" to Color.Gray
                    }
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(Color(0xFF0F0F23), shape = RoundedCornerShape(6.dp))
                            .padding(horizontal = 12.dp, vertical = 8.dp)
                    ) {
                        Text(
                            bannerText,
                            color = bannerColor,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            fontFamily = FontFamily.Monospace
                        )
                    }

                    MinerTextField(
                        "Server URL (http://host:8787)",
                        popServerUrl
                    ) { popServerUrl = it; popTestResult = null }

                    MinerTextField("Device Name", popDeviceName) { popDeviceName = it }

                    // --- Pairing code row (only shown when pairing is required
                    //     OR user wants to re-pair) ---
                    val showPairingInput = popPairingRequired || !hasStoredKey ||
                        popPairingCode.isNotEmpty()
                    if (showPairingInput) {
                        OutlinedTextField(
                            value = popPairingCode,
                            onValueChange = { raw ->
                                // Strip non-digit, cap at 6
                                popPairingCode = raw.filter { it.isDigit() }.take(6)
                                popPairResult = null
                            },
                            label = { Text("Pairing Code (6 digits)", color = Color.Gray) },
                            placeholder = { Text("• • • • • •", color = Color.Gray) },
                            textStyle = androidx.compose.ui.text.TextStyle(
                                color = Color.White,
                                fontFamily = FontFamily.Monospace,
                                fontSize = 18.sp,
                                letterSpacing = 4.sp
                            ),
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                            colors = OutlinedTextFieldDefaults.colors(
                                focusedBorderColor = Color(0xFF49EACB),
                                unfocusedBorderColor = Color.Gray
                            ),
                            modifier = Modifier.fillMaxWidth()
                        )
                        Text(
                            "Get the current code from PoPManager → Mobile Miners → Add Device.",
                            color = Color.Gray,
                            fontSize = 10.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }

                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        OutlinedButton(
                            onClick = {
                                if (popServerUrl.isBlank()) return@OutlinedButton
                                popTesting = true
                                popTestResult = null
                                scope.launch {
                                    val reporter = com.proofofprints.kasminer.popmanager
                                        .PoPManagerReporter(this@MainActivity, object :
                                            com.proofofprints.kasminer.popmanager.PoPManagerReporter.StatsProvider {
                                            override fun snapshot() = com.proofofprints.kasminer.popmanager
                                                .PoPManagerReporter.TelemetrySnapshot(
                                                "KAS", "", "", 0.0, 0, 0, 0.0, 0L,
                                                0f, "normal", 0, false, 0, "stopped"
                                            )
                                        })
                                    val result = reporter.testConnection(popServerUrl)
                                    popTestResult = result.fold(
                                        onSuccess = { "OK: $it" },
                                        onFailure = { "Failed: ${it.message}" }
                                    )
                                    popTesting = false
                                }
                            },
                            modifier = Modifier.weight(1f),
                            enabled = !popTesting && popServerUrl.isNotBlank()
                        ) {
                            Text(
                                if (popTesting) "Testing..." else "Test Connection",
                                color = Color(0xFF49EACB),
                                fontSize = 12.sp,
                                fontFamily = FontFamily.Monospace
                            )
                        }

                        // Pair button — enabled once a full 6-digit code is entered
                        Button(
                            onClick = {
                                val svc = miningService ?: run {
                                    popPairResult = "Service not bound yet — try again"
                                    return@Button
                                }
                                // Make sure the reporter has the current URL before pairing
                                svc.popManagerReporter.serverUrl = popServerUrl.trimEnd('/')
                                svc.popManagerReporter.deviceName = popDeviceName
                                popPairing = true
                                popPairResult = null
                                scope.launch {
                                    val result = svc.popManagerReporter.pairWithCode(popPairingCode)
                                    popPairResult = result.fold(
                                        onSuccess = { "Paired successfully" },
                                        onFailure = { "Failed: ${it.message}" }
                                    )
                                    if (result.isSuccess) {
                                        popPairingCode = ""
                                        // Persist the server URL + name alongside
                                        popPrefs.edit()
                                            .putString("server_url", popServerUrl.trimEnd('/'))
                                            .putString("device_name", popDeviceName)
                                            .apply()
                                    }
                                    popPairing = false
                                }
                            },
                            modifier = Modifier.weight(1f),
                            enabled = !popPairing &&
                                popPairingCode.length == 6 &&
                                popServerUrl.isNotBlank(),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = Color(0xFF49EACB),
                                disabledContainerColor = Color(0xFF2A2A3E)
                            )
                        ) {
                            Text(
                                if (popPairing) "Pairing..." else "Pair",
                                color = if (popPairing || popPairingCode.length != 6)
                                    Color.Gray else Color.Black,
                                fontSize = 12.sp,
                                fontWeight = FontWeight.Bold,
                                fontFamily = FontFamily.Monospace
                            )
                        }
                    }

                    // Re-pair link (when already paired, lets the user force a fresh pair)
                    if (hasStoredKey && !popPairingRequired) {
                        TextButton(
                            onClick = {
                                miningService?.popManagerReporter?.clearCredentials()
                                // Force the reporter into pairing_required state
                                miningService?.popManagerReporter?.stop()
                                miningService?.popManagerReporter?.start()
                                popPairResult = "Credentials cleared — enter new pairing code"
                            },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(
                                "Forget this device and re-pair",
                                color = Color(0xFFFFD700),
                                fontSize = 11.sp,
                                fontFamily = FontFamily.Monospace
                            )
                        }
                    }

                    popTestResult?.let {
                        Text(
                            it,
                            color = if (it.startsWith("OK")) Color(0xFF49EACB) else Color(0xFFFF6B6B),
                            fontSize = 12.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                    popPairResult?.let {
                        Text(
                            it,
                            color = if (it.startsWith("Paired")) Color(0xFF49EACB) else Color(0xFFFF6B6B),
                            fontSize = 12.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }
            }

            // ========= SAVED NOTICE (above the button so it's visible) =========
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

            // ========= SAVE BUTTON =========
            Button(
                onClick = {
                    onSave()
                    // Persist PoPManager settings directly to its prefs file
                    popPrefs.edit()
                        .putString("server_url", popServerUrl.trimEnd('/'))
                        .putString("device_name", popDeviceName)
                        .apply()
                    // Restart reporter so it picks up the new server URL /
                    // device name. We do NOT clear credentials here — if the
                    // user pointed at a different server they should use the
                    // "Forget this device and re-pair" link explicitly.
                    miningService?.let { svc ->
                        svc.popManagerReporter.stop()
                        svc.popManagerReporter.start()
                    }
                    // If PoPManager is configured and service isn't running,
                    // kick it so the reporter begins immediately.
                    if (popServerUrl.isNotBlank()) {
                        val intent = Intent(this@MainActivity, MiningService::class.java).apply {
                            action = MiningService.ACTION_ENSURE_RUNNING
                        }
                        ContextCompat.startForegroundService(this@MainActivity, intent)
                    }
                    showSaved = true
                },
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF49EACB)),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("SAVE", color = Color.Black, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace)
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

    /** Launch the ZXing QR scanner to capture a Kaspa wallet address. ZXing
     *  is bundled directly in the APK, so this works on first launch with no
     *  Play Services download. The camera permission is requested by the
     *  ZXing activity itself if not yet granted. */
    private fun launchWalletQrScanner(onResult: (String) -> Unit) {
        pendingScanResult = onResult
        val options = com.journeyapps.barcodescanner.ScanOptions().apply {
            setDesiredBarcodeFormats(com.journeyapps.barcodescanner.ScanOptions.QR_CODE)
            setBeepEnabled(false)
            setOrientationLocked(true)
            setBarcodeImageEnabled(false)
            // Use our custom portrait scanner with square framing box
            captureActivity = QrScannerActivity::class.java
        }
        qrScanLauncher.launch(options)
    }

    /** Parse and validate a scanned QR string, then hand the extracted
     *  Kaspa address to [onResult]. Handles bare addresses, URI-wrapped forms,
     *  and addresses with query params. */
    private fun handleScannedWalletAddress(raw: String, onResult: (String) -> Unit) {
        val trimmed = raw.trim()
        if (trimmed.isEmpty()) {
            android.widget.Toast.makeText(
                this, "QR code was empty", android.widget.Toast.LENGTH_SHORT
            ).show()
            return
        }

        // Some wallets encode the address as a URI: "kaspa:kaspa:qyp..." or
        // with query params: "kaspa:qyp...?amount=1.0". Extract the address
        // portion only.
        var address = trimmed
        if (address.startsWith("kaspa:kaspa:", ignoreCase = true)) {
            address = address.removePrefix("kaspa:")  // leaves "kaspa:qyp..."
        }
        address = address.substringBefore('?').substringBefore('&').trim()

        if (!address.startsWith("kaspa:", ignoreCase = true)) {
            android.widget.Toast.makeText(
                this, "Not a Kaspa address: $address",
                android.widget.Toast.LENGTH_LONG
            ).show()
            return
        }

        onResult(address)
        LogManager.info("Scanned wallet address: ${address.take(20)}...")
        android.widget.Toast.makeText(
            this, "Wallet scanned", android.widget.Toast.LENGTH_SHORT
        ).show()
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
                    "PoPMobile",
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
