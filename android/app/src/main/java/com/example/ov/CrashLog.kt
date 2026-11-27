package com.example.ov

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.text.format.DateFormat
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

// 閃退紀錄：App 如果當掉，下次打開時把原因顯示出來，方便截圖回報
object CrashLog {

    // Kotlin 這邊的錯誤：寫到 crash.txt
    fun install(ctx: Context, where: String) {
        val file = File(ctx.filesDir, "crash.txt")
        val old = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            try {
                val sw = StringWriter()
                e.printStackTrace(PrintWriter(sw))
                file.writeText("位置：$where（執行緒 ${t.name}）\n\n$sw")
            } catch (_: Throwable) {}
            old?.uncaughtException(t, e)
        }
    }

    private fun reasonText(r: Int): String = when (r) {
        ApplicationExitInfo.REASON_CRASH -> "程式錯誤 (Kotlin)"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "C 程式當掉"
        ApplicationExitInfo.REASON_ANR -> "沒有回應 (ANR)"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "記憶體不足被系統關掉"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "用太多資源被系統關掉"
        ApplicationExitInfo.REASON_SIGNALED -> "被系統訊號關掉"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "啟動失敗"
        else -> ""
    }

    // C 程式當掉時，系統紀錄是二進位格式，挑出看得懂的文字
    private fun nativeTrace(info: ApplicationExitInfo): String {
        return try {
            val bytes = info.traceInputStream?.use { it.readBytes() } ?: return ""
            val out = ArrayList<String>()
            val sb = StringBuilder()
            for (b in bytes) {
                val c = b.toInt() and 0xFF
                if (c in 32..126) sb.append(c.toChar())
                else {
                    if (sb.length >= 4) out.add(sb.toString())
                    sb.setLength(0)
                }
            }
            out.filter { s ->
                s.contains("libov") || s.contains("SIG") || s.contains("abort", true) ||
                    s.contains("fortify", true) || s.contains("Java_") || s.contains("Cause") ||
                    (s.length in 4..60 && s.all { it.isLetterOrDigit() || it == '_' } && s.contains('_'))
            }.distinct().take(60).joinToString("\n")
        } catch (e: Exception) { "" }
    }

    // 還沒顯示過的最新一次閃退；沒有就回傳 null
    fun lastCrash(ctx: Context): String? {
        val prefs = ctx.getSharedPreferences("crash", Context.MODE_PRIVATE)
        val seen = prefs.getLong("seen", 0L)
        var text: String? = null
        var newest = seen
        try {
            val am = ctx.getSystemService(ActivityManager::class.java)
            for (info in am.getHistoricalProcessExitReasons(ctx.packageName, 0, 10)) {
                val why = reasonText(info.reason)
                if (info.timestamp <= seen || why.isEmpty()) continue
                if (info.timestamp > newest) {
                    newest = info.timestamp
                    val time = DateFormat.format("MM-dd HH:mm:ss", info.timestamp)
                    val proc = if (info.processName.endsWith(":report")) "分析畫面" else "主畫面"
                    var t = "時間：$time\n位置：$proc\n原因：$why\n說明：${info.description ?: ""}\n" +
                            "當時記憶體：${info.pss / 1024} MB"
                    if (info.reason == ApplicationExitInfo.REASON_CRASH_NATIVE) t += "\n\n" + nativeTrace(info)
                    text = t
                }
            }
        } catch (_: Exception) {}
        val f = File(ctx.filesDir, "crash.txt")
        if (f.exists()) {
            val k = f.readText()
            f.delete()
            text = (text ?: "") + "\n\n---- 程式錯誤細節 ----\n" + k
        }
        prefs.edit().putLong("seen", maxOf(newest, System.currentTimeMillis())).apply()
        return text?.trim()
    }
}
