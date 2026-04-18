/**
 * Fires the system install intent once DownloadManager reports the update
 * APK finished. Registered in the manifest so it survives the app being
 * killed mid-download — the flow observer in UpdateDialog is the in-app
 * path, this is the cold-boot safety net.
 *
 * Dedupe: whichever side (flow observer vs. this receiver) handles the
 * install first clears the stored download ID. The other side then sees
 * no match and does nothing.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.update

import android.app.DownloadManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.core.content.FileProvider
import java.io.File

class InstallReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != DownloadManager.ACTION_DOWNLOAD_COMPLETE) return
        val broadcastId = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1L)
        if (broadcastId == -1L) return

        val prefs = context.applicationContext
            .getSharedPreferences(PrefsKeys.PREFS, Context.MODE_PRIVATE)
        val pendingId = prefs.getLong(PrefsKeys.PENDING_ID, -1L)
        if (pendingId == -1L || pendingId != broadcastId) {
            Log.i(TAG, "Ignoring download $broadcastId (pending=$pendingId)")
            return
        }

        // Clear the pending ID first so the in-app flow observer doesn't
        // also try to install the same file.
        prefs.edit().putLong(PrefsKeys.PENDING_ID, -1L).apply()

        val dm = context.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
        val cursor = dm.query(DownloadManager.Query().setFilterById(broadcastId))
        cursor.use { c ->
            if (c == null || !c.moveToFirst()) {
                Log.w(TAG, "DownloadManager has no record of $broadcastId")
                return
            }
            val status = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS))
            if (status != DownloadManager.STATUS_SUCCESSFUL) {
                Log.w(TAG, "Download $broadcastId did not succeed (status=$status)")
                return
            }
            val localUri = c.getString(c.getColumnIndexOrThrow(DownloadManager.COLUMN_LOCAL_URI))
                ?: return
            val file = File(Uri.parse(localUri).path ?: return)
            if (!file.exists()) {
                Log.w(TAG, "Downloaded file missing: $file")
                return
            }
            launchInstall(context.applicationContext, file)
        }
    }

    private fun launchInstall(context: Context, apkFile: File) {
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
        private const val TAG = "InstallReceiver"
    }
}

/** Shared SharedPreferences keys for the update subsystem. */
internal object PrefsKeys {
    const val PREFS = "update_checker"
    const val PENDING_ID = "pending_download_id"
}
