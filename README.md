# 星米OV手機日誌分析

分析 Android 手機的錯誤報告（bugreport），把幾十萬行的紀錄整理成 12 頁看得懂的結果，類似 DevCheck。

名字由來：三**星**、小**米**、**O**PPO、**v**ivo。

## 支援的手機

程式會自動認出品牌，不用自己選。

| 品牌 | 系統 | 備註 |
|---|---|---|
| 小米 / Redmi | HyperOS | 含開機失敗、差點卡死、系統更新紀錄 |
| 三星 | One UI | 含開關機紀錄、Knox 保固狀態、沒電關機紀錄 |
| OPPO / OnePlus / realme | ColorOS / OxygenOS / realme UI | |
| vivo / iQOO | OriginOS / Funtouch OS | |
| 其他 Android | | 只會讀 Android 共通的部分 |

## 12 個分頁

總覽、系統、處理器與螢幕、零件與規格、記憶體、儲存空間、電池、網路、背景凍結、穩定性、錯誤訊息、歷史紀錄。

每一頁會標示 **[正常]** 或 **[注意]**，例如電池健康度低於 80%、安全性更新超過 90 天、App 閃退等。

## 怎麼取得 bugreport

手機開啟開發者選項 → **錯誤報告（Bug report）** → 選「完整報告」，等通知列出現後分享出 zip 檔。

> bugreport 裡有很多個人資料（App 清單、Wi-Fi 名稱、通話紀錄等），**請不要上傳到網路上**。這個專案的 `.gitignore` 已經排除 zip 和 txt 檔。

## 檔案

```
phonecheck.c        分析程式（電腦版和 Android 版共用同一份）
android/            Android App 的程式碼
```

`phonecheck.c` 裡的順序：共用工具 → Android 共通資料 → 小米 → 三星 → OPPO → vivo → 主程式。
每個品牌都有 4 個函式（`xxx_line`、`xxx_finish`、`xxx_history`、`xxx_history_page`），新增品牌時照著加就好。

## 電腦版（Windows）

1. 用 Dev-C++（TDM-GCC）開啟 `phonecheck.c` 編譯
2. 執行後選擇 bugreport 的 zip 或 txt，程式會用 Windows 內建的 `tar` 解壓縮
3. 用數字選單切換 12 個分頁

檔案是 UTF-8 編碼。Windows 繁體中文的命令提示字元預設是 Big5，中文會變亂碼，請先把檔案轉成 Big5（ANSI）再編譯。

## Android App

用 Android Studio 的 **Native C++** 範本建立專案（套件名稱 `com.example.ov`、語言 Kotlin、最低版本 Android 16），然後：

1. `phonecheck.c` 放進 `app/src/main/cpp/`，並照 `android/app/src/main/cpp/CMakeLists.txt` 加到 `add_library`
2. 用 `android/app/src/main/java/com/example/ov/` 裡的兩個檔案取代／新增 `MainActivity.kt`、`ReportActivity.kt`
3. 用 `android/app/src/main/res/layout/activity_main.xml` 取代原本的畫面檔
4. 把 `android/AndroidManifest-ReportActivity.xml` 的內容加到 `AndroidManifest.xml` 的 `</application>` 上面

App 的流程：選擇檢測報告 → 解壓縮（只解出分析需要的檔案）→ 分析 → 12 頁結果，兩個階段都有進度條、百分比、速度和剩餘時間。

分析在獨立的程序（`:report`）裡執行，離開結果畫面就結束，下一次分析會從乾淨的狀態開始。

要分享安裝檔時，請用 **Build → Generate App Bundles or APKs → Generate APKs** 產生的 APK。直接按執行（▶）產生的 APK 是測試用的，在別的手機上會安裝失敗。

## 限制

- 一般 App 沒有權限自己產生 bugreport，所以要先手動抓好報告
- 廠商改了系統的紀錄格式後，有些欄位可能會顯示「未記錄」
