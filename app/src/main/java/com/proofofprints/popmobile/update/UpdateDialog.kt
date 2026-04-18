/**
 * "Update available" dialog — shows release notes, lets the user download
 * + install the new APK without leaving the app.
 *
 * Flow:
 *   1. Prompt: release notes + Later / Download
 *   2. If install permission missing → NeedPermission phase with a clear
 *      "Open Settings" CTA (no download kicks off until permission
 *      granted, so we can never get stuck with a cached APK and no way
 *      to install it)
 *   3. Downloading: linear progress bar
 *   4. Complete: hand off to the system installer via UpdateDownloader.
 *      InstallReceiver is the cold-boot safety net if the app is killed
 *      mid-download.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.update

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import kotlinx.coroutines.flow.collectLatest

private enum class Phase { Prompt, NeedPermission, Downloading, Failed }

@Composable
fun UpdateDialog(
    update: UpdateInfo,
    onDismiss: () -> Unit,
    onDecline: () -> Unit = onDismiss,
) {
    val context = LocalContext.current
    val downloader = remember { UpdateDownloader(context) }

    var phase by remember { mutableStateOf(Phase.Prompt) }
    var progress by remember { mutableStateOf<DownloadProgress>(DownloadProgress.Starting) }
    var startTrigger by remember { mutableStateOf(0) }
    var failReason by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(startTrigger) {
        if (startTrigger == 0) return@LaunchedEffect
        downloader.download(update).collectLatest { p ->
            progress = p
            when (p) {
                is DownloadProgress.Complete -> {
                    // Best-effort in-app install. Receiver dedupes if it fires first.
                    downloader.launchInstall(p.file)
                    onDismiss()
                }
                is DownloadProgress.Failed -> {
                    failReason = p.reason
                    phase = Phase.Failed
                }
                else -> Unit
            }
        }
    }

    Dialog(onDismissRequest = {
        // Only allow back/outside-tap dismiss when we're not mid-download.
        // Treat it as a "decline" so we don't nag about this version again.
        if (phase == Phase.Prompt || phase == Phase.NeedPermission || phase == Phase.Failed) {
            onDecline()
        }
    }) {
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = Color(0xFF1A1A2E)
        ) {
            Column(modifier = Modifier.padding(20.dp)) {
                Text(
                    "Update available",
                    color = Color(0xFF49EACB),
                    fontSize = 22.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    update.releaseName.ifBlank { update.tagName },
                    color = Color.White,
                    fontSize = 14.sp,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(Modifier.height(12.dp))

                when (phase) {
                    Phase.Prompt -> PromptBody(update, onLater = onDecline) {
                        if (downloader.canInstallPackages()) {
                            phase = Phase.Downloading
                            startTrigger++
                        } else {
                            phase = Phase.NeedPermission
                        }
                    }
                    Phase.NeedPermission -> NeedPermissionBody(
                        onCancel = onDecline,
                        onOpenSettings = {
                            openInstallPermissionSettings(context)
                            // Close the dialog WITHOUT marking this version as
                            // dismissed — the user said yes, they just have to
                            // grant the permission first. They'll tap Check
                            // for updates again after granting.
                            onDismiss()
                        }
                    )
                    Phase.Downloading -> DownloadingBody(progress, update)
                    Phase.Failed -> FailedBody(failReason ?: "Unknown", onClose = onDismiss) {
                        phase = Phase.Downloading
                        startTrigger++
                    }
                }
            }
        }
    }
}

@Composable
private fun PromptBody(update: UpdateInfo, onLater: () -> Unit, onDownload: () -> Unit) {
    Column(
        modifier = Modifier
            .heightIn(max = 260.dp)
            .verticalScroll(rememberScrollState())
    ) {
        Text(
            update.releaseNotes.ifBlank { "Release notes not available." },
            color = Color(0xFFCCCCCC),
            fontSize = 13.sp,
            fontFamily = FontFamily.Monospace
        )
    }
    Spacer(Modifier.height(12.dp))
    Text(
        "Size: ${formatBytes(update.apkSizeBytes)}",
        color = Color.Gray,
        fontSize = 11.sp,
        fontFamily = FontFamily.Monospace
    )
    Spacer(Modifier.height(16.dp))
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.End
    ) {
        TextButton(onClick = onLater) {
            Text("Later", color = Color.Gray, fontFamily = FontFamily.Monospace)
        }
        Spacer(Modifier.width(8.dp))
        Button(
            onClick = onDownload,
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF49EACB))
        ) {
            Text("Download", color = Color.Black, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun NeedPermissionBody(onCancel: () -> Unit, onOpenSettings: () -> Unit) {
    Text(
        "Android requires explicit permission to install apps from PoPMobile. " +
            "Tap Open Settings, enable \"Allow from this source\", then come " +
            "back and tap Check for updates again.",
        color = Color(0xFFCCCCCC),
        fontSize = 13.sp,
        fontFamily = FontFamily.Monospace
    )
    Spacer(Modifier.height(16.dp))
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.End
    ) {
        TextButton(onClick = onCancel) {
            Text("Cancel", color = Color.Gray, fontFamily = FontFamily.Monospace)
        }
        Spacer(Modifier.width(8.dp))
        Button(
            onClick = onOpenSettings,
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF49EACB))
        ) {
            Text("Open Settings", color = Color.Black, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun DownloadingBody(progress: DownloadProgress, update: UpdateInfo) {
    val (label, fraction) = when (progress) {
        is DownloadProgress.Starting ->
            "Starting download…" to 0f
        is DownloadProgress.Running -> {
            val total = if (progress.totalBytes > 0) progress.totalBytes else update.apkSizeBytes
            val pct = if (total > 0) (progress.bytesDownloaded.toFloat() / total) else 0f
            "${formatBytes(progress.bytesDownloaded)} / ${formatBytes(total)}" to pct
        }
        is DownloadProgress.Complete ->
            "Download complete. Opening installer…" to 1f
        is DownloadProgress.Failed ->
            "Failed" to 0f
    }

    if (fraction > 0f && progress !is DownloadProgress.Complete) {
        LinearProgressIndicator(
            progress = fraction,
            modifier = Modifier.fillMaxWidth(),
            color = Color(0xFF49EACB),
            trackColor = Color(0xFF2A2A40)
        )
    } else {
        LinearProgressIndicator(
            modifier = Modifier.fillMaxWidth(),
            color = Color(0xFF49EACB),
            trackColor = Color(0xFF2A2A40)
        )
    }
    Spacer(Modifier.height(10.dp))
    Text(label, color = Color(0xFFCCCCCC), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
}

@Composable
private fun FailedBody(reason: String, onClose: () -> Unit, onRetry: () -> Unit) {
    Text(
        "Download failed: $reason",
        color = Color(0xFFFF6B6B),
        fontSize = 13.sp,
        fontFamily = FontFamily.Monospace
    )
    Spacer(Modifier.height(16.dp))
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.End
    ) {
        TextButton(onClick = onClose) {
            Text("Close", color = Color.Gray, fontFamily = FontFamily.Monospace)
        }
        Spacer(Modifier.width(8.dp))
        Button(
            onClick = onRetry,
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF49EACB))
        ) {
            Text("Retry", color = Color.Black, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
        }
    }
}

private fun openInstallPermissionSettings(context: Context) {
    val intent = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES)
        .setData(Uri.parse("package:${context.packageName}"))
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    context.startActivity(intent)
}

private fun formatBytes(b: Long): String = when {
    b <= 0L -> "—"
    b < 1024 -> "$b B"
    b < 1024L * 1024 -> String.format("%.1f KB", b / 1024f)
    b < 1024L * 1024 * 1024 -> String.format("%.1f MB", b / (1024f * 1024f))
    else -> String.format("%.2f GB", b / (1024f * 1024f * 1024f))
}
