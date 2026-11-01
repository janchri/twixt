/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SUBSYS_TWIXT_TWIXT_INTERNAL_H_
#define SUBSYS_TWIXT_TWIXT_INTERNAL_H_

#include <stdint.h>

struct shell;

/* Parses an I2C address for a shell command under "twixt"; prints the error, returns -EINVAL */
int twixt_shell_parse_addr(const struct shell *sh, const char *arg, uint8_t *addr);

#endif /* SUBSYS_TWIXT_TWIXT_INTERNAL_H_ */
