/* vi: set expandtab sw=4 sts=4: */
/* pkg_hash.h - the opkg package management system

   Steven M. Ayer

   Copyright (C) 2002 Compaq Computer Corporation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#ifndef PKG_HASH_H
#define PKG_HASH_H

#include <solv/repo_deb.h>

#include "pkg.h"
#include "pkg_src.h"
#include "pkg_dest.h"
#include "hash_table.h"

#ifdef __cplusplus
extern "C" {
#endif

void file_hash_remove(const char *file_name);
pkg_t *file_hash_get_file_owner(const char *file_name);
void file_hash_set_file_owner(const char *file_name, pkg_t * pkg);

#ifdef __cplusplus
}
#endif
#endif                          /* PKG_HASH_H */
