/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Settings service of twixt.
 */

#ifndef TWIXT_SETTINGS_H_
#define TWIXT_SETTINGS_H_

#include <stdint.h>

#include <twixt/twixt.h>

/**
 * @defgroup twixt_settings Settings service
 * @ingroup twixt
 * @{
 *
 * Every MCU serves its Zephyr settings by name and gets and sets those of the
 * others (CONFIG_TWIXT_SETTINGS).
 *
 *     request:  TWIXT_SETTINGS_OP_GET, key, NUL         response: len, value
 *     request:  TWIXT_SETTINGS_OP_SET, key, NUL, value  response: no data
 *
 * The target calls settings_runtime_get() / settings_runtime_set() in its I2C
 * interrupt. So settings handlers must be static (SETTINGS_STATIC_HANDLER_DEFINE),
 * only copy memory, and keep their values under a spinlock. A read-only setting
 * refuses with -EACCES. After a set, a work item calls settings_save().
 *
 * Shell: <tt>twixt get \<addr\> \<key\></tt>,
 * <tt>twixt set \<addr\> \<key\> \<text\></tt>
 */

/** Op: get a setting */
#define TWIXT_SETTINGS_OP_GET 0
/** Op: set a setting */
#define TWIXT_SETTINGS_OP_SET 1

/** Longest key, NUL included */
#define TWIXT_SETTINGS_KEY_MAX   CONFIG_TWIXT_SETTINGS_KEY_MAX
/** Longest value */
#define TWIXT_SETTINGS_VALUE_MAX CONFIG_TWIXT_SETTINGS_VALUE_MAX

/**
 * @brief Get a setting of an MCU.
 *
 * @param addr MCU, may be this one.
 * @param key Name of the setting.
 * @param data Value buffer.
 * @param len Size of @p data; a longer value is cut.
 *
 * @return Length of the value, which may exceed @p len, or -errno:
 * @retval -ENOENT No such setting.
 * @retval -EINVAL No handler for the subtree, or key too long.
 * @retval -ENOTSUP The MCU serves no settings.
 */
int twixt_settings_get(uint8_t addr, const char *key, void *data, uint8_t len);

/**
 * @brief Set a setting of an MCU, which then stores it.
 *
 * @param addr MCU, may be this one.
 * @param key Name of the setting.
 * @param data Value.
 * @param len Length of @p data, up to TWIXT_SETTINGS_VALUE_MAX.
 *
 * @retval 0 Set.
 * @retval -EACCES Read-only.
 * @retval -errno See twixt_settings_get().
 */
int twixt_settings_set(uint8_t addr, const char *key, const void *data, uint8_t len);

/** @} */

#endif /* TWIXT_SETTINGS_H_ */
