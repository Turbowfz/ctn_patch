# SPDX-License-Identifier: GPL-2.0-only
# ctn_patch 外部模块 Makefile
#
# 目标内核：6.1.141-android14-11-o-gdc1b6a03413f
#          （一加 Ace 3 Pro / 8Gen3 / sm8650，Oplus 6.1 分支）
#
# 用法（需要一份能用的内核构建产物，见 README.md）：
#   make KDIR=/path/to/kernel/out
#
# 交叉编译工具链：
#   Android GKI 用 prebuilt clang，例如
#   CLANG=/path/to/prebuilts/clang/host/linux-x86/clang-r487747c/bin/clang

KDIR ?= /lib/modules/$(shell uname -r)/build
CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH ?= arm64

# 若给出 CLANG 则走 LLVM=1 路径（Android 内核推荐）
ifdef CLANG
LLVM_ARG := LLVM=1
CC_ARG := CC=$(CLANG)
LD_ARG := LD=ld.lld
else
LLVM_ARG :=
CC_ARG :=
LD_ARG :=
endif

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) $(LLVM_ARG) $(CC_ARG) $(LD_ARG) \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) $(LLVM_ARG) $(CC_ARG) $(LD_ARG) \
		clean

install: all
	@echo "把 ctn_patch.ko push 到手机后："
	@echo "  adb push ctn_patch.ko /data/local/tmp/"
	@echo "  adb shell su -c 'insmod /data/local/tmp/ctn_patch.ko'"

.PHONY: all clean install
