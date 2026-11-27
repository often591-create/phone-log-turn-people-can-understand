#!/bin/bash
# 模擬器測試：裝 App → 放假報告 → 直接開分析畫面 → 看有沒有當掉
set -x
mkdir -p test-out
P=com.example.ov
adb install -r phonelog-analyzer.apk
adb root; sleep 3
adb logcat -c
# 1) 主畫面能不能開
adb shell am start -W -n $P/.MainActivity
sleep 5
adb exec-out screencap -p > test-out/1-main.png
# 2) 放假報告進 App 的資料夾，直接開分析畫面
adb push .github/test/sample.txt /data/local/tmp/sample.txt
D=/data/user/0/$P/cache/work
adb shell "mkdir -p $D && cp /data/local/tmp/sample.txt $D/report.txt && chown -R \$(stat -c %u /data/user/0/$P):\$(stat -c %g /data/user/0/$P) /data/user/0/$P/cache"
adb shell am start -W -n $P/.ReportActivity --es path $D/report.txt
sleep 25
adb exec-out screencap -p > test-out/2-report.png
# 3) 點「專業模式」（對話框第二項）
adb shell input keyevent KEYCODE_DPAD_DOWN; adb shell input keyevent KEYCODE_DPAD_DOWN; adb shell input keyevent KEYCODE_ENTER
sleep 5
adb exec-out screencap -p > test-out/3-pro.png
adb logcat -d > test-out/logcat.txt
adb logcat -d -b crash > test-out/crash.txt
adb shell ls -la /data/user/0/$P/cache/out > test-out/out-files.txt 2>&1
adb shell cat /data/user/0/$P/cache/out/log.txt >> test-out/out-files.txt 2>&1
cat test-out/crash.txt
grep -E "FATAL|Fatal signal|AndroidRuntime" test-out/logcat.txt | head -40
if [ -s test-out/crash.txt ]; then echo "有閃退"; exit 1; fi
adb shell pidof $P:report || echo "分析畫面的程序不在了"
exit 0
