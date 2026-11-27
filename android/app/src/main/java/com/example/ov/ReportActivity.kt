package com.example.ov

import android.graphics.Typeface
import android.os.Bundle
import android.widget.ProgressBar
import android.os.Process
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
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

/* 結果畫面：呼叫 C 程式分析，再顯示選單和每一頁 */
class ReportActivity : AppCompatActivity() {

    private lateinit var menu: LinearLayout   // 選單（12 個按鈕）
    private lateinit var page: TextView       // 某一頁的內容
    private lateinit var scroll: ScrollView
    private lateinit var out: File            // C 程式把結果寫在這個資料夾
    private var inPage = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()

        menu = LinearLayout(this)
        menu.orientation = LinearLayout.VERTICAL
        page = TextView(this)
        page.typeface = Typeface.MONOSPACE
        page.textSize = 13f
        page.setTextIsSelectable(true)
        page.visibility = View.GONE
        val box = LinearLayout(this)
        box.orientation = LinearLayout.VERTICAL
        box.setPadding(pad, pad, pad, pad)
        box.addView(menu)
        box.addView(page)
        scroll = ScrollView(this)
        scroll.fitsSystemWindows = true
        scroll.addView(box)
        setContentView(scroll)
        title = "檢測結果"

        // 返回鍵：在某一頁就回選單，在選單就離開
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (inPage) showMenu() else finish()
            }
        })

        out = File(cacheDir, "out")
        if (done) { buildMenu(); return }      // 已經分析過了（例如畫面重建），直接顯示

        val msg = TextView(this)
        msg.textSize = 16f
        msg.text = "正在準備..."
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal)
        bar.max = 1000
        menu.addView(bar)
        menu.addView(msg)

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
                    if (ok) buildMenu() else msg.text = "分析失敗\n\n" + read("log.txt")
                }
            }
        }, "analyze", 64L * 1024 * 1024).start()
    }

    private fun read(name: String): String {
        val f = File(out, name)
        return if (f.exists()) f.readText().trim() else ""
    }

    // 選單：最上面是型號，下面 12 個按鈕
    private fun buildMenu() {
        menu.removeAllViews()
        val head = TextView(this)
        head.textSize = 16f
        head.text = read("head.txt") + "\n"
        menu.addView(head)
        read("menu.txt").lines().forEachIndexed { i, line ->
            val parts = line.split("\t")
            val name = parts[0]
            val state = parts.getOrElse(1) { "" }
            val b = Button(this)
            b.text = if (state.isEmpty()) name else "$name    $state"
            b.setOnClickListener { showPage(i + 1, name) }
            menu.addView(b)
        }
    }

    private fun showPage(n: Int, name: String) {
        page.text = read("p$n.txt")
        menu.visibility = View.GONE
        page.visibility = View.VISIBLE
        title = name
        inPage = true
        scroll.scrollTo(0, 0)
    }

    private fun showMenu() {
        page.visibility = View.GONE
        menu.visibility = View.VISIBLE
        title = "檢測結果"
        inPage = false
        scroll.scrollTo(0, 0)
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
