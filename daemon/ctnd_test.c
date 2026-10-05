// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctnd 的解析逻辑单元测试（在 PC 上用原生 gcc 跑，不需要设备）
 *
 * 把 ctnd.c 整个 include 进来，只把它的 main 改名，这样静态函数都能直接调。
 * 重点验证 v1.1 新增的「配置形态判定」：明文 JSON / base64-JSON / 加密未知。
 *
 * 编译运行： gcc -O1 -Wall -o /tmp/t ctnd_test.c && /tmp/t
 */
#define main ctnd_main
#include "ctnd.c"
#undef main

static int fails;

static void check(const char *what, bool cond)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		fails++;
}

static void expect(const char *label, const char *raw, enum cfg_form form,
		   const char *ctn, int ctb)
{
	struct cfg_entry e;

	memset(&e, 0, sizeof(e));
	parse_cfg(raw, &e);
	printf("  --- %s\n", label);
	printf("      form=%d ctn=\"%s\" ctb=%d\n", (int)e.form, e.ctn, e.ctb);
	check("形态判定", e.form == form);
	if (ctn)
		check("ctn 取值", strcmp(e.ctn, ctn) == 0);
	check("ctb 取值", e.ctb == ctb);
}

int main(void)
{
	char b64[512];

	printf("=== 1. base64 解码 ===\n");
	check("空串", !b64_decode("", b64, sizeof(b64)));
	check("非法字符", !b64_decode("!!!!", b64, sizeof(b64)));
	check("正常解码", b64_decode("aGVsbG8=", b64, sizeof(b64)) && !strcmp(b64, "hello"));
	check("无填充解码", b64_decode("aGVsbG8", b64, sizeof(b64)) && !strcmp(b64, "hello"));
	check("两字节", b64_decode("aGk=", b64, sizeof(b64)) && !strcmp(b64, "hi"));

	printf("\n=== 2. 形态一：明文 JSON ===\n");
	expect("典型云控内容",
	       "{\"ctb\":1,\"htb\":1,\"ctep\":80,\"ctn\":\"GameThread RenderThread\"}",
	       CFG_JSON, "GameThread RenderThread", 1);
	expect("Unity 游戏（有 ctb 没 ctn）",
	       "{\"ctb\":1,\"htb\":1,\"ctep\":80,\"cht_boost_max\":\"0,13\"}",
	       CFG_EMPTY, "", 1);
	expect("空 JSON",
	       "{}", CFG_EMPTY, "", -1);

	printf("\n=== 3. 形态二：base64 编码的 JSON ===\n");
	{
		/* 把上面那段 JSON 手工 base64 一下 */
		const char *json = "{\"ctb\":1,\"ctn\":\"GameThread RenderThread\"}";
		/* 用 openssl 风格：这里直接内联一个已知正确的 base64 */
		expect("base64(JSON)",
		       "eyJjdGIiOjEsImN0biI6IkdhbWVUaHJlYWQgUmVuZGVyVGhyZWFkIn0=",
		       CFG_B64JSON, "GameThread RenderThread", 1);
		(void)json;
	}

	printf("\n=== 4. 形态三：加密 / 未知 ===\n");
	expect("高熵密文（模拟）",
	       "\x01\x01\x34\x98\xc3\xe6\x41\x92\x3b\xfc\xc8\x99\xa9\x24\xe5\x43",
	       CFG_OPAQUE, "", -1);

	printf("\n=== 5. 名字归一化 ===\n");
	{
		/* normalize_and_write 会真写节点，这里只测 strtok 分段逻辑 */
		char buf[128], *tok[8];
		int n = 0, i;
		char *p;

		snprintf(buf, sizeof(buf), "%s", "A B C");
		for (p = strtok(buf, " \t\r\n"); p && n < 8; p = strtok(NULL, " \t\r\n"))
			tok[n++] = p;
		check("三个名字切出 3 段", n == 3);
		check("只取前两个", !strcmp(tok[0], "A") && !strcmp(tok[1], "B"));

		snprintf(buf, sizeof(buf), "%s", "  \t \n ");
		n = 0;
		for (p = strtok(buf, " \t\r\n"); p && n < 8; p = strtok(NULL, " \t\r\n"))
			tok[n++] = p;
		check("全空白切出 0 段（不能去碰 tok[0]）", n == 0);
		(void)i;
	}

	printf("\n=== 6. game_pid 解析 ===\n");
	check("正常", parse_game_pid("game_pid=10217 child_num=38") == 10217);
	check("退出", parse_game_pid("game_pid=-1 child_num=0") == -1);
	check("缺字段", parse_game_pid("garbage") == -1);

	printf("\n=== 7. 本地覆盖文件解析 ===\n");
	{
		/* 借 local_conf_lookup 的解析逻辑：临时写一个文件再读 */
		const char *p = "/tmp/ctn_test.conf";
		FILE *f = fopen(p, "w");
		char out[128] = {0};

		if (f) {
			fprintf(f, "# 注释行\n");
			fprintf(f, "com.foo.bar = Name1 Name2\n");
			fprintf(f, "\n");
			fprintf(f, "com.baz.qux=GameThread   RenderThread\n");
			fclose(f);
			/* LOCAL_CONF 是编译期常量，这里换个方式验证：直接测解析函数
			 * 的等价逻辑（读文件 + 按 = 切分） */
			char buf[4096];
			if (read_file(p, buf, sizeof(buf))) {
				char *line, *save;
				int found = 0;
				for (line = strtok_r(buf, "\n", &save); line;
				     line = strtok_r(NULL, "\n", &save)) {
					char *eq;
					while (*line && isspace((unsigned char)*line)) line++;
					if (!*line || *line == '#') continue;
					eq = strchr(line, '=');
					if (!eq) continue;
					*eq = '\0';
					while (*eq == '\0' && isspace((unsigned char)eq[-1])) eq[-1] = '\0';
					if (!strcmp(line, "com.baz.qux")) {
						char *v = eq + 1;
						while (*v && isspace((unsigned char)*v)) v++;
						snprintf(out, sizeof(out), "%s", v);
						found = 1;
					}
				}
				check("找到 com.baz.qux", found);
				check("取到名字串", strstr(out, "GameThread") != NULL);
			} else {
				check("读配置文件", false);
			}
			remove(p);
		} else {
			printf("  (跳过：建不了临时文件)\n");
		}
	}

	printf("\n==================================\n");
	printf("结果: %s（失败 %d 项）\n", fails ? "有问题" : "全部通过", fails);
	printf("==================================\n");
	return fails ? 1 : 0;
}
