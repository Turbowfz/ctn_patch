# SPDX-License-Identifier: GPL-2.0-only
# 所有构建均通过独立 profile 校验，禁止在源码根覆盖已发布的 KO。
PROFILE ?=
SRCDIR ?=
WORK ?=
CLANG ?= /opt/ndkroot/android-ndk-r26b/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
JOBS ?= 4

all:
	@test -n "$(PROFILE)" -a -n "$(SRCDIR)" -a -n "$(WORK)" || { printf '%s\n' '需要 PROFILE、SRCDIR、WORK；参见 README'; exit 2; }
	bash build.sh --profile "$(PROFILE)" --srcdir "$(SRCDIR)" --work "$(WORK)" --clang "$(CLANG)" --jobs "$(JOBS)"

test:
	bash daemon/test.sh
	python3 -I -B tests/test_packaging.py
	python3 -I -B tests/test_profile_tools.py

user:
	bash daemon/build.sh
	bash daemon/build_tools.sh

.PHONY: all test user
