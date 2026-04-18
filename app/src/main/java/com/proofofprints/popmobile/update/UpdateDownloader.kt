/**
 * Downloads a release APK via Android's DownloadManager and fires the
 * install intent once complete.
 *
 * - Target: the app's external files dir (`/Android/data/<pkg>/files/Download/`)
 *   so we don't need the WRITE_EXTERNAL_STORAGE permission on older devices.
 * - Progress: polled via DownloadManager.query() at 500 ms intervals.
 * - Install: handed off to the system installer via an ACTION_VIEW intent
 *   with a FileProvider content:// URI. The OS verifies signature match
 *   and shows its standard "Update existing app?" dialog.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.update

import android.app.DownloadManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.util.Log
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import java.io.File

sealed class DownloadProgress {
    object Starting : DownloadProgress()
    data class Running(val bytesDownloaded: Long, val totalBytes: Long) : DownloadProgress() {
        val fraction: Float
            get() = if (totalBytes > 0) (bytesDownloaded.toFloat() / totalBytes) else 0f
    }
    data class Complete(val file: File) : DownloadProgress()
    data class Failed(val reason: String) : DownloadProgress()
}

class UpdateDownloader(private val context: Context) {

    private val dm: DownloadManager =
        context.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager

    /**
     * Emits DownloadProgress events until the download succeeds, fails, or
     * the coroutine is cancelled. Safe to collect from a Compose
     * LaunchedEffect.
     */
    fun download(update: UpdateInfo): Flow<DownloadProgress> = flow {
        emit(DownloadProgress.Starting)

        // Overwrite any previous partial download with the same asset name.
        val destFile = File(
            context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS),
            update.apkAssetName
        )
        if (destFile.exists()) destFile.delete()

        val request = DownloadManager.Request(Uri.parse(update.apkUrl))
            .setTitle("PoPMobile ${update.tagName}")
            .setDescription("Downloading update…")
            .setDestinationUri(Uri.fromFile(destFile))
            .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
            .setAllowedOverMetered(true)
            .setMimeType("application/vnd.android.package-archive")

        val id = dm.enqueue(request)

        try {
            while (true) {
                val query = DownloadManager.Query().setFilterById(id)
                dm.query(query).use { cursor ->
                    if (cursor == null || !cursor.moveToFirst()) {
                        emit(DownloadProgress.Failed("Download lost from DownloadManager"))
                        return@flow
                    }
                    val status = cursor.getInt(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS))
                    val soFar = cursor.getLong(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR))
                    val total = cursor.getLong(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES))

                    when (status) {
                        DownloadManager.STATUS_SUCCESSFUL -> {
                            emit(DownloadProgress.Complete(destFile))
                            return@flow
                        }
                        DownloadManager.STATUS_FAILED -> {
                            val reason = cursor.getInt(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_REASON))
                            emit(DownloadProgress.Failed("Download failed (reason $reason)"))
                            return@flow
                        }
                        DownloadManager.STATUS_PENDING,
                        DownloadManager.STATUS_RUNNING,
                        DownloadManager.STATUS_PAUSED -> {
                            emit(DownloadProgress.Running(soFar, if (total > 0) total else update.apkSizeBytes))
                        }
                    }
                }
                delay(500)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Download cancelled / errored: ${e.message}")
            dm.remove(id)
            emit(DownloadProgress.Failed(e.message ?: "Cancelled"))
        }
    }.flowOn(Dispatchers.IO)

    /**
     * Fire Android's "Install this APK?" dialog. Requires the user to have
     * granted REQUEST_INSTALL_PACKAGES for this app (they'll be prompted
     * the first time).
     */
    fun launchInstall(apkFile: File) {
        val authority = "${context.packageName}.fileprovider"
        val uri = FileProvider.getUriForFile(context, authority, apkFile)
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
    }

    companion object {
        private const val TAG = "UpdateDownloader"
    }
}
