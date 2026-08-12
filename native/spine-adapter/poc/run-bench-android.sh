#!/bin/bash
# 在已连接的 Android 设备/模拟器上运行 SpineRuntime realtime 压测。
# 前置：设备已连接 + USB 调试开启（或模拟器已启动）。
# 用法：bash poc/run-bench-android.sh
set -e

SDK="/c/Users/Administrator/AppData/Local/Android/Sdk"
ADB="$SDK/platform-tools/adb.exe"
BENCH="build-android/spine-runtime-bench"

echo "=== 1. 检查设备 ==="
"$ADB" devices
if ! "$ADB" get-state >/dev/null 2>&1; then
    echo "错误：没有检测到 Android 设备/模拟器。请连接设备并开启 USB 调试，或启动模拟器后重试。"
    exit 1
fi

echo "=== 2. 推送 bench + 资源到设备 ==="
"$ADB" push "$BENCH" /data/local/tmp/spine-runtime-bench
"$ADB" push poc/assets /data/local/tmp/poc/assets

echo "=== 3. 运行（在设备上）==="
"$ADB" shell "cd /data/local/tmp && chmod +x spine-runtime-bench && ./spine-runtime-bench"

echo "=== 完成 ==="
