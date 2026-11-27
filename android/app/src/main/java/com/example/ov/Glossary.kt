package com.example.ov

import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.RelativeSizeSpan

// 原始資料的中文註解：常見英文詞 → 中文意思
// 每一行找到認得的詞，就在那行後面用灰藍色小字標出中文
object Glossary {

    private val words: Map<String, String> = linkedMapOf(
        // ---------- 電池與充電 ----------
        "battery" to "電池", "level" to "電量", "scale" to "滿格數值", "voltage" to "電壓",
        "temperature" to "溫度", "temp" to "溫度", "health" to "健康度", "status" to "狀態",
        "charging" to "充電中", "discharging" to "放電中", "charger" to "充電器", "charge" to "充電",
        "plugged" to "插著電", "unplugged" to "拔掉電", "AC powered" to "插座充電", "USB powered" to "USB 充電",
        "Wireless powered" to "無線充電", "Dock powered" to "底座充電", "present" to "有裝電池",
        "technology" to "電池材質", "capacity" to "容量", "cycle count" to "充電次數", "cycle_count" to "充電次數",
        "Max charging current" to "最大充電電流", "Max charging voltage" to "最大充電電壓",
        "Charge counter" to "剩餘電量 (微安時)", "current now" to "目前電流", "current_now" to "目前電流",
        "full" to "充滿", "power" to "電源 / 耗電", "Estimated power use" to "估計耗電",
        "mAh" to "毫安時 (電量單位)", "mA" to "毫安 (電流單位)", "mV" to "毫伏 (電壓單位)",
        "screen on" to "螢幕亮著", "screen off" to "螢幕關著", "screen" to "螢幕",
        "wake lock" to "不讓手機休眠", "wakelock" to "不讓手機休眠", "wake_lock" to "不讓手機休眠",
        "wakeup" to "喚醒", "Wakeup reason" to "被喚醒的原因", "doze" to "深度省電", "idle" to "閒置",
        "deviceidle" to "省電模式", "low_power" to "省電模式", "Power save" to "省電", "battery saver" to "省電模式",
        "whitelist" to "白名單 (不限制)", "standby" to "待機", "Battery History" to "電池歷史",

        // ---------- 記憶體與處理器 ----------
        "memory" to "記憶體", "RAM" to "記憶體", "Total RAM" to "記憶體總量", "Free RAM" to "可用記憶體",
        "Used RAM" to "已用記憶體", "Lost RAM" to "算不出用途的記憶體", "ZRAM" to "壓縮記憶體", "swap" to "虛擬記憶體",
        "MemTotal" to "記憶體總量", "MemFree" to "完全沒用的記憶體", "MemAvailable" to "可用記憶體",
        "Cached" to "快取", "Buffers" to "緩衝區", "SwapTotal" to "虛擬記憶體總量", "SwapFree" to "虛擬記憶體剩餘",
        "PSS" to "實際佔用記憶體", "RSS" to "佔用記憶體", "Native Heap" to "C 程式用的記憶體",
        "Dalvik Heap" to "Java 程式用的記憶體", "Graphics" to "圖形記憶體", "heap" to "程式記憶體",
        "lowmemorykiller" to "記憶體不足清程式", "lmkd" to "記憶體不足清程式", "oom" to "記憶體不足",
        "OutOfMemoryError" to "記憶體不足錯誤", "cpu" to "處理器", "CPU usage" to "處理器使用率", "Load" to "負載",
        "freq" to "頻率", "frequency" to "頻率", "governor" to "調頻方式", "cores" to "核心", "core" to "核心",
        "user" to "使用者 / 一般程式", "kernel" to "核心 (系統底層)", "iowait" to "等待讀寫", "irq" to "硬體中斷",
        "softirq" to "軟體中斷", "thermal" to "溫度控制", "throttling" to "降頻 (太熱)", "throttle" to "降頻",
        "Cached temperatures" to "目前各處溫度", "skin" to "機身表面", "SKIN" to "機身表面", "BATTERY" to "電池",
        "GPU" to "顯示晶片", "NPU" to "AI 晶片", "soc" to "處理器晶片", "uptime" to "開機多久",

        // ---------- 儲存空間 ----------
        "storage" to "儲存空間", "Data-Free" to "剩餘空間", "free" to "剩餘", "used" to "已使用", "total" to "總共",
        "available" to "可用", "disk" to "磁碟", "mount" to "掛載", "mounted" to "已掛載", "filesystem" to "檔案系統",
        "ufs" to "儲存晶片", "emmc" to "儲存晶片", "fstrim" to "整理儲存空間", "Cache-Free" to "快取剩餘",
        "App Size" to "App 大小", "App Data Size" to "App 資料大小", "App Cache Size" to "App 快取大小",
        "Photos Size" to "照片大小", "Videos Size" to "影片大小", "Audio Size" to "音樂大小",
        "Downloads Size" to "下載大小", "System Size" to "系統大小", "Other Size" to "其他大小",
        "read" to "讀取", "write" to "寫入", "written" to "寫入", "bytes" to "位元組",

        // ---------- 網路 ----------
        "network" to "網路", "wifi" to "Wi-Fi", "mobile" to "行動網路", "cellular" to "行動網路",
        "signal" to "訊號", "signal strength" to "訊號強度", "rssi" to "訊號強度", "dBm" to "訊號強度單位 (越接近 0 越好)",
        "connected" to "已連線", "disconnected" to "已斷線", "connecting" to "連線中", "connectivity" to "網路連線",
        "LTE" to "4G", "NR" to "5G", "5G" to "5G", "roaming" to "漫遊", "carrier" to "電信業者",
        "rx" to "下載", "tx" to "上傳", "rxBytes" to "下載量", "txBytes" to "上傳量", "rxPackets" to "下載封包",
        "txPackets" to "上傳封包", "uid" to "App 編號", "vpn" to "VPN", "dns" to "網址查詢 (DNS)",
        "Private DNS" to "加密 DNS", "ip" to "IP 位址", "proxy" to "代理伺服器", "airplane" to "飛航模式",
        "hotspot" to "熱點", "tethering" to "網路分享", "ping" to "連線測試", "latency" to "延遲",
        "bandwidth" to "頻寬", "link speed" to "連線速度", "frequency band" to "頻段",

        // ---------- 藍牙 / 定位 / 感測器 ----------
        "bluetooth" to "藍牙", "bonded" to "已配對", "paired" to "已配對", "a2dp" to "藍牙音樂",
        "hfp" to "藍牙通話", "headset" to "耳機", "codec" to "編碼 (音質格式)", "LDAC" to "高音質藍牙格式",
        "AAC" to "藍牙音質格式 AAC", "SBC" to "基本藍牙音質格式", "aptX" to "高通藍牙音質格式",
        "location" to "定位", "gps" to "GPS 定位", "gnss" to "衛星定位", "satellite" to "衛星", "satellites" to "衛星",
        "fix" to "定位成功", "TTFF" to "第一次定位花的時間", "accuracy" to "精準度", "cn0" to "衛星訊號強度",
        "sensor" to "感測器", "accelerometer" to "加速度計 (偵測移動)", "gyroscope" to "陀螺儀 (偵測旋轉)",
        "magnetometer" to "磁力計 (指南針)", "proximity" to "距離感測 (講電話關螢幕)", "light" to "光線感測",
        "pressure" to "氣壓計", "step" to "計步", "step counter" to "計步器", "hall" to "霍爾感測 (保護套)",
        "vendor" to "廠商", "rate" to "頻率", "wake-up" to "會喚醒手機",

        // ---------- 螢幕 / 音訊 / 相機 ----------
        "display" to "螢幕", "brightness" to "亮度", "refresh rate" to "更新率", "fps" to "每秒畫面數",
        "resolution" to "解析度", "density" to "像素密度", "dpi" to "像素密度", "hdr" to "高動態範圍 (HDR)",
        "jank" to "卡頓", "Janky frames" to "卡頓的畫面", "frames" to "畫面", "Total frames rendered" to "總共畫的畫面",
        "vsync" to "畫面同步", "SurfaceFlinger" to "畫面合成", "window" to "視窗", "orientation" to "方向",
        "rotation" to "旋轉", "touch" to "觸控", "input" to "輸入", "keyboard" to "鍵盤", "night mode" to "深色模式",
        "audio" to "音訊", "volume" to "音量", "speaker" to "喇叭", "earpiece" to "聽筒", "microphone" to "麥克風",
        "mic" to "麥克風", "stream" to "聲音類型", "music" to "音樂", "ring" to "鈴聲", "alarm" to "鬧鐘",
        "notification" to "通知", "vibrator" to "震動馬達", "vibration" to "震動", "haptic" to "觸覺回饋",
        "amplifier" to "擴大機晶片", "smartpa" to "喇叭擴大晶片", "camera" to "相機", "lens" to "鏡頭",
        "facing" to "朝向", "front" to "前", "back" to "後", "flash" to "閃光燈", "torch" to "手電筒",
        "CONNECT" to "打開相機", "DISCONNECT" to "關閉相機",

        // ---------- App 與系統 ----------
        "package" to "App 套件", "packages" to "App 套件", "app" to "App", "apps" to "App", "versionName" to "版本",
        "versionCode" to "版本號", "firstInstallTime" to "第一次安裝", "lastUpdateTime" to "最後更新",
        "installer" to "安裝來源", "installerPackageName" to "安裝來源", "enabled" to "開啟", "disabled" to "關閉",
        "permission" to "權限", "permissions" to "權限", "granted" to "已允許", "denied" to "已拒絕",
        "allow" to "允許", "ignore" to "忽略", "deny" to "拒絕", "foreground" to "前景 (正在用)",
        "background" to "背景", "service" to "服務 (背景程式)", "activity" to "畫面", "process" to "程式",
        "pid" to "程式編號", "thread" to "執行緒", "tid" to "執行緒編號", "broadcast" to "系統廣播",
        "receiver" to "接收者", "provider" to "資料提供者", "job" to "排程工作", "jobs" to "排程工作",
        "alarm" to "鬧鐘 / 定時", "sync" to "同步", "account" to "帳號", "usage" to "使用", "launch" to "啟動",
        "launcher" to "桌面", "system" to "系統", "settings" to "設定", "global" to "全域設定",
        "secure" to "安全設定", "default" to "預設", "value" to "值", "name" to "名稱", "time" to "時間",
        "duration" to "持續時間", "count" to "次數", "timestamp" to "時間戳記", "elapsed" to "經過時間",
        "last" to "最後一次", "first" to "第一次", "start" to "開始", "end" to "結束", "stop" to "停止",
        "started" to "已開始", "stopped" to "已停止", "running" to "執行中", "killed" to "被關掉",
        "kill" to "關掉", "died" to "掛掉", "restart" to "重新啟動", "boot" to "開機", "reboot" to "重新開機",
        "shutdown" to "關機", "update" to "更新", "upgrade" to "升級", "version" to "版本", "build" to "系統版本",
        "fingerprint" to "指紋 / 系統識別碼", "model" to "型號", "brand" to "品牌", "manufacturer" to "製造商",
        "device" to "裝置", "board" to "主機板", "hardware" to "硬體", "serial" to "序號", "sdk" to "安卓版本號",
        "security patch" to "安全性更新", "bootloader" to "開機程式", "verifiedbootstate" to "開機驗證狀態",
        "locked" to "已上鎖", "unlocked" to "已解鎖", "root" to "最高權限", "selinux" to "系統安全機制",
        "enforcing" to "安全機制開啟中", "permissive" to "安全機制關閉 (不安全)", "encryption" to "加密",
        "encrypted" to "已加密", "keyguard" to "鎖定畫面", "lock screen" to "鎖定畫面", "credential" to "密碼",
        "PIN" to "數字密碼", "PASSWORD" to "密碼", "PATTERN" to "圖形密碼", "biometric" to "生物辨識",
        "face" to "臉部", "accessibility" to "無障礙", "device admin" to "裝置管理", "admin" to "管理員",
        "nfc" to "NFC 感應", "usb" to "USB", "adb" to "電腦除錯連線", "developer" to "開發人員",
        "debug" to "除錯", "dumpsys" to "系統狀態傾印", "dumpstate" to "系統報告", "bugreport" to "問題報告",
        "property" to "系統參數", "properties" to "系統參數", "getprop" to "讀系統參數",

        // ---------- 錯誤與狀態 ----------
        "error" to "錯誤", "errors" to "錯誤", "fail" to "失敗", "failed" to "失敗", "failure" to "失敗",
        "warning" to "警告", "warn" to "警告", "fatal" to "嚴重錯誤", "crash" to "當掉", "crashed" to "當掉",
        "exception" to "程式例外 (出錯)", "Exception" to "程式例外 (出錯)", "ANR" to "App 沒回應",
        "not responding" to "沒有回應", "timeout" to "逾時", "timed out" to "逾時", "tombstone" to "底層當掉紀錄",
        "signal 11" to "記憶體存取錯誤", "SIGSEGV" to "記憶體存取錯誤", "SIGABRT" to "程式自己中止",
        "abort" to "中止", "null" to "空的 (沒有值)", "invalid" to "無效", "unknown" to "不明",
        "success" to "成功", "ok" to "正常", "good" to "良好", "true" to "是", "false" to "否",
        "on" to "開", "off" to "關", "yes" to "是", "no" to "否", "none" to "無", "active" to "使用中",
        "inactive" to "未使用", "pending" to "等待中", "blocked" to "被擋住", "denial" to "被拒絕",
        "avc" to "安全機制攔截", "watchdog" to "系統看門狗 (卡住偵測)", "dropbox" to "系統錯誤箱",
        "stack" to "程式呼叫順序", "backtrace" to "當掉時的程式位置", "at" to "在"
    )

    private val lookup: Map<String, String> = words.mapKeys { it.key.lowercase() }

    // 一個大的比對規則：前後不能接英文字母或數字（避免 "on" 對到 "phone"）
    // 注意：Android 的正規表示式裡 [ 後面不能直接接 :
    private val re: Regex by lazy {
        val alts = words.keys.sortedByDescending { it.length }.joinToString("|") { k ->
            k.replace(".", "\\.").replace("-", "\\-").replace(" ", "[ _]")
        }
        Regex("(?<![A-Za-z0-9])($alts)(?![A-Za-z0-9])", RegexOption.IGNORE_CASE)
    }

    // 太常見、單獨出現沒意義的詞：不標
    private val skipAlone = setOf("at", "on", "off", "no", "yes", "ok", "true", "false", "name", "value",
        "time", "count", "first", "last", "start", "end", "read", "write", "user", "system", "app", "apps",
        "light", "back", "front", "fix", "step", "ring", "full", "free", "total", "used", "rate", "null", "none",
        "default", "input", "window", "process", "thread", "update", "build", "device", "core", "cores", "stream")

    // logcat 每行的等級：11-27 12:00:00.000  1000  1000 E Tag: ...
    private val logLevel = Regex("^\\d\\d-\\d\\d \\d\\d\\d\\d\\d\\d\\.\\d+\\s+(?:\\S+\\s+){2,3}([VDIWEF]) ")
    private val levelCn = mapOf("V" to "詳細", "D" to "除錯", "I" to "資訊", "W" to "警告", "E" to "錯誤", "F" to "嚴重錯誤")

    private const val NOTE_COLOR = 0xFF1A73E8.toInt()
    private const val BAD_COLOR = 0xFFB3261E.toInt()

    // 把一段原文加上中文註解
    fun annotate(text: String): CharSequence {
        val sb = SpannableStringBuilder()
        for (line in text.split('\n')) {
            sb.append(line)
            if (line.isNotBlank() && line.length < 600) {
                val notes = ArrayList<String>()
                var bad = false
                logLevel.find(line.replace(":", ""))?.let { m ->
                    val lv = m.groupValues[1]
                    if (lv == "E" || lv == "F" || lv == "W") {   // 只標警告和錯誤，一般訊息不標
                        notes.add(levelCn[lv] ?: lv)
                        bad = true
                    }
                }
                val seen = HashSet<String>()
                for (m in re.findAll(line)) {
                    val key = m.value.lowercase().replace('_', ' ')
                    val cn = lookup[key] ?: lookup[m.value.lowercase()] ?: continue
                    if (!seen.add(cn)) continue
                    if (key in skipAlone) continue
                    notes.add("${m.value}=$cn")
                    if (notes.size >= 5) break
                }
                if (notes.isNotEmpty()) {
                    val s = sb.length
                    sb.append("   ← ").append(notes.joinToString("、"))
                    sb.setSpan(ForegroundColorSpan(if (bad) BAD_COLOR else NOTE_COLOR), s, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                    sb.setSpan(RelativeSizeSpan(0.95f), s, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
            }
            sb.append('\n')
        }
        return sb
    }
}
