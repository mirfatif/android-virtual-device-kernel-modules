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

// This is implicitly included in the command line.
// #include "include_test_lib.h"

MODULE_DESCRIPTION("A test module for Kleaf testing purposes");
MODULE_AUTHOR("Yifan Hong <elsk@google.com>");
MODULE_LICENSE("GPL v2");

int include_test(void) {
    include_test_lib();
    return 0;
}
