/**
 * Splash screen — shows PoPMiner logo and Proof of Prints branding.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.kasminer.ui

import android.annotation.SuppressLint
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.core.*
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

@SuppressLint("CustomSplashScreen")
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
        // Fade-in animation
        val alpha = remember { Animatable(0f) }

        LaunchedEffect(Unit) {
            alpha.animateTo(
                targetValue = 1f,
                animationSpec = tween(durationMillis = 800, easing = EaseOutCubic)
            )
            delay(1500)
            onFinished()
        }

        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xFF0F0F23)),
            contentAlignment = Alignment.Center
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.alpha(alpha.value)
            ) {
                // Logo
                Image(
                    painter = painterResource(id = R.drawable.ic_popminer_logo),
                    contentDescription = "PoPMiner Logo",
                    modifier = Modifier.size(200.dp)
                )

                Spacer(modifier = Modifier.height(24.dp))

                // PoPMiner title
                Text(
                    text = "PoPMiner",
                    color = Color(0xFF49EACB),
                    fontSize = 42.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )

                Spacer(modifier = Modifier.height(8.dp))

                // Proof of Prints subtitle
                Text(
                    text = "Proof of Prints",
                    color = Color.White,
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Light,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}
