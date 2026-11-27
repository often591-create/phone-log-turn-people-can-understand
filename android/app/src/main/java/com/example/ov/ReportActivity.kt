package com.example.ov

import android.graphics.Typeface
import android.os.Bundle
import android.os.Process
import android.view.View
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File

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

/* 結果畫面：呼叫 C 程式分析，把所有分類一次列在同一頁 */
class ReportActivity : AppCompatActivity() {

    private lateinit var box: LinearLayout     // 整頁的內容
    private lateinit var scroll: ScrollView
    private lateinit var out: File             // C 程式把結果寫在這個資料夾
    private val heads = ArrayList<TextView>()  // 每個分類的標題（點一下收合 / 展開）
    private val bodies = ArrayList<TextView>() // 每個分類的內容
    private val names = ArrayList<String>()
    private val states = ArrayList<String>()

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(16)

        box = LinearLayout(this)
        box.orientation = LinearLayout.VERTICAL
        box.setPadding(pad, pad, pad, pad)
        scroll = ScrollView(this)
        scroll.fitsSystemWindows = true
        scroll.addView(box)
        setContentView(scroll)
        title = "檢測結果"

        out = File(cacheDir, "out")
        if (done) { buildAll(); return }      // 已經分析過了（例如畫面重建），直接顯示

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
                    if (ok) buildAll() else msg.text = "分析失敗\n\n" + read("log.txt")
                }
            }
        }, "analyze", 64L * 1024 * 1024).start()
    }

    private fun read(name: String): String {
        val f = File(out, name)
        return if (f.exists()) f.readText().trim() else ""
    }

    // 某一頁的內容，去掉第一行的「==== 標題 ====」（標題已經另外顯示了）
    private fun pageText(n: Int): String {
        val lines = read("p$n.txt").lines()
        val rest = if (lines.isNotEmpty() && lines[0].contains("====")) lines.drop(1) else lines
        return rest.joinToString("\n").trim('\n')
    }

    // 整頁：型號 → 分類目錄 → 每個分類的標題和內容
    private fun buildAll() {
        box.removeAllViews()
        heads.clear(); bodies.clear(); names.clear(); states.clear()

        val head = TextView(this)
        head.textSize = 16f
        head.text = read("head.txt")
        box.addView(head)

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
            val warn = state.contains("注意")
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

            val body = TextView(this)
            body.typeface = Typeface.MONOSPACE
            body.textSize = 13f
            body.setTextIsSelectable(true)
            body.setPadding(dp(4), dp(8), dp(4), 0)
            body.text = pageText(i + 1)

            heads.add(h)
            bodies.add(body)
            setHead(i)
            h.setOnClickListener {
                body.visibility = if (body.visibility == View.VISIBLE) View.GONE else View.VISIBLE
                setHead(i)
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
                setHead(i)
                scroll.post { scroll.smoothScrollTo(0, h.top) }
            }
            chips.addView(c)
        }

        toggle.setOnClickListener {
            val anyOpen = bodies.any { it.visibility == View.VISIBLE }
            for (i in bodies.indices) {
                bodies[i].visibility = if (anyOpen) View.GONE else View.VISIBLE
                setHead(i)
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
        init {
            System.loadLibrary("ov")
        }
    }
}
