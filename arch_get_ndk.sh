#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 在 Arch(WSL) 里下载并解压 Android NDK r26b（内含 clang 17.0.2，与设备内核同版本）
#
# 安全要点（上一版踩过坑）：绝不做 `mv "$VAR"/*` 这种带未加保护变量的通配移动 ——
# 变量为空时它会展开成 /* 把整个根文件系统搬走。这里改为「解压到独立目录 + 直接用」，
# 不做任何移动。
set -uo pipefail

NDKROOT="/opt/ndkroot"          # 解压到这里，里面是 android-ndk-r26b/
NDKVER="r26b"
URL="https://dl.google.com/android/repository/android-ndk-${NDKVER}-linux.zip"
ZIP="/opt/ndk-${NDKVER}.zip"

CLANG="$NDKROOT/android-ndk-${NDKVER}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"

if [ -x "$CLANG" ]; then
	echo "NDK 已就绪：$CLANG"
	"$CLANG" --version | head -2
	exit 0
fi

echo "--- 下载 NDK ${NDKVER}（约 669MB）"
mkdir -p "$NDKROOT"
curl -L --retry 3 --connect-timeout 20 -o "$ZIP" "$URL" \
	-w "\nHTTP=%{http_code} 用时=%{time_total}s 速度=%{speed_download}B/s 大小=%{size_download}\n"

# 校验下载完整性：必须是 zip（PK 开头），且大小合理
SZ=$(stat -c%s "$ZIP" 2>/dev/null || echo 0)
echo "--- 下载大小 $SZ 字节"
if [ "$SZ" -lt 500000000 ]; then
	echo "!! 下载不完整（小于 500MB），中止"
	exit 1
fi
if [ "$(head -c2 "$ZIP")" != "PK" ]; then
	echo "!! 不是 zip 文件（可能下到了错误页），中止"
	exit 1
fi

echo "--- 解压到 $NDKROOT"
unzip -q -o "$ZIP" -d "$NDKROOT"
rm -f "$ZIP"

if [ ! -x "$CLANG" ]; then
	echo "!! 解压后找不到 clang：$CLANG"
	echo "   实际目录内容："
	ls -la "$NDKROOT" 2>/dev/null | head
	exit 1
fi

echo "--- 完成"
"$CLANG" --version | head -2
echo "NDK_CLANG=$CLANG"
