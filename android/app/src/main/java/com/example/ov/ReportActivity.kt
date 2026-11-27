package com.example.ov

import android.graphics.Typeface
import android.os.Bundle
import android.os.Process
import android.text.Editable
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextWatcher
import android.text.style.ForegroundColorSpan
import android.text.style.RelativeSizeSpan
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.RandomAccessFile

// 進度條的位置：0 ～ 1000（比 0 ～ 100 更順）
fun permille(done: Double, total: Double): Int =
    if (total > 0) (done * 1000 / total).toInt().coerceIn(0, 1000) else 0

// 進度文字，例如：
// 正在分析  45%
// 120 / 260 MB · 35.2 MB/秒 · 約剩 4 秒
fun progressText(title: String, done: Double, total: Double, startMs: Long): String {
    val mb = 1024.0 * 1024.0
    val sec = (System.currentTimeMillis() - startMs) / 1000.0
    val speed = if (sec > 0.3) done / mb / sec else 0.0
    val sb = StringBuilder()
    if (total > 0) {
        val pct = (done * 100 / total).toInt().coerceIn(0, 100)
        sb.append("$title  $pct%\n")
        sb.append(String.format("%.0f / %.0f MB", done / mb, total / mb))
    } else {
        sb.append("$title\n")
        sb.append(String.format("%.0f MB", done / mb))
    }
    if (speed > 0) sb.append(String.format(" · %.1f MB/秒", speed))
    if (total > 0 && speed > 0 && sec >= 1 && done < total) {
        val left = ((total - done) / mb / speed).toInt() + 1
        sb.append(" · 約剩 $left 秒")
    }
    return sb.toString()
}

// 原始資料：遮掉常見的個人資料（Email、MAC、IMEI、SIM 卡號、手機號碼、座標、Wi-Fi 名稱、帳號）
// 用 by lazy：真的要遮資料時才建立，不會拖累其他畫面
// 注意：Android 的正規表示式裡 [ 後面不能直接接 :，要寫成 \\:
private val maskRules by lazy { listOf(
    Regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}") to "***@***",
    Regex("(?<![0-9A-Fa-f\\:])([0-9A-Fa-f]{2}[\\:\\-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f\\:])") to "XX:XX:XX:XX:XX:XX",
    Regex("(?<![\\d.])89\\d{17,18}(?![\\d.])") to "(SIM 卡號)",
    Regex("(?<![\\d.])\\d{15}(?![\\d.])") to "(序號)",
    Regex("(?<!\\d)(\\+?886|0)9\\d{2}[- ]?\\d{3}[- ]?\\d{3}(?!\\d)") to "(電話號碼)",
    Regex("\\+\\d{10,14}(?!\\d)") to "(電話號碼)",
    Regex("-?\\d{1,3}\\.\\d{4,}\\s*,\\s*-?\\d{1,3}\\.\\d{4,}") to "(座標)",
    Regex("((?:B?SSID|ssid|wifiNetworkKey|networkName|mSsid)[\\:=] ?)\"[^\"]*\"") to "$1\"***\"",
    Regex("(name=)[^,}\\]]*(, type=)") to "$1***$2"
) }

// words：C 程式從報告裡找到的 Wi-Fi 名稱、裝置名稱（常含人名、地址），整個換成 ***
fun maskPrivate(text: String, words: List<String>): String {
    var t = text
    for (w in words) t = t.replace(w, "***")
    for ((re, rep) in maskRules) t = re.replace(t, rep)
    return t
}

/* 結果畫面：呼叫 C 程式分析，可以選一般模式或專業模式 */
class ReportActivity : AppCompatActivity() {

    private lateinit var box: LinearLayout     // 整頁的內容
    private lateinit var scroll: ScrollView
    private lateinit var out: File             // C 程式把結果寫在這個資料夾
    private val heads = ArrayList<TextView>()  // 每個分類的標題（點一下收合 / 展開）
    private val bodies = ArrayList<View>()     // 每個分類的內容
    private val names = ArrayList<String>()
    private val states = ArrayList<String>()
    private var screen = 0                     // 0 結果頁、1 原始資料清單、2 某個區段的原文
    private var mainY = 0                      // 回到結果頁時捲回原本的位置

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        CrashLog.install(this, "分析畫面")
        val pad = dp(16)

        box = LinearLayout(this)
        box.orientation = LinearLayout.VERTICAL
        box.setPadding(pad, pad, pad, pad)
        scroll = ScrollView(this)
        scroll.fitsSystemWindows = true
        scroll.addView(box)
        setContentView(scroll)
        title = "檢測結果"

        // 返回鍵：在原文就回清單，在清單就回結果頁，在結果頁就離開
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                when (screen) {
                    2 -> showRawList()
                    1 -> { buildAll(); scroll.post { scroll.scrollTo(0, mainY) } }
                    else -> finish()
                }
            }
        })

        out = File(cacheDir, "out")
        if (done) { if (mode == 0) askMode() else buildAll(); return }   // 已經分析過了（例如畫面重建）

        val msg = TextView(this)
        msg.textSize = 16f
        msg.text = "正在準備..."
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal)
        bar.max = 1000
        box.addView(bar)
        box.addView(msg)

        // 每 0.2 秒問 C 程式讀到哪裡了
        val start = System.currentTimeMillis()
        val ticker = object : Runnable {
            override fun run() {
                val read = doneBytes()
                val total = totalBytes()
                bar.progress = permille(read, total)
                msg.text = when {
                    total <= 0 -> "正在準備..."
                    read >= total -> "正在分析  100%\n正在整理結果..."
                    else -> progressText("正在分析", read, total, start)
                }
                scroll.postDelayed(this, 200)
            }
        }
        scroll.post(ticker)

        val path = intent.getStringExtra("path") ?: ""
        // C 程式用比較多記憶體，給它一個大一點的空間（64 MB）來跑
        Thread(null, {
            out.deleteRecursively()
            out.mkdirs()
            val ok = analyze(path, out.path) == 1
            done = ok
            runOnUiThread {
                scroll.removeCallbacks(ticker)
                if (!isDestroyed) {
                    if (!ok) msg.text = "分析失敗\n\n" + read("log.txt")
                    else if (mode == 0) askMode()
                    else buildAll()
                }
            }
        }, "analyze", 64L * 1024 * 1024).start()
    }

    // 分析完先問要用哪一種模式
    private fun askMode() {
        AlertDialog.Builder(this)
            .setTitle("要用哪一種模式看？")
            .setItems(arrayOf(
                "一般模式\n常用的分類，簡單好懂",
                "專業模式\n全部分類、專業數據、白話說明，還可以看原始資料"
            )) { _, which ->
                mode = which + 1
                buildAll()
            }
            .setCancelable(false)
            .show()
    }

    private fun read(name: String): String {
        val f = File(out, name)
        return if (f.exists()) f.readText().trim() else ""
    }

    // 某一頁的內容：去掉第一行的「==== 標題 ====」；
    // 「※」開頭的是白話說明，一般模式不顯示，專業模式用灰色小一點的字
    private fun pageText(n: Int): CharSequence {
        val lines = read("p$n.txt").lines()
        val rest = if (lines.isNotEmpty() && lines[0].contains("====")) lines.drop(1) else lines
        val sb = SpannableStringBuilder()
        for (line in rest) {
            val isNote = line.trimStart().startsWith("※")
            if (isNote && mode != 2) continue
            if (sb.isEmpty() && line.isBlank()) continue
            val s = sb.length
            sb.append(line).append('\n')
            if (isNote) {
                sb.setSpan(ForegroundColorSpan(0xFF5F6368.toInt()), s, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                sb.setSpan(RelativeSizeSpan(0.92f), s, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
        }
        while (sb.isNotEmpty() && sb[sb.length - 1] == '\n') sb.delete(sb.length - 1, sb.length)
        return sb
    }

    // 整頁：型號 → 模式切換 → 分類目錄 → 每個分類的標題和內容
    private fun buildAll() {
        screen = 0
        title = "檢測結果"
        box.removeAllViews()
        heads.clear(); bodies.clear(); names.clear(); states.clear()

        val head = TextView(this)
        head.textSize = 16f
        head.text = read("head.txt")
        box.addView(head)

        // 切換模式
        val modeBtn = Button(this)
        modeBtn.isAllCaps = false
        modeBtn.text = if (mode == 2) "目前：專業模式（點這裡換成一般模式）" else "目前：一般模式（點這裡換成專業模式）"
        modeBtn.setOnClickListener {
            mode = if (mode == 2) 1 else 2
            buildAll()
        }
        box.addView(modeBtn)

        // 目錄：點一下就跳到那個分類
        val chips = LinearLayout(this)
        chips.orientation = LinearLayout.HORIZONTAL
        val chipScroll = HorizontalScrollView(this)
        chipScroll.addView(chips)
        box.addView(chipScroll)

        // 全部收合 / 全部展開
        val toggle = Button(this)
        toggle.text = "全部收合"
        box.addView(toggle)

        read("menu.txt").lines().filter { it.isNotBlank() }.forEachIndexed { i, line ->
            val parts = line.split("\t")
            val name = parts[0]
            val state = parts.getOrElse(1) { "" }
            val level = parts.getOrElse(2) { "1" }
            if (level == "2" && mode != 2) return@forEachIndexed     // 一般模式不顯示專業分類
            val warn = state.contains("注意")
            val idx = names.size
            names.add(name)
            states.add(state)

            val h = TextView(this)
            h.textSize = 17f
            h.setTypeface(null, Typeface.BOLD)
            h.setPadding(dp(12), dp(10), dp(12), dp(10))
            h.setBackgroundColor(if (warn) 0xFFFDECEA.toInt() else 0xFFE8EAED.toInt())
            h.setTextColor(if (warn) 0xFFB3261E.toInt() else 0xFF202124.toInt())
            val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            lp.topMargin = dp(16)
            h.layoutParams = lp

            val body = LinearLayout(this)
            body.orientation = LinearLayout.VERTICAL
            val text = TextView(this)
            text.typeface = Typeface.MONOSPACE
            text.textSize = 13f
            text.setTextIsSelectable(true)
            text.setPadding(dp(4), dp(8), dp(4), 0)
            text.text = pageText(i + 1)
            body.addView(text)
            if (name == "原始資料") {
                val open = Button(this)
                open.text = "打開原始資料清單"
                open.setOnClickListener { mainY = scroll.scrollY; showRawList() }
                body.addView(open)
            }

            heads.add(h)
            bodies.add(body)
            setHead(idx)
            h.setOnClickListener {
                body.visibility = if (body.visibility == View.VISIBLE) View.GONE else View.VISIBLE
                setHead(idx)
            }
            box.addView(h)
            box.addView(body)

            val c = Button(this)
            c.text = if (warn) "$name !" else name
            c.isAllCaps = false
            c.textSize = 13f
            c.minWidth = 0
            c.minimumWidth = 0
            c.setOnClickListener {
                body.visibility = View.VISIBLE
                setHead(idx)
                scroll.post { scroll.smoothScrollTo(0, h.top) }
            }
            chips.addView(c)
        }

        toggle.setOnClickListener {
            val anyOpen = bodies.any { it.visibility == View.VISIBLE }
            for (k in bodies.indices) {
                bodies[k].visibility = if (anyOpen) View.GONE else View.VISIBLE
                setHead(k)
            }
            toggle.text = if (anyOpen) "全部展開" else "全部收合"
        }
    }

    // 標題：▼ 打開、▶ 收起來，後面接 [正常] / [注意]
    private fun setHead(i: Int) {
        val open = bodies[i].visibility == View.VISIBLE
        val st = if (states[i].isEmpty()) "" else "   " + states[i]
        heads[i].text = (if (open) "▼ " else "▶ ") + names[i] + st
    }

    // ---------------- 原始資料 ----------------

    private class Sec(val start: Long, val end: Long, val kind: String, val name: String, val priv: Boolean, val note: String)
    private var rawPath = ""
    private val secs = ArrayList<Sec>()
    private var maskWords: List<String> = emptyList()

    private fun loadSections() {
        if (secs.isNotEmpty()) return
        maskWords = read("mask.txt").lines().filter { it.length >= 3 }.sortedByDescending { it.length }
        val lines = read("sections.txt").lines()
        if (lines.isEmpty()) return
        rawPath = lines[0].split("\t")[0]
        for (k in 1 until lines.size) {
            val f = lines[k].split("\t")
            if (f.size < 5) continue
            val s = f[0].toLongOrNull() ?: continue
            val e = f[1].toLongOrNull() ?: continue
            if (e <= s) continue
            secs.add(Sec(s, e, f[2], f[3], f[4] == "1", f.getOrElse(5) { "" }))
        }
    }

    private fun sizeText(b: Long): String = when {
        b >= 1024L * 1024 -> String.format("%.1f MB", b / 1024.0 / 1024.0)
        b >= 1024 -> String.format("%.0f KB", b / 1024.0)
        else -> "$b B"
    }

    // 清單：每個區段的名稱、白話說明、大小，上面可以搜尋
    private fun showRawList() {
        screen = 1
        title = "原始資料"
        loadSections()
        box.removeAllViews()

        val warn = TextView(this)
        warn.textSize = 14f
        warn.setTextColor(0xFFB3261E.toInt())
        warn.text = "這裡是報告的原文，共 ${secs.size} 個區段。已經遮掉常見的個人資料（帳號、電話、IMEI、Wi-Fi 名稱、座標等），" +
                "但仍可能有漏網之魚，請不要截圖分享。標示「個人資料」的區段不提供原文。"
        box.addView(warn)

        val search = EditText(this)
        search.hint = "搜尋區段名稱或說明，例如 battery、電池"
        search.setSingleLine()
        box.addView(search)

        val items = ArrayList<Pair<TextView, String>>()
        for (sec in secs) {
            val t = TextView(this)
            t.textSize = 14f
            t.setPadding(dp(8), dp(10), dp(8), dp(10))
            val note = if (sec.note.isNotEmpty()) sec.note else if (sec.kind == "D") "系統服務的狀態" else "系統資訊"
            val tag = if (sec.priv) "  (個人資料，不提供原文)" else ""
            t.text = "${sec.name}\n  $note · ${sizeText(sec.end - sec.start)}$tag"
            if (sec.priv) t.setTextColor(0xFF9AA0A6.toInt())
            else t.setOnClickListener { showSection(sec) }
            box.addView(t)
            items.add(Pair(t, (sec.name + " " + note).lowercase()))
        }
        search.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) {
                val q = s.toString().trim().lowercase()
                for ((t, key) in items) t.visibility = if (q.isEmpty() || key.contains(q)) View.VISIBLE else View.GONE
            }
        })
        scroll.post { scroll.scrollTo(0, 0) }
    }

    // 讀檔案裡的一段（最多 200 KB 一次）
    private fun readChunk(from: Long, len: Long): String {
        return try {
            RandomAccessFile(rawPath, "r").use { f ->
                f.seek(from)
                val buf = ByteArray(len.toInt())
                val n = f.read(buf)
                if (n <= 0) "" else String(buf, 0, n, Charsets.UTF_8)
            }
        } catch (e: Exception) {
            "讀不到原文：" + e.message
        }
    }

    // 某個區段的原文
    private fun showSection(sec: Sec) {
        screen = 2
        title = sec.name
        box.removeAllViews()
        val chunk = 200L * 1024
        var pos = sec.start

        val info = TextView(this)
        info.textSize = 14f
        info.text = "${sec.name}\n${if (sec.note.isNotEmpty()) sec.note else "說明：系統的內部資料"}\n大小 ${sizeText(sec.end - sec.start)}，每次顯示 200 KB\n藍色「←」後面是英文詞的中文意思；紅色是錯誤或警告"
        box.addView(info)

        val text = TextView(this)
        text.typeface = Typeface.MONOSPACE
        text.textSize = 11f
        text.setTextIsSelectable(true)
        box.addView(text)

        val more = Button(this)
        more.text = "再載入 200 KB"
        box.addView(more)

        fun load() {
            val len = minOf(chunk, sec.end - pos)
            if (len <= 0) { more.visibility = View.GONE; return }
            text.append(Glossary.annotate(maskPrivate(readChunk(pos, len), maskWords)))
            pos += len
            if (pos >= sec.end) more.visibility = View.GONE
        }
        more.setOnClickListener { load() }
        load()
        scroll.post { scroll.scrollTo(0, 0) }
    }

    // 離開結果畫面就結束這個分析用的程序，下次分析會從乾淨的狀態開始
    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) Process.killProcess(Process.myPid())
    }

    external fun analyze(path: String, outDir: String): Int
    external fun doneBytes(): Double
    external fun totalBytes(): Double

    companion object {
        var done = false
        var mode = 0          // 0 還沒選、1 一般模式、2 專業模式
        init {
            System.loadLibrary("ov")
        }
    }
}
