#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# 读 ELF64 小端文件里某个 section 的大小。只用 od + awk（设备上没有 readelf）。
#
# 用途：设备上那份 oplus_bsp_game_opt.ko 是给那个内核编的，它的
#       .gnu.linkonce.this_module 段大小 == 那个内核的 sizeof(struct module)。
#       跟我们 .ko 的比一比，就知道 struct module 布局对不对 —— 这是唯一会
#       导致 insmod 崩机（而不是干净失败）的差异。
#
# 用法: sh elf_sec_size.sh <文件> <section名>
#   找到打印十进制大小（成功），找不到无输出（退出码 1）。
f="$1"; want="$2"
[ -f "$f" ] || exit 1

# --- ELF 头里取 e_shoff/e_shentsize/e_shnum/e_shstrndx（偏移 40 起 24 字节）---
# 注意 od 的输出会分多行，必须先攒齐再算（早期版本直接在 { } 里算，
# 第二行把数组覆盖了，结果全是 0）。窗口起点是文件偏移 40，所以：
#   e_shoff     在窗口内下标 0..7
#   e_shentsize 在文件偏移 58 → 窗口内下标 18..19 → 1-based 第 19/20 字节
#   e_shnum     文件偏移 60 → 第 21/22 字节
#   e_shstrndx  文件偏移 62 → 第 23/24 字节
set -- $(od -An -v -tu1 -j 40 -N 24 "$f" 2>/dev/null | awk '
	{ for (i = 1; i <= NF; i++) b[n++] = $i }
	END {
		shoff  = b[0] + b[1]*256 + b[2]*65536 + b[3]*16777216 \
		       + b[4]*4294967296 + b[5]*1099511627776
		esz    = b[18] + b[19]*256
		num    = b[20] + b[21]*256
		strndx = b[22] + b[23]*256
		print shoff, esz, num, strndx
	}')
shoff=$1; esz=$2; num=$3; strndx=$4
[ -n "$shoff" ] && [ "$num" -gt 0 ] 2>/dev/null || exit 1

# --- shstrtab 的 sh_offset / sh_size（section 头里 +24 / +32）---
stroff=$(od -An -v -tu1 -j $((shoff + strndx * esz + 24)) -N 8 "$f" 2>/dev/null \
	| awk '{ v = 0; for (i = 8; i >= 1; i--) v = v * 256 + $i; print v }')
strsz=$(od -An -v -tu1 -j $((shoff + strndx * esz + 32)) -N 8 "$f" 2>/dev/null \
	| awk '{ v = 0; for (i = 8; i >= 1; i--) v = v * 256 + $i; print v }')
[ -n "$stroff" ] && [ "$strsz" -gt 0 ] 2>/dev/null || exit 1

# 字符串表整块转成 hex 字符串传给 awk（section 名都是 ASCII）
strhex=$(od -An -v -tx1 -j "$stroff" -N "$strsz" "$f" 2>/dev/null | tr -d ' \n')

# --- 遍历 section 头，比对名字 ---
od -An -v -tu1 -j "$shoff" -N $((num * esz)) "$f" 2>/dev/null \
| awk -v want="$want" -v strhex="$strhex" -v esz="$esz" '
	{ for (i = 1; i <= NF; i++) b[n++] = $i }
	END {
		hex = "0123456789abcdef"
		for (s = 0; s * esz + 40 <= n; s++) {
			o = s * esz
			nameoff = b[o] + b[o+1]*256 + b[o+2]*65536 + b[o+3]*16777216
			name = ""
			for (p = nameoff; p * 2 + 2 <= length(strhex); p++) {
				hi = index(hex, substr(strhex, p*2 + 1, 1))
				lo = index(hex, substr(strhex, p*2 + 2, 1))
				if (hi == 0 || lo == 0) break
				c = (hi - 1) * 16 + (lo - 1)
				if (c == 0) break
				name = name sprintf("%c", c)
			}
			if (name == want) {
				print b[o+32] + b[o+33]*256 + b[o+34]*65536 + b[o+35]*16777216
				exit 0
			}
		}
		exit 1
	}'
