/**
 * Thermal and battery monitoring for safe mobile mining.
 *
 * Reads CPU thermal zones and battery state to automatically
 * throttle or pause mining when the device gets too hot or
 * battery drops too low.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.service

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

        /** Temperature thresholds in Celsius */
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
        val cpuTemp: Float,           // Celsius (0 if unavailable)
        val batteryPercent: Int,      // 0-100
        val isCharging: Boolean,
        val thermalState: ThermalState,
        val recommendedThreads: Int   // Based on thermal state
    )

    private var maxThreads: Int = Runtime.getRuntime().availableProcessors()

    /**
     * Read current device thermal and battery status.
     */
    fun getStatus(currentThreads: Int): DeviceStatus {
        val cpuTemp = readCpuTemperature()
        val batteryPercent = getBatteryPercent()
        val isCharging = isDeviceCharging()

        maxThreads = currentThreads

        val thermalState = when {
            cpuTemp >= TEMP_CRITICAL -> ThermalState.CRITICAL
            cpuTemp >= TEMP_THROTTLE -> ThermalState.THROTTLE
            cpuTemp >= TEMP_WARNING -> ThermalState.WARNING
            batteryPercent <= BATTERY_CRITICAL && !isCharging -> ThermalState.CRITICAL
            batteryPercent <= BATTERY_LOW && !isCharging -> ThermalState.WARNING
            else -> ThermalState.NORMAL
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
            recommendedThreads = recommendedThreads
        )
    }

    /**
     * Read CPU temperature from thermal zones.
     * Returns temperature in Celsius, or 0 if unavailable.
     */
    private fun readCpuTemperature(): Float {
        // Try Android PowerManager thermal status (API 29+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            try {
                val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
                val thermalStatus = pm.currentThermalStatus
                // Map thermal status to approximate temperature
                return when (thermalStatus) {
                    PowerManager.THERMAL_STATUS_NONE -> 35.0f
                    PowerManager.THERMAL_STATUS_LIGHT -> 42.0f
                    PowerManager.THERMAL_STATUS_MODERATE -> 48.0f
                    PowerManager.THERMAL_STATUS_SEVERE -> 53.0f
                    PowerManager.THERMAL_STATUS_CRITICAL -> 58.0f
                    PowerManager.THERMAL_STATUS_EMERGENCY -> 65.0f
                    PowerManager.THERMAL_STATUS_SHUTDOWN -> 70.0f
                    else -> readThermalZones()
                }
            } catch (e: Exception) {
                Log.d(TAG, "PowerManager thermal API unavailable: ${e.message}")
            }
        }

        return readThermalZones()
    }

    /**
     * Read from /sys/class/thermal/thermal_zone* as fallback.
     */
    private fun readThermalZones(): Float {
        var maxTemp = 0.0f

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

                        // Most zones report in millidegrees
                        val tempC = if (rawTemp > 1000) rawTemp / 1000.0f else rawTemp

                        // Prefer CPU-related zones
                        if (type.contains("cpu", ignoreCase = true) ||
                            type.contains("tsens", ignoreCase = true) ||
                            type.contains("soc", ignoreCase = true)) {
                            if (tempC > maxTemp) maxTemp = tempC
                        } else if (maxTemp == 0.0f && tempC > 0) {
                            maxTemp = tempC
                        }
                    }
                } catch (e: Exception) {
                    // Skip unreadable zones
                }
            }
        } catch (e: Exception) {
            Log.d(TAG, "Cannot read thermal zones: ${e.message}")
        }

        return maxTemp
    }

    /**
     * Get battery percentage (0-100).
     */
    private fun getBatteryPercent(): Int {
        val bm = context.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
        return bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
    }

    /**
     * Check if the device is currently charging.
     */
    private fun isDeviceCharging(): Boolean {
        val intent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        return status == BatteryManager.BATTERY_STATUS_CHARGING ||
               status == BatteryManager.BATTERY_STATUS_FULL
    }
}
