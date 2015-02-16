/* vi: set expandtab sw=4 sts=4: */
/* opkg_download.h - the opkg package management system

   Carl D. Worth

   Copyright (C) 2001 University of Southern California

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#ifndef OPKG_SOLV_H
#define OPKG_SOLV_H

#include "str_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODE_UNKNOWN,
    MODE_INSTALL,
    MODE_REMOVE,
    MODE_UPGRADE,
    MODE_DIST_UPGRADE,
    MODE_FILES,
    MODE_LIST,
    MODE_LIST_INSTALLED,
    MODE_FLAG_HOLD,
    MODE_FLAG_NOPRUNE,
    MODE_FLAG_USER,
    MODE_FLAG_OK,
    MODE_FLAG_INSTALLED,
    MODE_FLAG_UNPACKED
} opkg_solv_mode_t;

void opkg_solv_init();
void opkg_solv_add_arch(const char *arch, int priority);
void opkg_solv_prepare();
int opkg_solv_process(str_list_t *pkg_names, opkg_solv_mode_t mode);
opkg_solv_mode_t opkg_solv_mode_from_flag_str(const char *str);

#ifdef __cplusplus
}
#endif
#endif                          /* OPKG_SOLV_H */
