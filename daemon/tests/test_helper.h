/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTND_TEST_HELPER_H
#define CTND_TEST_HELPER_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int test_count;

#define CHECK(expr) do { \
	test_count++; \
	if (!(expr)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

#endif
