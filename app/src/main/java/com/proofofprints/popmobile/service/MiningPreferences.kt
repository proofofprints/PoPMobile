/**
 * Persistent user-configurable knobs for thermal protection and power
 * management. Backed by SharedPreferences (same as the rest of the app)
 * to keep storage consistent and dependency-free.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.service

import android.content.Context
import android.content.SharedPreferences

class MiningPreferences(context: Context) {

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    // ===== Thermal protection =====

    /** Master switch. When false, mining never pauses or throttles on temperature. */
    var thermalProtectionEnabled: Boolean
        get() = prefs.getBoolean(KEY_THERMAL_ENABLED, DEFAULT_THERMAL_ENABLED)
        set(value) { prefs.edit().putBoolean(KEY_THERMAL_ENABLED, value).apply() }

    /** Info-only threshold — UI warning, no throttling. °C. */
    var warnTempC: Float
        get() = prefs.getFloat(KEY_WARN_TEMP, DEFAULT_WARN_TEMP)
        set(value) { prefs.edit().putFloat(KEY_WARN_TEMP, value).apply() }

    /** Reduce thread count to N/2 at this temperature. °C. */
    var throttleTempC: Float
        get() = prefs.getFloat(KEY_THROTTLE_TEMP, DEFAULT_THROTTLE_TEMP)
        set(value) { prefs.edit().putFloat(KEY_THROTTLE_TEMP, value).apply() }

    /** Pause mining entirely at this temperature. °C. */
    var pauseTempC: Float
        get() = prefs.getFloat(KEY_PAUSE_TEMP, DEFAULT_PAUSE_TEMP)
        set(value) { prefs.edit().putFloat(KEY_PAUSE_TEMP, value).apply() }

    /** Temperature that must be reached (sustained) to resume after a pause. °C.
     *  Hysteresis gap between [pauseTempC] and [resumeTempC] prevents thrash. */
    var resumeTempC: Float
        get() = prefs.getFloat(KEY_RESUME_TEMP, DEFAULT_RESUME_TEMP)
        set(value) { prefs.edit().putFloat(KEY_RESUME_TEMP, value).apply() }

    // ===== Power / battery =====

    /** User has an external power source (CellHasher-style rig, bench PSU, etc.)
     *  Disables all battery-based gates because the reported battery state isn't
     *  meaningful when the battery is bypassed or absent. */
    var externalPowerMode: Boolean
        get() = prefs.getBoolean(KEY_EXT_POWER, DEFAULT_EXT_POWER)
        set(value) { prefs.edit().putBoolean(KEY_EXT_POWER, value).apply() }

    /** Only allow mining while the device is charging. Ignored when
     *  [externalPowerMode] is true. */
    var requireCharging: Boolean
        get() = prefs.getBoolean(KEY_REQUIRE_CHARGING, DEFAULT_REQUIRE_CHARGING)
        set(value) { prefs.edit().putBoolean(KEY_REQUIRE_CHARGING, value).apply() }

    /** Pause mining when battery drops below this percentage. Ignored when
     *  [externalPowerMode] is true. */
    var batteryCutoffPercent: Int
        get() = prefs.getInt(KEY_BATT_CUTOFF, DEFAULT_BATT_CUTOFF)
        set(value) { prefs.edit().putInt(KEY_BATT_CUTOFF, value).apply() }

    companion object {
        private const val PREFS_NAME = "mining_protect"

        // Thermal keys
        private const val KEY_THERMAL_ENABLED = "thermal_enabled"
        private const val KEY_WARN_TEMP = "warn_temp"
        private const val KEY_THROTTLE_TEMP = "throttle_temp"
        private const val KEY_PAUSE_TEMP = "pause_temp"
        private const val KEY_RESUME_TEMP = "resume_temp"

        // Power keys
        private const val KEY_EXT_POWER = "ext_power"
        private const val KEY_REQUIRE_CHARGING = "require_charging"
        private const val KEY_BATT_CUTOFF = "batt_cutoff"

        // Defaults tuned to TCL A3X real data: idle ~38 °C, sustainable mining
        // peaks ~78 °C with 6 threads. Kernel shutdown trip is ~100 °C.
        const val DEFAULT_THERMAL_ENABLED = true
        const val DEFAULT_WARN_TEMP = 75.0f
        const val DEFAULT_THROTTLE_TEMP = 85.0f
        const val DEFAULT_PAUSE_TEMP = 92.0f
        const val DEFAULT_RESUME_TEMP = 70.0f

        const val DEFAULT_EXT_POWER = false
        const val DEFAULT_REQUIRE_CHARGING = true
        const val DEFAULT_BATT_CUTOFF = 15

        // Slider bounds used by the Settings UI
        const val TEMP_SLIDER_MIN = 50.0f
        const val TEMP_SLIDER_MAX = 100.0f
        const val BATT_CUTOFF_MIN = 5
        const val BATT_CUTOFF_MAX = 50
    }
}
