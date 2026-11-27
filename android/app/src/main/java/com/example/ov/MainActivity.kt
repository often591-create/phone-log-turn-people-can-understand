package com.example.ov

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.example.ov.databinding.ActivityMainBinding
import java.io.File
import java.io.FilterInputStream
import java.io.InputStream
import java.util.zip.ZipInputStream

// 會記錄「已經讀了幾個位元組」的讀檔工具，進度條用
class CountingStream(s: InputStream) : FilterInputStream(s) {
    @Volatile var count = 0L
    override fun read(): Int {
        val r = super.read()
        if (r >= 0) count++
        return r
    }
    override fun read(b: ByteArray, off: Int, len: Int): Int {
        val r = super.read(b, off, len)
        if (r > 0) count += r
        return r
    }
    override fun skip(n: Long): Long {
        val r = super.skip(n)
        count += r
        return r
    }
}

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    @Volatile private var counter: CountingStream? = null

    // 打開系統的檔案選擇畫面，選完會回到這裡
    private val pickFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) prepare(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        CrashLog.install(this, "主畫面")

        // 上一次閃退的原因（有的話）
        CrashLog.lastCrash(this)?.let { msg ->
            val tv = android.widget.TextView(this)
            tv.setTextIsSelectable(true)
            tv.textSize = 12f
            tv.setPadding(48, 24, 48, 0)
            tv.text = msg
            val sv = android.widget.ScrollView(this)
            sv.addView(tv)
            androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle("上次閃退的原因（請截圖回報）")
                .setView(sv)
                .setPositiveButton("知道了", null)
                .show()
        }

        binding.pickButton.setOnClickListener {
            pickFile.launch(arrayOf("*/*"))
        }
    }

    // 把選到的檔案放進 App 自己的資料夾：zip 就解壓縮，txt 就複製，然後打開結果畫面
    private fun prepare(uri: Uri) {
        val (name, size) = fileInfo(uri)
        val isZip = name.lowercase().endsWith(".zip")
        val title = if (isZip) "正在解壓縮" else "正在複製"
        val start = System.currentTimeMillis()
        counter = null
        binding.pickButton.isEnabled = false
        binding.progress.progress = 0
        binding.progress.visibility = View.VISIBLE

        // 每 0.2 秒更新一次進度條和文字
        val ticker = object : Runnable {
            override fun run() {
                val done = (counter?.count ?: 0L).toDouble()
                binding.progress.progress = permille(done, size.toDouble())
                binding.sampleText.text = "你選了：$name\n\n" + progressText(title, done, size.toDouble(), start)
                binding.root.postDelayed(this, 200)
            }
        }
        binding.root.post(ticker)

        Thread {
            val path = try {
                val work = File(cacheDir, "work")
                work.deleteRecursively()          // 刪掉上一次的
                work.mkdirs()
                if (isZip) {
                    unzip(uri, File(work, "report"))
                    File(work, "report.zip").path    // C 程式看到同名資料夾已經存在，就直接用
                } else {
                    val f = File(work, "report.txt")
                    contentResolver.openInputStream(uri)!!.use { input ->
                        val cs = CountingStream(input)
                        counter = cs
                        f.outputStream().use { o -> cs.copyTo(o, 1 shl 16) }
                    }
                    f.path
                }
            } catch (e: Exception) {
                null
            }

            runOnUiThread {
                binding.root.removeCallbacks(ticker)
                binding.progress.visibility = View.GONE
                binding.pickButton.isEnabled = true
                if (path == null) {
                    binding.sampleText.text = "你選了：$name\n\n這個檔案打不開"
                } else {
                    binding.sampleText.text = "你選了：$name"
                    startActivity(Intent(this, ReportActivity::class.java).putExtra("path", path))
                }
            }
        }.start()
    }

    // 只解壓縮分析會用到的檔案，比較快、比較省空間
    private fun unzip(uri: Uri, dir: File) {
        val root = dir.canonicalPath + File.separator
        contentResolver.openInputStream(uri)!!.use { input ->
            val cs = CountingStream(input.buffered(1 shl 16))
            counter = cs
            ZipInputStream(cs).use { zip ->
                var e = zip.nextEntry
                while (e != null) {
                    val n = e.name
                    val want = (!n.contains('/') && n.endsWith(".txt")) ||
                            ((n.startsWith("FS/mnt/rescue/") || n.startsWith("FS/cache/recovery/") ||
                              n.startsWith("FS/data/anr/") || n.startsWith("FS/data/tombstones/")) && !n.endsWith(".pb"))
                    val f = File(dir, n)
                    if (want && !e.isDirectory && f.canonicalPath.startsWith(root)) {
                        f.parentFile?.mkdirs()
                        f.outputStream().use { zip.copyTo(it) }
                    }
                    e = zip.nextEntry
                }
            }
        }
    }

    // 取得檔案名稱和大小
    private fun fileInfo(uri: Uri): Pair<String, Long> {
        var name = "未知"
        var size = 0L
        contentResolver.query(uri, null, null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val n = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                val s = c.getColumnIndex(OpenableColumns.SIZE)
                if (n >= 0) name = c.getString(n) ?: name
                if (s >= 0 && !c.isNull(s)) size = c.getLong(s)
            }
        }
        return Pair(name, size)
    }
}
