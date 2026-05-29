/**
 * Splash screen — single OverBuild Labs hex + 'OverMobile' wordmark,
 * then navigates to MainActivity. Brand attribution (OverBuild Labs,
 * support email, website) lives in the in-app About card.
 *
 * Copyright (c) 2026 OverBuild Labs
 */
package com.proofofprints.popmobile.ui

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.proofofprints.popmobile.R
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
        LaunchedEffect(Unit) {
            delay(2000)
            onFinished()
        }

        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xFF0C0C0F)),
            contentAlignment = Alignment.Center
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Image(
                    painter = painterResource(id = R.drawable.overbuild_logo_large),
                    contentDescription = "OverBuild Labs Logo",
                    modifier = Modifier.size(180.dp)
                )

                Spacer(modifier = Modifier.height(24.dp))

                Text(
                    "OverMobile",
                    color = Color.White,
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}
