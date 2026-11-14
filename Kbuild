# SPDX-License-Identifier: GPL-2.0-only
# Per-profile build copies generate kernel/ctn_profile.h; no fallback profile.
ccflags-y += -I$(src)/kernel -include $(src)/kernel/ctn_profile.h
obj-m += ctn_patch.o
