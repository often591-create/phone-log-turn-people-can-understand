# 星米OV手機日誌分析

分析 Android 手機的錯誤報告（bugreport），把幾百萬行的紀錄整理成 22 個看得懂的分類，類似 DevCheck。

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

## 22 個分類

| 分類 | 內容 |
|---|---|
| 總覽 | 所有標示 [注意] 的項目 |
| 系統 | 型號、系統版本、安全性更新、開發者選項、出廠時的 Android 版本、核心與基頻版本、銷售地區、語言、加密 |
| 處理器與螢幕 | 處理器、頻率、休息時間、解析度、可用更新率、像素密度、HDR 格式 |
| 零件與規格 | 記憶體、儲存、螢幕面板、觸控、指紋、鏡頭感光元件、充電晶片、喇叭放大器、震動馬達、Wi-Fi 晶片 |
| 記憶體 | 總量、目前可用、虛擬記憶體用量、被特殊原因關掉的 App |
| 儲存空間 | 壽命、讀寫錯誤、空間用在哪裡（App、照片、影片…）、最佔空間的 10 個 App |
| 電池 | 健康度、充放電次數、抓取當下的電量 / 電壓 / 溫度、電壓範圍 |
| 充電紀錄 | 最近幾次充電（從幾 % 充到幾 %、多久、插座 / USB / 無線）、平均每小時掉多少電、掉電最快的時段 |
| 耗電分析 | 耗電排行、不讓手機休眠最久的元件、叫醒手機最多次的原因、用鬧鐘叫醒手機最多次的 App |
| 溫度 | 處理器、圖形晶片、電池、機身、USB 接口的溫度、發熱等級 |
| 網路 | Wi-Fi 訊號與速度、短暫斷線、目前的 Wi-Fi 規格與頻段、SIM 與行動網路 |
| 藍牙 | 狀態、裝置名稱、配對過的裝置 |
| 定位 (GPS) | 定位次數、首次定位速度、準確度、衛星訊號、雙頻定位、用到的衛星系統（不含座標） |
| 感測器 | 加速度計、陀螺儀、磁力計、光線、距離、氣壓計等的型號和廠商 |
| 音訊與震動 | 立體聲 / 單聲道、喇叭放大器晶片、震動馬達晶片 |
| 應用程式 | 安裝數量（自己裝的 / 系統內建）、最近安裝、最近更新、網路用量最多的 App |
| 使用時間 | 這一週每個 App 用了多久 |
| 權限使用 | 最近用過位置、相機、麥克風、剪貼簿、聯絡人、通話紀錄、簡訊的 App |
| 背景凍結 | Google Play 服務、LINE、Messenger、微信被凍結的時間 |
| 穩定性 | App 閃退、卡死、底層崩潰、系統崩潰 |
| 錯誤訊息 | E / F 等級的錯誤數量 |
| 歷史紀錄 | 系統更新、恢復原廠、當機、開機原因（要選 zip 才有） |

有判斷標準的分類會標示 **[正常]** 或 **[注意]**，例如電池健康度低於 80%、安全性更新超過 90 天、電池或機身超過 45°C、App 閃退等。

每支手機的報告內容不一樣，報告裡沒有的項目會顯示「未記錄」或不顯示。例如三星的報告沒有 App 使用時間、OPPO 的報告沒有鬧鐘統計。

不會顯示的個人資料：帳號、IMEI、Wi-Fi 名稱、定位座標、通知內容、聯絡人與通話內容。

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
3. 用數字選單切換 22 個分類

檔案是 UTF-8 編碼。Windows 繁體中文的命令提示字元預設是 Big5，中文會變亂碼，請先把檔案轉成 Big5（ANSI）再編譯。

## Android App

用 Android Studio 的 **Native C++** 範本建立專案（套件名稱 `com.example.ov`、語言 Kotlin、最低版本 Android 16），然後：

1. `phonecheck.c` 放進 `app/src/main/cpp/`，並照 `android/app/src/main/cpp/CMakeLists.txt` 加到 `add_library`
2. 用 `android/app/src/main/java/com/example/ov/` 裡的兩個檔案取代／新增 `MainActivity.kt`、`ReportActivity.kt`
3. 用 `android/app/src/main/res/layout/activity_main.xml` 取代原本的畫面檔
4. 把 `android/AndroidManifest-ReportActivity.xml` 的內容加到 `AndroidManifest.xml` 的 `</application>` 上面

App 的流程：選擇檢測報告 → 解壓縮（只解出分析需要的檔案）→ 分析 → 所有分類列在同一頁。解壓縮和分析都有進度條、百分比、速度和剩餘時間。

結果頁最上面有分類目錄，點一下會跳到那個分類；點分類標題可以收合或展開，也可以一次全部收合。

分析在獨立的程序（`:report`）裡執行，離開結果畫面就結束，下一次分析會從乾淨的狀態開始。

要分享安裝檔時，請用 **Build → Generate App Bundles or APKs → Generate APKs** 產生的 APK。直接按執行（▶）產生的 APK 是測試用的，在別的手機上會安裝失敗。

## 限制

- 一般 App 沒有權限自己產生 bugreport，所以要先手動抓好報告
- 廠商改了系統的紀錄格式後，有些欄位可能會顯示「未記錄」
