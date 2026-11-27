package com.example.ov

import android.content.Context
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

// 規格資料庫：App 裡內建一份（assets），有網路時從 GitHub 下載最新版
// 放在 cacheDir/data，C 程式會從這裡讀
object DataSync {
    private const val BASE =
        "https://raw.githubusercontent.com/often591-create/phone-log-turn-people-can-understand/main/data/"
    private val FILES = listOf("devices.txt", "parts.tsv")

    fun dir(ctx: Context) = File(ctx.cacheDir, "data")

    private fun localVersion(ctx: Context): Int =
        File(dir(ctx), "version.txt").takeIf { it.exists() }?.readText()?.trim()?.toIntOrNull() ?: 0

    private fun assetVersion(ctx: Context): Int = try {
        ctx.assets.open("version.txt").bufferedReader().use { it.readText().trim().toIntOrNull() ?: 0 }
    } catch (e: Exception) { 0 }

    // 確定 data 資料夾裡有東西：沒有、或內建的比較新，就用內建的
    @Synchronized
    fun prepare(ctx: Context) {
        try {
            val d = dir(ctx)
            d.mkdirs()
            val av = assetVersion(ctx)
            if (av > 0 && (av > localVersion(ctx) || FILES.any { !File(d, it).exists() })) {
                for (f in FILES + "version.txt") {
                    ctx.assets.open(f).use { i -> File(d, f).outputStream().use { o -> i.copyTo(o) } }
                }
            }
        } catch (_: Exception) {}
    }

    private fun get(url: String, timeoutMs: Int): String? = try {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = timeoutMs
        c.readTimeout = timeoutMs
        c.useCaches = false
        try {
            if (c.responseCode == 200) c.inputStream.bufferedReader().use { it.readText() } else null
        } finally { c.disconnect() }
    } catch (e: Exception) { null }

    // 在背景執行：先問版本號（很小），有新版才下載整張表；任何一步失敗就放棄，繼續用舊的
    fun update(ctx: Context) {
        prepare(ctx)
        val remote = get(BASE + "version.txt", 4000)?.trim()?.toIntOrNull() ?: return
        if (remote <= localVersion(ctx)) return
        val got = HashMap<String, String>()
        for (f in FILES) {
            val t = get(BASE + f, 15000) ?: return
            if (!t.startsWith("#")) return          // 內容不對（例如網路被導到別的頁面）
            got[f] = t
        }
        synchronized(this) {
            val d = dir(ctx)
            for ((f, t) in got) {
                val tmp = File(d, "$f.tmp")
                tmp.writeText(t)
                tmp.renameTo(File(d, f))
            }
            File(d, "version.txt").writeText(remote.toString())   // 最後才寫版本號
        }
    }
}
