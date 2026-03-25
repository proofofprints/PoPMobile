/**
 * KAS Miner Material3 theme — dark theme with Kaspa green accent.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.ui

import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val KASMinerColorScheme = darkColorScheme(
    primary = Color(0xFF49EACB),
    onPrimary = Color.Black,
    secondary = Color(0xFFFFD700),
    onSecondary = Color.Black,
    background = Color(0xFF0F0F23),
    onBackground = Color.White,
    surface = Color(0xFF1A1A2E),
    onSurface = Color.White,
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
