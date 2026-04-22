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

class ThermalMonitor(private val context: Context) {

    companion object {
        private const val TAG = "ThermalMonitor"

        /** Temperature thresholds in Celsius — used only when we have a real
         *  raw sensor reading. When the system exposes a bucketed thermal
         *  status via PowerManager we use that directly instead. */
        const val TEMP_WARNING = 45.0f
        const val TEMP_THROTTLE = 50.0f   // Reduce threads at this temp
        const val TEMP_CRITICAL = 55.0f   // Pause mining entirely

        /** Battery thresholds */
        const val BATTERY_LOW = 20        // Warn user
        const val BATTERY_CRITICAL = 10   // Pause mining
    }

    enum class ThermalState {
        NORMAL,     // Full speed mining
        WARNING,    // Getting warm — inform user
        THROTTLE,   // Too hot — reduce thread count
        CRITICAL    // Emergency — pause mining
    }

    data class DeviceStatus(
        val cpuTemp: Float,           // Celsius (0 if unavailable; may be battery temp if CPU sensors locked down)
        val batteryPercent: Int,      // 0-100
        val isCharging: Boolean,
        val thermalState: ThermalState,
        val recommendedThreads: Int   // Based on thermal state
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

        // Single ACTION_BATTERY_CHANGED read covers percent-fallback,
        // charging state, and battery temperature.
        val batteryIntent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val batteryPercent = readBatteryPercent(batteryIntent)
        val isCharging = readIsCharging(batteryIntent)
        val batteryTemp = readBatteryTemperature(batteryIntent)

        val cpuTemp = readCpuTemperature()
        // Display temp: CPU if we have it, otherwise fall back to battery
        // temperature. Battery sits right next to the SoC so it's a decent
        // proxy when thermal_zone is locked down.
        val displayTemp = if (cpuTemp > 0f) cpuTemp else batteryTemp

        // Direct PowerManager thermal status is the truthiest signal when
        // available. Fall back to our temperature threshold logic if not.
        val thermalStateFromOs = readOsThermalState()
        val thermalState = when {
            thermalStateFromOs != null -> combineWithBattery(thermalStateFromOs, batteryPercent, isCharging)
            displayTemp >= TEMP_CRITICAL -> ThermalState.CRITICAL
            displayTemp >= TEMP_THROTTLE -> ThermalState.THROTTLE
            displayTemp >= TEMP_WARNING -> ThermalState.WARNING
            batteryPercent in 1..BATTERY_CRITICAL && !isCharging -> ThermalState.CRITICAL
            batteryPercent in 1..BATTERY_LOW && !isCharging -> ThermalState.WARNING
            else -> ThermalState.NORMAL
        }

        val recommendedThreads = when (thermalState) {
            ThermalState.CRITICAL -> 0  // Pause
            ThermalState.THROTTLE -> maxOf(1, currentThreads / 2)
            ThermalState.WARNING -> maxOf(1, currentThreads - 1)
            ThermalState.NORMAL -> currentThreads
        }

        return DeviceStatus(
            cpuTemp = displayTemp,
            batteryPercent = batteryPercent,
            isCharging = isCharging,
            thermalState = thermalState,
            recommendedThreads = recommendedThreads
        )
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
     *  battery outranks a NORMAL thermal state but not a CRITICAL one. */
    private fun combineWithBattery(
        os: ThermalState,
        batteryPercent: Int,
        isCharging: Boolean
    ): ThermalState {
        if (isCharging || batteryPercent <= 0) return os
        val batteryState = when {
            batteryPercent <= BATTERY_CRITICAL -> ThermalState.CRITICAL
            batteryPercent <= BATTERY_LOW -> ThermalState.WARNING
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

    /** One-shot dump of every thermal zone the OS exposes. Run once per session
     *  so we can see in logcat which zones exist and which are readable by the
     *  unprivileged app UID — vendors lock different zones on different ROMs. */
    private fun dumpAllThermalZones() {
        try {
            val thermalDir = File("/sys/class/thermal/")
            val zones = thermalDir.listFiles()?.filter { it.name.startsWith("thermal_zone") }?.sortedBy { it.name }
            if (zones.isNullOrEmpty()) {
                Log.w(TAG, "ZONE_DUMP: /sys/class/thermal has no thermal_zone* entries")
                return
            }
            Log.i(TAG, "ZONE_DUMP: ${zones.size} thermal zones found")
            for (zone in zones) {
                val type = try {
                    File(zone, "type").takeIf { it.exists() && it.canRead() }?.readText()?.trim() ?: "?"
                } catch (e: Exception) { "err:${e.message}" }
                val temp = try {
                    val f = File(zone, "temp")
                    if (f.exists() && f.canRead()) f.readText().trim() else "unreadable"
                } catch (e: Exception) { "err:${e.message}" }
                Log.i(TAG, "ZONE_DUMP ${zone.name} type=$type temp=$temp")
            }
        } catch (e: Exception) {
            Log.w(TAG, "ZONE_DUMP failed: ${e.message}")
        }
    }

    /**
     * Read from /sys/class/thermal/thermal_zone* as a raw-temperature source.
     * Many OEMs restrict these on retail Android; we fall through to battery
     * temperature in that case.
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

        try {
            val thermalDir = File("/sys/class/thermal/")
            if (!thermalDir.exists()) return 0.0f

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

                        // Most zones report in millidegrees
                        val tempC = if (rawTemp > 1000) rawTemp / 1000.0f else rawTemp

                        // Prefer CPU-related zones
                        if (type.contains("cpu", ignoreCase = true) ||
                            type.contains("tsens", ignoreCase = true) ||
                            type.contains("soc", ignoreCase = true)) {
                            if (tempC > maxTemp) {
                                maxTemp = tempC
                                maxZone = zone.name
                                maxType = type
                            }
                        } else if (maxTemp == 0.0f && tempC > 0) {
                            maxTemp = tempC
                            maxZone = zone.name
                            maxType = type
                        }
                    }
                } catch (e: Exception) {
                    // Skip unreadable zones
                }
            }
        } catch (e: Exception) {
            Log.d(TAG, "Cannot read thermal zones: ${e.message}")
        }

        if (maxTemp > 0f) {
            Log.i(TAG, "thermal max=$maxTemp°C from $maxZone ($maxType)")
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
