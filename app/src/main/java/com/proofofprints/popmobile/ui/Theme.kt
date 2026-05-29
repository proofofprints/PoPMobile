/**
 * OverMobile Material3 theme — OverBuild Labs palette (emerald primary,
 * violet accent, near-black surfaces).
 *
 * Copyright (c) 2026 OverBuild Labs
 */
package com.proofofprints.popmobile.ui

import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val KASMinerColorScheme = darkColorScheme(
    primary = Color(0xFF10B981),       // emerald-500
    onPrimary = Color.Black,
    secondary = Color(0xFF8B5CF6),     // violet-500
    onSecondary = Color.White,
    background = Color(0xFF0C0C0F),    // page background
    onBackground = Color.White,
    surface = Color(0xFF16161B),       // section background
    onSurface = Color.White,
    surfaceVariant = Color(0xFF16161B),// card background (neutral grey-black, matches sections)
    onSurfaceVariant = Color.White,
    error = Color(0xFFFF4444),
    onError = Color.White
)

@Composable
fun KASMinerTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = KASMinerColorScheme,
        typography = Typography(),
        content = content
    )
}
