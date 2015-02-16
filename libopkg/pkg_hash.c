/* vi: set expandtab sw=4 sts=4: */
/* opkg_hash.c - the opkg package management system

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

#include "config.h"

#include <stdio.h>

#include "hash_table.h"
#include "release.h"
#include "pkg.h"
#include "pkg_hash.h"

static const char *strip_offline_root(const char *file_name)
{
    unsigned int len;

    if (opkg_config->offline_root) {
        len = strlen(opkg_config->offline_root);
        if (strncmp(file_name, opkg_config->offline_root, len) == 0)
            file_name += len;
    }

    return file_name;
}

void file_hash_remove(const char *file_name)
{
    file_name = strip_offline_root(file_name);
    hash_table_remove(&opkg_config->file_hash, file_name);
}

pkg_t *file_hash_get_file_owner(const char *file_name)
{
    file_name = strip_offline_root(file_name);
    return hash_table_get(&opkg_config->file_hash, file_name);
}

void file_hash_set_file_owner(const char *file_name, pkg_t * owning_pkg)
{
    pkg_t *old_owning_pkg;

    file_name = strip_offline_root(file_name);

    old_owning_pkg = hash_table_get(&opkg_config->file_hash, file_name);
    hash_table_insert(&opkg_config->file_hash, file_name, owning_pkg);

    if (old_owning_pkg) {
        pkg_get_installed_files(old_owning_pkg);
        str_list_remove_elt(old_owning_pkg->installed_files, file_name);
        pkg_free_installed_files(old_owning_pkg);

        /* mark this package to have its filelist written */
        old_owning_pkg->state_flag |= SF_FILELIST_CHANGED;
        owning_pkg->state_flag |= SF_FILELIST_CHANGED;
    }
}
