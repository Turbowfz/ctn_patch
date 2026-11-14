/* SPDX-License-Identifier: GPL-2.0-only */
#include "ctnd.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int b64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

bool ctnd_base64(const char *in, size_t len, char *out, size_t cap, size_t *written)
{
	unsigned int acc = 0, bits = 0;
	size_t i, n = 0, digits = 0, pads = 0;
	bool padded = false;

	if (!in || !out || !cap || !written)
		return false;
	*written = 0;
	for (i = 0; i < len; i++) {
		int v;
		unsigned char c = (unsigned char)in[i];

		if (space(c))
			continue;
		if (c == '=') {
			padded = true;
			if (++pads > 2)
				return false;
			continue;
		}
		v = b64_value(c);
		if (v < 0 || padded)
			return false;
		acc = (acc << 6) | (unsigned int)v;
		bits += 6;
		digits++;
		if (bits >= 8) {
			bits -= 8;
			if (n + 1 >= cap)
				return false;
			out[n++] = (char)((acc >> bits) & 255U);
		}
		acc &= (1U << bits) - 1U;
	}
	if (!digits || digits % 4 == 1 || acc ||
	    (pads && (digits + pads) % 4 != 0) ||
	    (pads == 1 && digits % 4 != 3) ||
	    (pads == 2 && digits % 4 != 2))
		return false;
	out[n] = '\0';
	*written = n;
	return true;
}

bool ctnd_names(const char *in, char *out, size_t cap)
{
	char names[2][CTND_NAME_LEN];
	size_t lengths[2] = {0, 0}, n = 0, total;
	const unsigned char *p = (const unsigned char *)in;

	if (!in || !out || !cap)
		return false;
	while (*p) {
		while (space(*p)) p++;
		if (!*p) break;
		if (n == 2) return false;
		while (*p && !space(*p)) {
			if (*p < 32 || *p == 127 || *p == ':' || *p == ',' ||
			    lengths[n] + 1 >= CTND_NAME_LEN)
				return false;
			names[n][lengths[n]++] = (char)*p++;
		}
		names[n][lengths[n]] = '\0';
		n++;
	}
	if (!n) return false;
	if (n == 2 && !strcmp(names[0], names[1])) n = 1;
	total = lengths[0] + (n == 2 ? lengths[1] + 1 : 0);
	if (total + 1 > cap) return false;
	memcpy(out, names[0], lengths[0]);
	if (n == 2) {
		out[lengths[0]] = ' ';
		memcpy(out + lengths[0] + 1, names[1], lengths[1]);
	}
	out[total] = '\0';
	return true;
}

static bool integer(const char *p, const char **end, int *out)
{
	char *q;
	long v;

	if (!p || !(*p == '-' || (*p >= '0' && *p <= '9')))
		return false;
	errno = 0;
	v = strtol(p, &q, 10);
	if (q == p || errno || v < INT_MIN || v > INT_MAX)
		return false;
	*end = q;
	*out = (int)v;
	return true;
}

bool ctnd_node_names(const char *in, char *out, size_t cap)
{
	char buf[512], names[CTND_NAMES_LEN];
	char *colon, *comma, *second, *last;
	const char *end;
	int pid;
	size_t a, b;

	if (!in || strlen(in) >= sizeof(buf)) return false;
	memcpy(buf, in, strlen(in) + 1);
	colon = strchr(buf, ':');
	comma = strchr(buf, ',');
	if (!colon || !comma || colon >= comma) return false;
	*colon = '\0';
	if (!integer(colon + 1, &end, &pid) || end != comma) return false;
	second = comma + 1;
	last = strchr(second, ':');
	if (!last) return false;
	*last = '\0';
	if (!integer(last + 1, &end, &pid)) return false;
	while (space((unsigned char)*end)) end++;
	if (*end) return false;
	a = strlen(buf);
	b = strlen(second);
	if (!a || !b || a >= CTND_NAME_LEN || b >= CTND_NAME_LEN ||
	    a + b + 2 > sizeof(names)) return false;
	memcpy(names, buf, a);
	names[a] = ' ';
	memcpy(names + a + 1, second, b + 1);
	return ctnd_names(names, out, cap);
}

bool ctnd_node_enable(const char *in, char *out, size_t cap)
{
	const char *end;
	int v;

	if (!in || cap < 2) return false;
	while (space((unsigned char)*in)) in++;
	if (!strncmp(in, "ct_enable=", 10)) in += 10;
	if (!integer(in, &end, &v) || (v != 0 && v != 1)) return false;
	while (space((unsigned char)*end)) end++;
	if (*end) return false;
	out[0] = v ? '1' : '0';
	out[1] = '\0';
	return true;
}

bool ctnd_game_pid(const char *text, int *pid)
{
	bool found = false;

	if (!text || !pid) return false;
	while (*text) {
		const char *end;
		int v;

		while (space((unsigned char)*text)) text++;
		if (!*text) break;
		if (!strncmp(text, "game_pid=", 9)) {
			if (found || !integer(text + 9, &end, &v) || v < -1 ||
			    (*end && !space((unsigned char)*end))) return false;
			*pid = v;
			found = true;
			text = end;
		} else {
			while (*text && !space((unsigned char)*text)) text++;
		}
	}
	return found;
}

struct json {
	const unsigned char *p, *end;
};

static void ws(struct json *j)
{
	while (j->p < j->end && space(*j->p)) j->p++;
}

static bool take(struct json *j, unsigned char c)
{
	ws(j);
	if (j->p == j->end || *j->p != c) return false;
	j->p++;
	return true;
}

static bool hex4(struct json *j, unsigned int *value)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		unsigned char c;
		unsigned int digit;

		if (j->p == j->end) return false;
		c = *j->p++;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10U;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10U;
		else return false;
		v = v * 16U + digit;
	}
	*value = v;
	return true;
}

static bool utf8(struct json *j, unsigned int *cp)
{
	unsigned int c = *j->p++, v, low;
	int n, i;

	if (c < 128) { *cp = c; return true; }
	if (c >= 0xc2 && c <= 0xdf) { n = 1; v = c & 31U; low = 0x80; }
	else if (c >= 0xe0 && c <= 0xef) { n = 2; v = c & 15U; low = 0x800; }
	else if (c >= 0xf0 && c <= 0xf4) { n = 3; v = c & 7U; low = 0x10000; }
	else return false;
	for (i = 0; i < n; i++) {
		if (j->p == j->end || (*j->p & 0xc0) != 0x80) return false;
		v = (v << 6) | (*j->p++ & 63U);
	}
	if (v < low || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
	*cp = v;
	return true;
}

static void put_cp(unsigned int cp, char *out, size_t cap, size_t *len)
{
	unsigned char bytes[4];
	size_t n, i;

	if (cp < 0x80) { bytes[0] = (unsigned char)cp; n = 1; }
	else if (cp < 0x800) {
		bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
		bytes[1] = (unsigned char)(0x80 | (cp & 63)); n = 2;
	} else if (cp < 0x10000) {
		bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
		bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
		bytes[2] = (unsigned char)(0x80 | (cp & 63)); n = 3;
	} else {
		bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
		bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
		bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 63));
		bytes[3] = (unsigned char)(0x80 | (cp & 63)); n = 4;
	}
	for (i = 0; i < n; i++) {
		if (out && *len + 1 < cap) out[*len] = (char)bytes[i];
		(*len)++;
	}
}

static bool string(struct json *j, char *out, size_t cap, bool *fits)
{
	size_t len = 0;

	if (!take(j, '"')) return false;
	while (j->p < j->end) {
		unsigned int cp;

		if (*j->p == '"') {
			j->p++;
			if (out && cap) out[len < cap ? len : cap - 1] = '\0';
			if (fits) *fits = !out || len < cap;
			return true;
		}
		if (*j->p < 32) return false;
		if (*j->p == '\\') {
			j->p++;
			if (j->p == j->end) return false;
			switch (*j->p++) {
			case '"': cp = '"'; break;
			case '\\': cp = '\\'; break;
			case '/': cp = '/'; break;
			case 'b': cp = '\b'; break;
			case 'f': cp = '\f'; break;
			case 'n': cp = '\n'; break;
			case 'r': cp = '\r'; break;
			case 't': cp = '\t'; break;
			case 'u': {
				unsigned int low;

				if (!hex4(j, &cp)) return false;
				if (cp >= 0xd800 && cp <= 0xdbff) {
					if (j->end - j->p < 2 || j->p[0] != '\\' || j->p[1] != 'u')
						return false;
					j->p += 2;
					if (!hex4(j, &low) || low < 0xdc00 || low > 0xdfff) return false;
					cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
				} else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
				/* NUL cannot be represented in the node protocol or our keys. */
				if (!cp) return false;
				break;
			}
			default: return false;
			}
		} else if (!utf8(j, &cp)) return false;
		put_cp(cp, out, cap, &len);
	}
	return false;
}

static bool number(struct json *j)
{
	const unsigned char *start = j->p;

	if (j->p < j->end && *j->p == '-') j->p++;
	if (j->p == j->end) return false;
	if (*j->p == '0') j->p++;
	else {
		if (*j->p < '1' || *j->p > '9') return false;
		do { j->p++; } while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
	}
	if (j->p < j->end && *j->p == '.') {
		j->p++;
		if (j->p == j->end || *j->p < '0' || *j->p > '9') return false;
		do { j->p++; } while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
	}
	if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
		j->p++;
		if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
		if (j->p == j->end || *j->p < '0' || *j->p > '9') return false;
		do { j->p++; } while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
	}
	return j->p > start;
}

static bool value(struct json *j, unsigned int depth)
{
	unsigned char open, close;
	static const char *const literals[] = {"true", "false", "null"};
	size_t i;

	if (depth > 32) return false;
	ws(j);
	if (j->p == j->end) return false;
	if (*j->p == '"') return string(j, NULL, 0, NULL);
	if (*j->p == '{' || *j->p == '[') {
		open = *j->p++;
		close = open == '{' ? '}' : ']';
		ws(j);
		if (j->p < j->end && *j->p == close) { j->p++; return true; }
		for (;;) {
			if (open == '{' && (!string(j, NULL, 0, NULL) || !take(j, ':'))) return false;
			if (!value(j, depth + 1)) return false;
			if (take(j, close)) return true;
			if (!take(j, ',')) return false;
		}
	}
	for (i = 0; i < sizeof(literals) / sizeof(literals[0]); i++) {
		size_t n = strlen(literals[i]);

		if ((size_t)(j->end - j->p) >= n && !memcmp(j->p, literals[i], n)) {
			j->p += n;
			return true;
		}
	}
	return number(j);
}

static bool object(struct json *j, struct ctnd_cfg *cfg)
{
	bool seen_names = false, seen_ctb = false;

	if (!take(j, '{')) return false;
	if (take(j, '}')) return true;
	for (;;) {
		char key[16];
		bool fits;

		if (!string(j, key, sizeof(key), &fits) || !take(j, ':')) return false;
		ws(j);
		if (fits && !strcmp(key, "ctn")) {
			char names[512];
			const char *p;

			if (seen_names || !string(j, names, sizeof(names), &fits) || !fits) return false;
			seen_names = true;
			p = names;
			while (space((unsigned char)*p)) p++;
			if (*p && !ctnd_names(names, cfg->names, sizeof(cfg->names))) return false;
		} else if (fits && !strcmp(key, "ctb")) {
			const unsigned char *start = j->p, *p;
			unsigned int v = 0;

			if (seen_ctb || !number(j)) return false;
			seen_ctb = true;
			for (p = start; p < j->p; p++) {
				if (*p < '0' || *p > '9' || v > (unsigned int)INT_MAX / 10U ||
				    (v == (unsigned int)INT_MAX / 10U && *p - '0' > INT_MAX % 10))
					return false;
				v = v * 10U + (unsigned int)(*p - '0');
			}
			cfg->have_ctb = true;
			cfg->ctb = (int)v;
		} else if (!value(j, 1)) return false;
		if (take(j, '}')) return true;
		if (!take(j, ',')) return false;
	}
}

enum ctnd_result ctnd_parse_cfg(const char *raw, size_t len, struct ctnd_cfg *cfg)
{
	struct json j;
	struct ctnd_cfg parsed = {0};
	char *decoded = NULL;
	const char *src = raw;
	size_t n = len, i = 0;
	bool ok;

	memset(cfg, 0, sizeof(*cfg));
	cfg->result = CTND_UNSUPPORTED;
	if (!raw || !len || len > CTND_RAW_MAX || memchr(raw, '\0', len)) return cfg->result;
	while (i < len && space((unsigned char)raw[i])) i++;
	if (i == len) return cfg->result;
	if (raw[i] != '{') {
		decoded = malloc(len + 1);
		if (!decoded) { cfg->result = CTND_ERROR; return cfg->result; }
		if (!ctnd_base64(raw, len, decoded, len + 1, &n) || memchr(decoded, '\0', n)) {
			free(decoded);
			return cfg->result;
		}
		src = decoded;
	}
	j.p = (const unsigned char *)src;
	j.end = j.p + n;
	ok = object(&j, &parsed);
	ws(&j);
	if (ok && j.p == j.end) {
		parsed.result = parsed.names[0] ? CTND_VALID : CTND_EMPTY;
		*cfg = parsed;
	}
	free(decoded);
	return cfg->result;
}
