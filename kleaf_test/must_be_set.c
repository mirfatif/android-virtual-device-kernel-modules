/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kconfig.h>

MODULE_DESCRIPTION("A test module for Kleaf testing purposes");
MODULE_AUTHOR("Yifan Hong <elsk@google.com>");
MODULE_LICENSE("GPL v2");

#if !IS_ENABLED(CONFIG_KLEAF_MUST_BE_SET)
#error CONFIG_KLEAF_MUST_BE_SET is not set!
#endif
