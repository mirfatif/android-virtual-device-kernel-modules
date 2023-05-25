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

MODULE_DESCRIPTION("A test module for Kleaf testing purposes");
MODULE_AUTHOR("Yifan Hong <elsk@google.com>");
MODULE_LICENSE("GPL v2");

// -DNUMBER=123
#if NUMBER != 123
#error bad number
#endif

// -DBOOLDEF
#ifndef BOOLDEF
#error bad boolean
#endif

// -DSTR=MYTOKEN
#define MYTOKEN 123456
#if STR != MYTOKEN
#error bad string
#endif

// -DMY_FUNC_DECL='VOID FUNC SEMICOLON'
#define VOID void
#define FUNC myfunction(void)
#define SEMICOLON ;
MY_FUNC_DECL

// -Wno-null-dereference -Wno-unused-value
void other_func(void) {
    *((int8_t*)NULL);
}
