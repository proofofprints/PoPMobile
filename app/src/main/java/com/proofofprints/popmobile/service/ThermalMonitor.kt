/**
 * Thermal and battery monitoring for safe mobile mining.
 *
 * Reads CPU thermal zones and battery state to automatically
 * throttle or pause mining when the device gets too hot or
 * battery drops too low.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.service

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import android.util.Log
import java.io.File

class ThermalMonitor(
    private val context: Context,
    private val prefs: MiningPreferences = MiningPreferences(context)
) {

    companion object {
        private const val TAG = "ThermalMonitor"
        @Suppress("DEPRECATION")
        private fun readSocModel(): String =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Build.SOC_MODEL
            else Build.HARDWARE
    }

    enum class ThermalState {
        NORMAL,     // Full speed mining
        WARNING,    // Getting warm — inform user
        THROTTLE,   // Too hot — reduce thread count
        CRITICAL    // Emergency — pause mining
    }

    data class DeviceStatus(
        val cpuTemp: Float,           // Celsius (0 if no CPU sensor was readable)
        val batteryPercent: Int,      // 0-100
        val isCharging: Boolean,
        val thermalState: ThermalState,
        val recommendedThreads: Int,  // Based on thermal state
        /** True when at least one usable thermal signal is available — either
         *  a sysfs zone read (cpuTemp > 0) or a PowerManager.getThermalHeadroom
         *  value (Android 11+). When false, thermal protection cannot
         *  meaningfully act on this device and the UI should say so. */
        val thermalSensorAvailable: Boolean
    )

    /** A single thermal_zone entry as exposed to the diagnostics UI. */
    data class ZoneReading(
        val root: String,        // "/sys/class/thermal/" or "/sys/devices/virtual/thermal/"
        val name: String,        // e.g. "thermal_zone19"
        val type: String,        // e.g. "cpu-1-2-usr"
        val tempC: Float?,       // parsed Celsius, or null if unreadable / out of range
        val rawTemp: String,     // raw file content (or "unreadable" / "err:...")
        val isPicked: Boolean,   // true if this is the zone our heuristic chose
        val isCpuHinted: Boolean // matches cpuZoneHints — would be preferred if CPU
    )

    /** Snapshot for the in-app Thermal Diagnostics screen. Lets users on
     *  unsupported SoCs paste a copy/paste-able report into a bug ticket so
     *  we can grow the heuristic from real-device data. */
    data class ThermalDiagnostics(
        val zones: List<ZoneReading>,
        val pickedTempC: Float,                       // 0 if no zone matched
        val headroom: Float?,                         // null if API unavailable
        val headroomNote: String,                     // human-readable status
        val osThermalStatus: String,                  // currentThermalStatus name or "unsupported"
        val androidApi: Int = Build.VERSION.SDK_INT,
        val socModel: String = readSocModel(),
        val manufacturer: String = Build.MANUFACTURER,
        val deviceModel: String = Build.MODEL
    )

    private var maxThreads: Int = Runtime.getRuntime().availableProcessors()
    private var dumpedZones: Boolean = false

    /**
     * Read current device thermal and battery status.
     */
    fun getStatus(currentThreads: Int): DeviceStatus {
        maxThreads = currentThreads
        if (!dumpedZones) {
            dumpAllThermalZones()
            dumpedZones = true
        }

        // Single ACTION_BATTERY_CHANGED read covers percent-fallback and
        // charging state. We deliberately do NOT use battery temperature as a
        // CPU-temp fallback — the battery sits adjacent to the SoC but lags
        // 15–20 °C under load, so acting on it as if it were CPU temp would
        // mis-fire protection. If sysfs is locked down we rely on the
        // PowerManager headroom signal instead, and if that's also missing we
        // surface "thermal sensor unavailable" rather than guessing.
        val batteryIntent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val batteryPercent = readBatteryPercent(batteryIntent)
        val isCharging = readIsCharging(batteryIntent)

        val cpuTemp = readCpuTemperature()
        val headroom = readThermalHeadroom()
        val thermalSensorAvailable = cpuTemp > 0f ||
            (headroom != null && !headroom.isNaN())

        // If the user has disabled thermal protection outright, never escalate
        // past NORMAL — the UI still shows the real temp (so they can monitor)
        // but MiningService won't act on it.
        val thermalState = if (!prefs.thermalProtectionEnabled) {
            ThermalState.NORMAL
        } else {
            computeThermalState(cpuTemp, batteryPercent, isCharging, headroom)
        }

        val recommendedThreads = when (thermalState) {
            ThermalState.CRITICAL -> 0  // Pause
            ThermalState.THROTTLE -> maxOf(1, currentThreads / 2)
            ThermalState.WARNING -> maxOf(1, currentThreads - 1)
            ThermalState.NORMAL -> currentThreads
        }

        return DeviceStatus(
            cpuTemp = cpuTemp,
            batteryPercent = batteryPercent,
            isCharging = isCharging,
            thermalState = thermalState,
            recommendedThreads = recommendedThreads,
            thermalSensorAvailable = thermalSensorAvailable
        )
    }

    /** Combine four signals into a single thermal severity:
     *
     *  1. User-configurable °C thresholds (pause/throttle/warn) against the
     *     sysfs CPU reading. The user's thresholds ALWAYS win when they
     *     escalate higher than the OS-derived bucket — vendor thresholds are
     *     tuned for casual workloads and won't fire until the SoC is
     *     dangerously hot, so a user who sets pause at 70 °C should see mining
     *     pause at 70 °C regardless of what PowerManager says.
     *  2. PowerManager.getCurrentThermalStatus() bucket (API 29+).
     *  3. PowerManager.getThermalHeadroom() (API 30+) — a continuous,
     *     OEM-calibrated normalized [0, 1+] signal that works even when sysfs
     *     is SELinux-locked. The single highest-impact fallback for retail OEM
     *     ROMs that hide thermal_zone* from unprivileged apps.
     *  4. Battery cutoff (low-battery as critical, also API-independent).
     *
     *  We pick the maximum severity across (1)+(2)+(3); (4) is folded in
     *  separately because it's about power, not heat. Battery is ignored
     *  entirely in external-power mode. */
    private fun computeThermalState(
        displayTemp: Float,
        batteryPercent: Int,
        isCharging: Boolean,
        headroom: Float?
    ): ThermalState {
        val osState = readOsThermalState() ?: ThermalState.NORMAL
        val headroomState = headroom?.let { headroomToState(it) } ?: ThermalState.NORMAL
        val thresholdState = if (displayTemp > 0f) when {
            displayTemp >= prefs.pauseTempC -> ThermalState.CRITICAL
            displayTemp >= prefs.throttleTempC -> ThermalState.THROTTLE
            displayTemp >= prefs.warnTempC -> ThermalState.WARNING
            else -> ThermalState.NORMAL
        } else ThermalState.NORMAL  // no sysfs reading — leave to OS / headroom

        // ThermalState ordinal is severity — pick whichever source is most
        // conservative across all three thermal signals.
        val tempState = listOf(thresholdState, osState, headroomState)
            .maxByOrNull { it.ordinal } ?: ThermalState.NORMAL

        if (prefs.externalPowerMode) return tempState
        return combineWithBattery(tempState, batteryPercent, isCharging)
    }

    /** Map a PowerManager.getThermalHeadroom value to our internal bucket.
     *  Headroom is normalized: 1.0 == at the OEM's throttle threshold,
     *  > 1.0 == the device is actively throttling. Thresholds are deliberately
     *  conservative — we'd rather catch a near-throttle moment a tick early
     *  than miss it. */
    private fun headroomToState(h: Float): ThermalState {
        if (h.isNaN()) return ThermalState.NORMAL
        return when {
            h >= 1.0f -> ThermalState.CRITICAL
            h >= 0.85f -> ThermalState.THROTTLE
            h >= 0.70f -> ThermalState.WARNING
            else -> ThermalState.NORMAL
        }
    }

    /** Read PowerManager.getThermalHeadroom(0) — the OEM's normalized
     *  current-vs-throttle-threshold value. Available only on Android 11+
     *  (API 30); pre-R devices return null and we fall back to whatever sysfs
     *  / OS-bucket signals are available. The call can also throw on some OEM
     *  ROMs that don't ship a thermal HAL — caught and treated as
     *  "unavailable". */
    private fun readThermalHeadroom(): Float? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
        return try {
            val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
            val h = pm.getThermalHeadroom(0)
            if (h.isNaN()) null else h
        } catch (e: Exception) {
            Log.d(TAG, "PowerManager headroom API unavailable: ${e.message}")
            null
        }
    }

    /**
     * Ask the OS's PowerManager for its current thermal-status bucket and
     * map it directly onto our ThermalState enum. Returns null on API<29 or
     * when the call throws (happens on some OEM AOSP forks).
     */
    private fun readOsThermalState(): ThermalState? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        return try {
            val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
            when (pm.currentThermalStatus) {
                PowerManager.THERMAL_STATUS_NONE,
                PowerManager.THERMAL_STATUS_LIGHT -> ThermalState.NORMAL
                PowerManager.THERMAL_STATUS_MODERATE -> ThermalState.WARNING
                PowerManager.THERMAL_STATUS_SEVERE -> ThermalState.THROTTLE
                PowerManager.THERMAL_STATUS_CRITICAL,
                PowerManager.THERMAL_STATUS_EMERGENCY,
                PowerManager.THERMAL_STATUS_SHUTDOWN -> ThermalState.CRITICAL
                else -> ThermalState.NORMAL
            }
        } catch (e: Exception) {
            Log.d(TAG, "PowerManager thermal API unavailable: ${e.message}")
            null
        }
    }

    /** Roll battery-level concerns into the OS thermal decision — low
     *  battery outranks a NORMAL thermal state but not a CRITICAL one.
     *  The cutoff is user-configurable; WARNING fires at 2× the cutoff so
     *  the user sees a heads-up before we pause.
     *
     *  We deliberately ignore `isCharging` here. A slow USB charger (~500 mA)
     *  can report STATUS_CHARGING while the device still loses charge under
     *  mining load — a previous version short-circuited on isCharging and
     *  let battery slide past the cutoff. Always evaluate the cutoff; the
     *  resume path will pick mining back up once the charger catches up.
     *  Users who want mining to ignore battery entirely should enable
     *  externalPowerMode (handled one frame up the stack). */
    private fun combineWithBattery(
        os: ThermalState,
        batteryPercent: Int,
        @Suppress("UNUSED_PARAMETER") isCharging: Boolean
    ): ThermalState {
        if (batteryPercent <= 0) return os
        val cutoff = prefs.batteryCutoffPercent
        val warnLevel = (cutoff * 2).coerceAtMost(50)
        val batteryState = when {
            batteryPercent <= cutoff -> ThermalState.CRITICAL
            batteryPercent <= warnLevel -> ThermalState.WARNING
            else -> ThermalState.NORMAL
        }
        // ThermalState enum is declared in severity order, ordinal == severity.
        return if (batteryState.ordinal > os.ordinal) batteryState else os
    }

    /**
     * Read CPU temperature from /sys/class/thermal/thermal_zone*. Returns
     * temperature in Celsius, or 0 if no readable zone exists.
     * (The PowerManager path is handled separately above for the thermal
     * STATE — raw temp only gets displayed when this or battery has a value.)
     */
    private fun readCpuTemperature(): Float = readThermalZones()

    /**
     * Snapshot every thermal zone the OS exposes plus the headroom signal,
     * for the in-app Thermal Diagnostics screen. Heavier than getStatus() —
     * meant to be called only when the user opens Settings → Diagnostics, not
     * on every poller tick. Marks which zone our heuristic would pick so
     * users can see whether we got it right.
     */
    fun getDiagnostics(): ThermalDiagnostics {
        val zones = mutableListOf<ZoneReading>()
        var pickedTempC = 0.0f
        var pickedKey = ""

        for (root in thermalZoneRoots) {
            val thermalDir = try { File(root) } catch (_: Exception) { continue }
            if (!thermalDir.exists()) continue
            val entries = try {
                thermalDir.listFiles()?.filter { it.name.startsWith("thermal_zone") }?.sortedBy { it.name }
            } catch (_: Exception) { null } ?: continue

            for (zone in entries) {
                val type = try {
                    File(zone, "type").takeIf { it.exists() && it.canRead() }?.readText()?.trim() ?: ""
                } catch (e: Exception) { "err:${e.message}" }
                val rawTemp = try {
                    val f = File(zone, "temp")
                    if (f.exists() && f.canRead()) f.readText().trim() else "unreadable"
                } catch (e: Exception) { "err:${e.message}" }

                val parsed = rawTemp.toFloatOrNull()
                val tempC: Float? = if (parsed != null) {
                    val c = if (parsed > 1000) parsed / 1000.0f else parsed
                    if (c in -20f..125f) c else null
                } else null

                val isCpuHinted = typeLooksLikeCpu(type)
                val isUsable = tempC != null &&
                    !type.contains("step", ignoreCase = true) &&
                    !type.contains("trip", ignoreCase = true) &&
                    !type.equals("soc", ignoreCase = true)

                // Mirror readThermalZones()'s pick logic so the diagnostic UI
                // marks the zone that protection actually acts on.
                if (isUsable && tempC != null) {
                    if (isCpuHinted) {
                        if (tempC > pickedTempC) {
                            pickedTempC = tempC
                            pickedKey = "$root${zone.name}"
                        }
                    } else if (pickedTempC == 0.0f && tempC > 0f) {
                        pickedTempC = tempC
                        pickedKey = "$root${zone.name}"
                    }
                }

                zones += ZoneReading(
                    root = root,
                    name = zone.name,
                    type = type,
                    tempC = tempC,
                    rawTemp = rawTemp,
                    isPicked = false,        // patched below once pickedKey is final
                    isCpuHinted = isCpuHinted
                )
            }
        }

        val pickedZones = zones.map {
            it.copy(isPicked = ("${it.root}${it.name}") == pickedKey)
        }

        val headroom = readThermalHeadroom()
        val headroomNote = when {
            headroom != null -> "OK"
            Build.VERSION.SDK_INT < Build.VERSION_CODES.R ->
                "Requires Android 11+ (this device: API ${Build.VERSION.SDK_INT})"
            else -> "Unavailable on this device (no thermal HAL)"
        }

        val osBucket = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            try {
                val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
                when (pm.currentThermalStatus) {
                    PowerManager.THERMAL_STATUS_NONE -> "NONE"
                    PowerManager.THERMAL_STATUS_LIGHT -> "LIGHT"
                    PowerManager.THERMAL_STATUS_MODERATE -> "MODERATE"
                    PowerManager.THERMAL_STATUS_SEVERE -> "SEVERE"
                    PowerManager.THERMAL_STATUS_CRITICAL -> "CRITICAL"
                    PowerManager.THERMAL_STATUS_EMERGENCY -> "EMERGENCY"
                    PowerManager.THERMAL_STATUS_SHUTDOWN -> "SHUTDOWN"
                    else -> "unknown"
                }
            } catch (_: Exception) { "error" }
        } else "Requires Android 10+"

        return ThermalDiagnostics(
            zones = pickedZones,
            pickedTempC = pickedTempC,
            headroom = headroom,
            headroomNote = headroomNote,
            osThermalStatus = osBucket
        )
    }

    /** One-shot dump of every thermal zone the OS exposes (under both
     *  /sys/class/thermal/ and /sys/devices/virtual/thermal/, since some OEM
     *  ROMs only expose one). Run once per session so we can see in logcat
     *  which zones exist and which are readable by the unprivileged app UID
     *  — vendors lock different zones on different ROMs.
     *
     *  Also logs the PowerManager thermal-headroom value when available
     *  (Android 11+) so we can see at-a-glance whether the OEM-calibrated
     *  fallback signal is working on this device. */
    private fun dumpAllThermalZones() {
        for (root in thermalZoneRoots) {
            dumpZonesUnder(root)
        }
        val headroom = readThermalHeadroom()
        if (headroom != null) {
            Log.i(TAG, "ZONE_DUMP PowerManager.getThermalHeadroom(0)=$headroom")
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Log.i(TAG, "ZONE_DUMP PowerManager.getThermalHeadroom unavailable on this device (no thermal HAL)")
        } else {
            Log.i(TAG, "ZONE_DUMP PowerManager.getThermalHeadroom requires Android 11+ (this device: API ${Build.VERSION.SDK_INT})")
        }
    }

    private fun dumpZonesUnder(root: String) {
        try {
            val thermalDir = File(root)
            if (!thermalDir.exists()) {
                Log.i(TAG, "ZONE_DUMP $root not present")
                return
            }
            val zones = thermalDir.listFiles()?.filter { it.name.startsWith("thermal_zone") }?.sortedBy { it.name }
            if (zones.isNullOrEmpty()) {
                Log.w(TAG, "ZONE_DUMP $root has no thermal_zone* entries")
                return
            }
            Log.i(TAG, "ZONE_DUMP $root has ${zones.size} thermal zones")
            for (zone in zones) {
                val type = try {
                    File(zone, "type").takeIf { it.exists() && it.canRead() }?.readText()?.trim() ?: "?"
                } catch (e: Exception) { "err:${e.message}" }
                val temp = try {
                    val f = File(zone, "temp")
                    if (f.exists() && f.canRead()) f.readText().trim() else "unreadable"
                } catch (e: Exception) { "err:${e.message}" }
                Log.i(TAG, "ZONE_DUMP $root${zone.name} type=$type temp=$temp")
            }
        } catch (e: Exception) {
            Log.w(TAG, "ZONE_DUMP failed: ${e.message}")
        }
    }

    /**
     * Type-string fragments that across vendor kernels reliably identify a CPU
     * / SoC thermal zone (case-insensitive `contains` match).
     *
     * Sources confirmed:
     *  - Snapdragon legacy (msm-3.18 tsens binding) and current (cpuss / apc /
     *    soc_thermal naming).
     *  - MediaTek mtk_ts_cpu.c kernel driver.
     *  - Exynos exynos_tmu driver and big.LITTLE.tri device-tree bindings.
     *  - Google Tensor virtual-skin zone names from public dumps.
     * Kirin / Unisoc are partial — they fall through to the
     * "highest non-trip non-soc zone" fallback if no hint matches.
     */
    private val cpuZoneHints = listOf(
        // Snapdragon
        "cpu", "tsens", "apc", "cpuss", "soc_thermal",
        // MediaTek
        "mtktscpu", "mtktsap",
        // Exynos / Kirin (cluster0_thermal / cluster1_thermal / etc.)
        "cluster",
        // Exynos big.LITTLE.tri
        "big", "mid", "little",
        // Google Tensor
        "virtual-skin-cpu", "tpu"
    )

    private fun typeLooksLikeCpu(type: String): Boolean =
        cpuZoneHints.any { type.contains(it, ignoreCase = true) }

    /** Both root paths under which Android exposes thermal zones. Most devices
     *  symlink one to the other, but some OEM ROMs (notably Samsung / Xiaomi
     *  builds) only expose one of the two — probing both costs nothing and
     *  recovers a zone we'd otherwise miss. */
    private val thermalZoneRoots = listOf(
        "/sys/class/thermal/",
        "/sys/devices/virtual/thermal/"
    )

    /**
     * Read from `/sys/class/thermal/thermal_zone*` (and the equivalent
     * `/sys/devices/virtual/thermal/...` path) as the raw-temperature source.
     * Many OEMs restrict these on retail Android via SELinux; in that case
     * this returns 0 and the caller falls back to other signals
     * (PowerManager headroom / OS thermal-status bucket).
     *
     * On Qualcomm Bengal (TCL A3X) several zones named `*-step` / `*-max-step`
     * report the trip-point threshold (e.g. 100000 mC = 100 °C) rather than a
     * live temperature — so we skip anything whose type looks like a trip
     * reporter, and log which zone contributed the winning reading so future
     * mis-calibration is diagnosable from logcat.
     */
    private fun readThermalZones(): Float {
        var maxTemp = 0.0f
        var maxZone = ""
        var maxType = ""
        var maxRoot = ""

        for (root in thermalZoneRoots) {
            try {
                val thermalDir = File(root)
                if (!thermalDir.exists()) continue

                thermalDir.listFiles()?.filter { it.name.startsWith("thermal_zone") }?.forEach { zone ->
                    try {
                        val typeFile = File(zone, "type")
                        val tempFile = File(zone, "temp")

                        if (tempFile.exists() && tempFile.canRead()) {
                            val type = if (typeFile.exists()) typeFile.readText().trim() else ""
                            val rawTemp = tempFile.readText().trim().toFloatOrNull() ?: return@forEach

                            // Skip zones that report trip-point thresholds, not
                            // live temperatures. On Bengal these sit at a fixed
                            // 95–100 °C and dominate the max when the device is
                            // cold.
                            if (type.contains("step", ignoreCase = true) ||
                                type.contains("trip", ignoreCase = true)) {
                                return@forEach
                            }

                            // On Qualcomm PMIC the zone literally named "soc"
                            // is State of Charge (battery %), not System-on-Chip
                            // temperature. Ignore it.
                            if (type.equals("soc", ignoreCase = true)) {
                                return@forEach
                            }

                            // Most zones report in millidegrees
                            val tempC = if (rawTemp > 1000) rawTemp / 1000.0f else rawTemp

                            // Sanity range: phones don't operate outside -20..125 °C,
                            // anything else is a sensor reporting something that
                            // isn't temperature (state-of-charge, voltage, trip
                            // threshold, disabled sensor stuck at -40000 or 0).
                            if (tempC < -20f || tempC > 125f) return@forEach

                            // Prefer CPU-related zones; fall back to highest
                            // readable non-trip non-soc zone if no hinted zone
                            // exists (Kirin / Unisoc / unknown SoCs).
                            if (typeLooksLikeCpu(type)) {
                                if (tempC > maxTemp) {
                                    maxTemp = tempC
                                    maxZone = zone.name
                                    maxType = type
                                    maxRoot = root
                                }
                            } else if (maxTemp == 0.0f && tempC > 0) {
                                maxTemp = tempC
                                maxZone = zone.name
                                maxType = type
                                maxRoot = root
                            }
                        }
                    } catch (e: Exception) {
                        // Skip unreadable zones
                    }
                }
            } catch (e: Exception) {
                Log.d(TAG, "Cannot read thermal zones under $root: ${e.message}")
            }
        }

        if (maxTemp > 0f) {
            Log.i(TAG, "thermal max=$maxTemp°C from $maxRoot$maxZone ($maxType)")
        }
        return maxTemp
    }

    /**
     * Get battery percentage (0-100). Tries BatteryManager.BATTERY_PROPERTY_CAPACITY
     * first, which is API 21+ and should work everywhere, but some OEMs return
     * -1 or 0 in practice. Falls back to the EXTRA_LEVEL / EXTRA_SCALE pair from
     * ACTION_BATTERY_CHANGED, which is more universally supported.
     */
    private fun readBatteryPercent(batteryIntent: Intent?): Int {
        val bm = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
        val capacity = bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
        if (capacity in 0..100) return capacity

        // Fallback — ACTION_BATTERY_CHANGED extras.
        val level = batteryIntent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = batteryIntent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        return if (level >= 0 && scale > 0) {
            (level * 100 / scale).coerceIn(0, 100)
        } else {
            0
        }
    }

    /** Check if the device is currently charging. */
    private fun readIsCharging(batteryIntent: Intent?): Boolean {
        val status = batteryIntent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        return status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL
    }

    /**
     * Battery temperature from ACTION_BATTERY_CHANGED. The battery sits
     * adjacent to the SoC on essentially every phone, so its temperature
     * is a reasonable proxy for "how hot is the device" when the OEM
     * locks down the CPU thermal zones.
     *
     * EXTRA_TEMPERATURE is documented as tenths of a degree Celsius.
     * Returns 0 if unavailable.
     */
    private fun readBatteryTemperature(batteryIntent: Intent?): Float {
        val tenths = batteryIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1) ?: -1
        return if (tenths > 0) tenths / 10.0f else 0f
    }
}
