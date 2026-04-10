/**
 * Splash screen — shows Kaspa logo first, then PoP logo, then navigates to MainActivity.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.ui

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.proofofprints.kasminer.R
import kotlinx.coroutines.delay

class SplashActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            SplashScreen {
                startActivity(Intent(this@SplashActivity, MainActivity::class.java))
                finish()
            }
        }
    }

    @Composable
    fun SplashScreen(onFinished: () -> Unit) {
        // 0 = Kaspa logo, 1 = PoP logo, 2 = done
        var phase by remember { mutableIntStateOf(0) }

        val kaspaAlpha by animateFloatAsState(
            targetValue = if (phase == 0) 1f else 0f,
            animationSpec = tween(durationMillis = 800),
            label = "kaspaFade"
        )

        val popAlpha by animateFloatAsState(
            targetValue = if (phase == 1) 1f else 0f,
            animationSpec = tween(durationMillis = 800),
            label = "popFade"
        )

        LaunchedEffect(Unit) {
            // Phase 0: Show Kaspa logo
            delay(2000)
            // Phase 1: Show PoP logo
            phase = 1
            delay(2000)
            // Done
            onFinished()
        }

        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xFF0F0F23)),
            contentAlignment = Alignment.Center
        ) {
            // Kaspa logo (phase 0)
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.alpha(kaspaAlpha)
            ) {
                Image(
                    painter = painterResource(id = R.drawable.kaspa_logo_large),
                    contentDescription = "Kaspa Logo",
                    modifier = Modifier.size(180.dp)
                )

                Spacer(modifier = Modifier.height(24.dp))

                Text(
                    "PoPMiner",
                    color = Color(0xFF49EACB),
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }

            // PoP logo (phase 1)
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.alpha(popAlpha)
            ) {
                Image(
                    painter = painterResource(id = R.drawable.pop_logo),
                    contentDescription = "Proof of Prints Logo",
                    modifier = Modifier.size(128.dp)
                )

                Spacer(modifier = Modifier.height(24.dp))

                Text(
                    "Proof of Prints",
                    color = Color.White,
                    fontSize = 22.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}
