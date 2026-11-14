/* SPDX-License-Identifier: GPL-2.0-only */
#include "../ctnd.h"
#include "test_helper.h"
#include <string.h>

static void expect(const char *raw, enum ctnd_result result, const char *names, int ctb)
{
	struct ctnd_cfg cfg;

	CHECK(ctnd_parse_cfg(raw, strlen(raw), &cfg) == result);
	CHECK(cfg.result == result);
	CHECK(!strcmp(cfg.names, names));
	CHECK(cfg.have_ctb == (ctb >= 0));
	if (ctb >= 0) CHECK(cfg.ctb == ctb);
}

int main(void)
{
	char out[512];
	size_t n;
	int pid;
	struct ctnd_cfg cfg;

	CHECK(ctnd_base64("aGVsbG8=", 8, out, sizeof(out), &n) && n == 5 && !strcmp(out, "hello"));
	CHECK(ctnd_base64("aGVsbG8", 7, out, sizeof(out), &n) && !strcmp(out, "hello"));
	CHECK(ctnd_base64("aGk=", 4, out, sizeof(out), &n) && !strcmp(out, "hi"));
	CHECK(!ctnd_base64("", 0, out, sizeof(out), &n));
	CHECK(!ctnd_base64("a", 1, out, sizeof(out), &n));
	CHECK(!ctnd_base64("aGk=garbage", 11, out, sizeof(out), &n));
	CHECK(!ctnd_base64("aGk===", 6, out, sizeof(out), &n));
	CHECK(!ctnd_base64("aGl=", 4, out, sizeof(out), &n));
	CHECK(!ctnd_base64("aGVsbG8=", 8, out, 3, &n));
	expect("{\"ctb\":1,\"ctn\":\"GameThread RenderThread\"}", CTND_VALID, "GameThread RenderThread", 1);
	expect("eyJjdGIiOjEsImN0biI6IkdhbWVUaHJlYWQgUmVuZGVyVGhyZWFkIn0=", CTND_VALID,
	       "GameThread RenderThread", 1);
	expect("{\"ctn\":\"Solo\"}", CTND_VALID, "Solo", -1);
	expect("{\"ctn\":\"Solo Solo\"}", CTND_VALID, "Solo", -1);
	expect("{\"note\":\"ctn\",\"ctn\":\"RealOne RealTwo\"}", CTND_VALID, "RealOne RealTwo", -1);
	expect("{\"nested\":{\"ctn\":\"Wrong\",\"ctb\":1}}", CTND_EMPTY, "", -1);
	expect("{\"nested\":[{\"ctn\":\"Wrong\"}],\"ctn\":\"Right\"}", CTND_VALID, "Right", -1);
	expect("{\"ct\\u006e\":\"Ga\\u006de Render\",\"ctb\":7}", CTND_VALID, "Game Render", 7);
	expect("{}", CTND_EMPTY, "", -1);
	expect("{\"ctb\":0}", CTND_EMPTY, "", 0);
	expect("{\"ctn\":\" \t\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\" \\t \\r\\n\"}", CTND_EMPTY, "", -1);
	expect("{\"ctn\":\"\"}", CTND_EMPTY, "", -1);
	expect("{\"ctn\":null}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":3}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\",\"ctn\":\"B\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctb\":1,\"ctb\":0}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\",\"ctb\":-1}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctb\":1.0}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctb\":2147483648}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A B C\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A:B\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\\u0000B\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\\uD800\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\\q\"}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"Unterminated}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\",}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\",\"x\":01}", CTND_UNSUPPORTED, "", -1);
	expect("{\"ctn\":\"A\"} trailing", CTND_UNSUPPORTED, "", -1);
	expect("prefix {\"ctn\":\"Wrong\"}", CTND_UNSUPPORTED, "", -1);
	expect("[]", CTND_UNSUPPORTED, "", -1);
	expect("opaque", CTND_UNSUPPORTED, "", -1);
	expect("", CTND_UNSUPPORTED, "", -1);
	CHECK(ctnd_parse_cfg("{}\0ignored", 10, &cfg) == CTND_UNSUPPORTED);
	memset(out, 'a', sizeof(out));
	out[99] = '\0';
	CHECK(ctnd_names(out, cfg.names, sizeof(cfg.names)));
	out[99] = 'a'; out[100] = '\0';
	CHECK(!ctnd_names(out, cfg.names, sizeof(cfg.names)));
	CHECK(ctnd_names("  A\tB  ", out, sizeof(out)) && !strcmp(out, "A B"));
	CHECK(!ctnd_names(" \r\n", out, sizeof(out)));
	CHECK(!ctnd_names("A B C", out, sizeof(out)));
	CHECK(!ctnd_names("ABCDE", out, 3));
	CHECK(ctnd_node_names("CustomOne:123,CustomTwo:-1\n", out, sizeof(out)) && !strcmp(out, "CustomOne CustomTwo"));
	CHECK(ctnd_node_names("Solo:-1,Solo:-1", out, sizeof(out)) && !strcmp(out, "Solo"));
	CHECK(!ctnd_node_names("A:bad,B:2", out, sizeof(out)));
	CHECK(!ctnd_node_names("A:1,B:2 junk", out, sizeof(out)));
	CHECK(ctnd_node_enable("0\n", out, sizeof(out)) && !strcmp(out, "0"));
	CHECK(ctnd_node_enable("ct_enable=1\n", out, sizeof(out)) && !strcmp(out, "1"));
	CHECK(!ctnd_node_enable("2", out, sizeof(out)));
	CHECK(!ctnd_node_enable("0garbage", out, sizeof(out)));
	CHECK(ctnd_game_pid("game_pid=123 child_num=100", &pid) && pid == 123);
	CHECK(ctnd_game_pid("game_pid=-1 child_num=0", &pid) && pid == -1);
	CHECK(!ctnd_game_pid("garbage", &pid));
	CHECK(!ctnd_game_pid("not_game_pid=3", &pid));
	CHECK(!ctnd_game_pid("game_pid=3x", &pid));
	CHECK(!ctnd_game_pid("game_pid=2147483648", &pid));
	CHECK(!ctnd_game_pid("game_pid=1 game_pid=-1", &pid));
	printf("PASS parser: %u checks\n", test_count);
	return 0;
}
