// SPDX-License-Identifier: GPL-2.0-only
/* parse 逻辑实测：把 ctn_patch.c 里的 is_space_char / parse_one_name / parse_names
 * 原样搬过来（只替换 copy_from_user → memcpy），用 gcc 在 WSL 里跑一组输入，
 * 看写入格式到底怎么判定。这就是设备上 write 路径的真实行为。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CT_NUM        2
#define CT_NAME_LEN   100
#define CT_INPUT_LEN  256

#define EINVAL 22
#define E2BIG  7
#define ENAMETOOLONG 36

static int is_space_char(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\v' || c == '\f';
}

static int parse_one_name(const char **cursor, char *out)
{
	const char *start;
	const char *p = *cursor;
	size_t len;

	while (*p && is_space_char(*p))
		p++;
	if (!*p)
		return -EINVAL;
	start = p;
	while (*p && !is_space_char(*p))
		p++;
	len = p - start;
	if (!len)
		return -EINVAL;
	if (len >= CT_NAME_LEN)
		return -ENAMETOOLONG;
	memcpy(out, start, len);
	out[len] = '\0';
	*cursor = p;
	return 0;
}

static int parse_names(const char *ubuf, size_t count, char out[CT_NUM][CT_NAME_LEN])
{
	char input[CT_INPUT_LEN];
	const char *cursor;
	int i;

	if (!count || count >= sizeof(input))
		return -E2BIG;
	memcpy(input, ubuf, count);
	if (memchr(input, '\0', count))
		return -EINVAL;
	input[count] = '\0';

	cursor = input;
	for (i = 0; i < CT_NUM; i++) {
		int ret = parse_one_name(&cursor, out[i]);

		if (ret)
			return ret;
	}
	while (*cursor && is_space_char(*cursor))
		cursor++;
	if (*cursor)
		return -EINVAL;
	return 0;
}

static void t(const char *label, const char *s)
{
	char out[CT_NUM][CT_NAME_LEN];
	int r = parse_names(s, strlen(s), out);

	if (r == 0)
		printf("  %-34s -> 接受   [0]=\"%s\"  [1]=\"%s\"\n", label, out[0], out[1]);
	else
		printf("  %-34s -> 拒绝 (-%d %s)\n", label, r,
		       r == EINVAL ? "EINVAL" : r == E2BIG ? "E2BIG" : "ENAMETOOLONG");
}

int main(void)
{
	printf("=== 写入格式实测（设备 write 路径的真实逻辑）===\n");
	t("UnityMain UnityGfxDevice", "UnityMain UnityGfxDevice");
	t("A B", "A B");
	t("A    B  (多空格)", "A    B  ");
	t("A\\tB  (制表符)", "A\tB");
	t("A\\nB  (换行)", "A\nB");
	printf("--- 只填一个线程 ---\n");
	t("onlyone", "onlyone");
	t("A (空格结尾)", "A ");
	t("A (换行结尾)", "A\n");
	t("(空)", "");
	printf("--- 填三个 ---\n");
	t("A B C", "A B C");
	printf("--- 边界长度 ---\n");
	t("15字符 15字符", "ABCDEFGHIJKLMNO ABCDEFGHIJKLMNO");
	t("16字符 15字符", "ABCDEFGHIJKLMNOP ABCDEFGHIJKLMNO");
	t("99字符 99字符", "000000000011111111112222222222333333333344444444445555555555666666666677777777778888888888999999999999 000000000011111111112222222222333333333344444444445555555555666666666677777777778888888888999999999999");
	t("100字符 1字符", "0000000000111111111122222222223333333333444444444455555555556666666666777777777788888888889999999999999 A");
	printf("--- 同名字写两次 ---\n");
	t("MyGame MyGame", "MyGame MyGame");
	return 0;
}
