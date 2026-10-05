#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Arch(WSL) 侧环境准备：pacman 换国内源 → 装内核模块构建依赖 → 报告 clang 版本
# 由 Windows 侧这样调起：
#   MSYS_NO_PATHCONV=1 wsl.exe -d archlinux -u root -- /bin/bash /mnt/c/.../arch_setup.sh
set -uo pipefail

MIRROR_PRIMARY="https://mirrors.tuna.tsinghua.edu.cn/archlinux"
MIRROR_BACKUP="https://mirrors.ustc.edu.cn/archlinux"

echo "############ 1. pacman 镜像 ############"
if ! grep -q "tuna.tsinghua\|ustc.edu.cn" /etc/pacman.d/mirrorlist 2>/dev/null; then
	cp -n /etc/pacman.d/mirrorlist /etc/pacman.d/mirrorlist.orig 2>/dev/null
	{
		echo "Server = ${MIRROR_PRIMARY}/\$repo/os/\$arch"
		echo "Server = ${MIRROR_BACKUP}/\$repo/os/\$arch"
		cat /etc/pacman.d/mirrorlist
	} > /tmp/ml && mv /tmp/ml /etc/pacman.d/mirrorlist
fi
head -3 /etc/pacman.d/mirrorlist | sed 's/^/    /'

# 开并行下载，快很多
if ! grep -q "^ParallelDownloads" /etc/pacman.conf; then
	sed -i 's/^#ParallelDownloads.*/ParallelDownloads = 5/' /etc/pacman.conf
	grep -q "^ParallelDownloads" /etc/pacman.conf || echo "ParallelDownloads = 5" >> /etc/pacman.conf
fi
grep "^ParallelDownloads" /etc/pacman.conf | sed 's/^/    /'

echo
echo "############ 2. keyring ############"
if [ ! -d /etc/pacman.d/gnupg ] || [ ! -f /etc/pacman.d/gnupg/pubring.gpg ]; then
	echo "--- 初始化 keyring（第一次要一会儿）"
	pacman-key --init
	pacman-key --populate archlinux
else
	echo "    keyring 已存在，跳过"
fi

echo
echo "############ 3. 同步数据库 ############"
pacman -Sy --noconfirm 2>&1 | tail -5

echo
echo "############ 4. 安装依赖 ############"
# 内核模块构建需要的东西：
#   make/gcc/binutils → base-devel（gcc 用来编主机侧工具 fixdep/genksyms/modpost）
#   bc bison flex openssl libelf perl cpio xz → Kbuild / 内核构建脚本
#   python git curl unzip → 杂项
#
# 注意：这里**故意不装 Arch 自带的 clang/llvm**（当前是 23.1.1，比设备内核的
# 17.0.2 高 6 个大版本）。设备内核开了 CONFIG_CFI_CLANG，模块回调的 kCFI 类型号
# 必须和内核一致，版本差太多有风险。我们直接用 NDK r26b 里的 clang 17.0.2
# （和设备同版本），工具链从那里出，所以不需要系统的 llvm。
DEPS=(base-devel bc bison flex openssl libelf perl cpio xz
      python git curl unzip rsync which)

echo "--- 装: ${DEPS[*]}"
DEBIAN_FRONTEND=noninteractive pacman -S --noconfirm --needed "${DEPS[@]}" 2>&1 | tail -25

echo
echo "############ 5. 工具链版本 ############"
echo "--- 主机 gcc（编内核的 host 工具用）"
gcc --version 2>&1 | head -1 | sed 's/^/    /'
for t in make gcc perl bc bison flex python3 git curl unzip; do
	printf "    %-16s %s\n" "$t" "$(command -v $t || echo '缺失!')"
done
echo "--- 交叉编译器将由 NDK 提供（arch_get_ndk.sh）"

echo
echo "############ 6. 和设备的编译器对比 ############"
DEV_VER="170002"   # 设备内核 CONFIG_CLANG_VERSION，即 clang 17.0.2
MY_VER="$(clang -dumpversion 2>/dev/null | tr -d '.')"
echo "    设备内核用 clang : 17.0.2 (170002)"
echo "    本机 clang       : $(clang --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [ "$MY_VER" = "$DEV_VER" ]; then
	echo "    [OK] 完全一致"
elif [ "${MY_VER:0:2}" = "17" ]; then
	echo "    [OK] 同为 LLVM 17 大版本，kCFI 类型号一致，可用"
else
	echo "    [注意] 大版本不同（设备 17，本机 ${MY_VER:0:2}）。"
	echo "           内核开了 CONFIG_WERROR=y，新版 clang 的新警告会变编译错误；"
	echo "           且 kCFI 类型号有版本差异风险。build.sh 会自动加 -Wno-error 兜住前者，"
	echo "           后者我会用设备自带的 .ko 做类型号比对来验证。"
fi

echo
echo "############ 7. 磁盘 / 内核头文件可用性 ############"
df -h / | tail -1 | sed 's/^/    /'
ls /usr/include/linux/kconfig.h >/dev/null 2>&1 && echo "    系统头文件在（不影响，我们只用内核源码里的头）"

echo
echo "############ 准备完成 ############"
