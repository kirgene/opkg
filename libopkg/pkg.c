/* vi: set expandtab sw=4 sts=4: */
/* pkg.c - the opkg package management system

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

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <malloc.h>
#include <stdlib.h>
#include <solv/chksum.h>
#include <fcntl.h>

#include "pkg.h"

#include "pkg_extract.h"
#include "opkg_download.h"
#include "opkg_message.h"
#include "opkg_utils.h"
#include "opkg_verify.h"

#include "xfuncs.h"
#include "sprintf_alloc.h"
#include "file_util.h"
#include "xsystem.h"
#include "pkg_hash.h"

typedef struct enum_map enum_map_t;
struct enum_map {
    unsigned int value;
    const char *str;
};

static const enum_map_t pkg_state_want_map[] = {
    {SW_UNKNOWN, "unknown"},
    {SW_INSTALL, "install"},
    {SW_DEINSTALL, "deinstall"},
    {SW_PURGE, "purge"}
};

static const enum_map_t pkg_state_flag_map[] = {
    {SF_OK, "ok"},
    {SF_REINSTREQ, "reinstreq"},
    {SF_HOLD, "hold"},
    {SF_REPLACE, "replace"},
    {SF_NOPRUNE, "noprune"},
    {SF_PREFER, "prefer"},
    {SF_OBSOLETE, "obsolete"},
    {SF_USER, "user"},
};

static const enum_map_t pkg_state_status_map[] = {
    {SS_NOT_INSTALLED, "not-installed"},
    {SS_UNPACKED, "unpacked"},
    {SS_HALF_CONFIGURED, "half-configured"},
    {SS_INSTALLED, "installed"},
    {SS_HALF_INSTALLED, "half-installed"},
    {SS_CONFIG_FILES, "config-files"},
    {SS_POST_INST_FAILED, "post-inst-failed"},
    {SS_REMOVAL_FAILED, "removal-failed"}
};

void pkg_init_from_solvable(pkg_t *pkg, Solvable* s);
char* get_solv_dep(pkg_t *pkg, Offset offset);

void pkg_init(pkg_t * pkg, Solvable *s)
{
    pkg->name = NULL;
    pkg->epoch = 0;
    pkg->version = NULL;
    pkg->revision = NULL;
    pkg->force_reinstall = 0;
    pkg->dest = NULL;
    pkg->src = NULL;
    pkg->architecture = NULL;
    pkg->maintainer = NULL;
    pkg->section = NULL;
    pkg->description = NULL;
    pkg->state_want = SW_UNKNOWN;
    pkg->wanted_by = pkg_vec_alloc();
    pkg->state_flag = SF_OK;
    pkg->state_status = SS_NOT_INSTALLED;
    pkg->depends_str = NULL;
    pkg->provides_str = NULL;
    pkg->suggests_str = NULL;
    pkg->recommends_str = NULL;
    pkg->conflicts_str = NULL;
    pkg->replaces_str = NULL;

    active_list_init(&pkg->list);

    pkg->filename = NULL;
    pkg->local_filename = NULL;
    pkg->tmp_unpack_dir = NULL;
    pkg->md5sum = NULL;
    pkg->sha256sum = NULL;
    pkg->size = 0;
    pkg->installed_size = 0;
    pkg->priority = NULL;
    pkg->source = NULL;
    conffile_list_init(&pkg->conffiles);
    pkg->installed_files = NULL;
    pkg->installed_files_ref_cnt = 0;
    pkg->essential = 0;
    pkg->provided_by_hand = 0;
    pkg->tags = NULL;

    if (s)
        pkg_init_from_solvable(pkg, s);
}

pkg_t *pkg_new(Solvable *s)
{
    pkg_t *pkg;

    pkg = xcalloc(1, sizeof(pkg_t));
    pkg_init(pkg, s);

    return pkg;
}

void pkg_deinit(pkg_t * pkg)
{
    free(pkg->name);
    pkg->name = NULL;

    pkg->epoch = 0;

    free(pkg->version);
    pkg->version = NULL;
    /* revision shares storage with version, so don't free */
    pkg->revision = NULL;

    pkg->force_reinstall = 0;

    /* owned by opkg_conf_t */
    pkg->dest = NULL;
    /* owned by opkg_conf_t */
    pkg->src = NULL;

    free(pkg->architecture);
    pkg->architecture = NULL;

    free(pkg->maintainer);
    pkg->maintainer = NULL;

    free(pkg->section);
    pkg->section = NULL;

    free(pkg->description);
    pkg->description = NULL;

    pkg->state_want = SW_UNKNOWN;
    pkg_vec_free(pkg->wanted_by);
    pkg->state_flag = SF_OK;
    pkg->state_status = SS_NOT_INSTALLED;

    active_list_clear(&pkg->list);

    free(pkg->replaces_str);
    pkg->replaces_str = NULL;

    free(pkg->conflicts_str);
    pkg->conflicts_str = NULL;

    free(pkg->recommends_str);
    pkg->recommends_str = NULL;

    free(pkg->depends_str);
    pkg->depends_str = NULL;

    free(pkg->provides_str);
    pkg->provides_str = NULL;

    free(pkg->suggests_str);
    pkg->suggests_str = NULL;

    free(pkg->filename);
    pkg->filename = NULL;

    free(pkg->local_filename);
    pkg->local_filename = NULL;

    /* CLEANUP: It'd be nice to pullin the cleanup function from
     * opkg_install.c here. See comment in
     * opkg_install.c:cleanup_temporary_files */
    free(pkg->tmp_unpack_dir);
    pkg->tmp_unpack_dir = NULL;

    free(pkg->md5sum);
    pkg->md5sum = NULL;

    free(pkg->sha256sum);
    pkg->sha256sum = NULL;

    free(pkg->priority);
    pkg->priority = NULL;

    free(pkg->source);
    pkg->source = NULL;

    conffile_list_deinit(&pkg->conffiles);

    /* XXX: QUESTION: Is forcing this to 1 correct? I suppose so,
     * since if they are calling deinit, they should know. Maybe do an
     * assertion here instead? */
    pkg->installed_files_ref_cnt = 1;
    pkg_free_installed_files(pkg);
    pkg->essential = 0;

    free(pkg->tags);
    pkg->tags = NULL;
}

int pkg_init_from_file(pkg_t * pkg, const char *filename)
{
#if 0
    int fd, err = 0;
    FILE *control_file;
    char *control_path, *tmp;

    pkg_init(pkg);

    pkg->local_filename = xstrdup(filename);

    tmp = xstrdup(filename);
    sprintf_alloc(&control_path, "%s/%s.control.XXXXXX", opkg_config->tmp_dir,
                  basename(tmp));
    free(tmp);
    fd = mkstemp(control_path);
    if (fd == -1) {
        opkg_perror(ERROR, "Failed to make temp file %s", control_path);
        err = -1;
        goto err0;
    }

    control_file = fdopen(fd, "r+");
    if (control_file == NULL) {
        opkg_perror(ERROR, "Failed to fdopen %s", control_path);
        close(fd);
        err = -1;
        goto err1;
    }

    err = pkg_extract_control_file_to_stream(pkg, control_file);
    if (err) {
        opkg_msg(ERROR, "Failed to extract control file from %s.\n", filename);
        goto err2;
    }

    rewind(control_file);

    err = pkg_parse_from_stream(pkg, control_file, 0);
    if (err) {
        if (err == 1) {
            opkg_msg(ERROR, "Malformed package file %s.\n", filename);
        }
        err = -1;
    }

 err2:
    fclose(control_file);
 err1:
    unlink(control_path);
 err0:
    free(control_path);

    return err;
#endif
}

#if 0
/* Merge any new information in newpkg into oldpkg */
int pkg_merge(pkg_t * oldpkg, pkg_t * newpkg)
{
    if (oldpkg == newpkg) {
        return 0;
    }

    if (!oldpkg->auto_installed)
        oldpkg->auto_installed = newpkg->auto_installed;

    if (!oldpkg->src)
        oldpkg->src = newpkg->src;
    if (!oldpkg->dest)
        oldpkg->dest = newpkg->dest;
    if (!oldpkg->arch_priority)
        oldpkg->arch_priority = newpkg->arch_priority;
    if (!oldpkg->section)
        oldpkg->section = xstrdup(newpkg->section);

    if (!oldpkg->depends_count && !oldpkg->pre_depends_count
        && !oldpkg->recommends_count && !oldpkg->suggests_count) {
        oldpkg->depends_count = newpkg->depends_count;
        newpkg->depends_count = 0;

        oldpkg->pre_depends_count = newpkg->pre_depends_count;
        newpkg->pre_depends_count = 0;

        oldpkg->recommends_count = newpkg->recommends_count;
        newpkg->recommends_count = 0;

        oldpkg->suggests_count = newpkg->suggests_count;
        newpkg->suggests_count = 0;
    }

    if (oldpkg->provides_count <= 1) {
        oldpkg->provides_count = newpkg->provides_count;
        newpkg->provides_count = 0;
    }

    if (!oldpkg->conflicts_count) {
        oldpkg->conflicts_count = newpkg->conflicts_count;
        newpkg->conflicts_count = 0;
    }

    if (!oldpkg->replaces_count) {
        oldpkg->replaces_count = newpkg->replaces_count;
        newpkg->replaces_count = 0;
    }

    if (!oldpkg->filename)
        oldpkg->filename = xstrdup(newpkg->filename);
    if (!oldpkg->local_filename)
        oldpkg->local_filename = xstrdup(newpkg->local_filename);
    if (!oldpkg->tmp_unpack_dir)
        oldpkg->tmp_unpack_dir = xstrdup(newpkg->tmp_unpack_dir);
    if (!oldpkg->md5sum)
        oldpkg->md5sum = xstrdup(newpkg->md5sum);
#if defined HAVE_SHA256
    if (!oldpkg->sha256sum)
        oldpkg->sha256sum = xstrdup(newpkg->sha256sum);
#endif
    if (!oldpkg->size)
        oldpkg->size = newpkg->size;
    if (!oldpkg->installed_size)
        oldpkg->installed_size = newpkg->installed_size;
    if (!oldpkg->priority)
        oldpkg->priority = xstrdup(newpkg->priority);
    if (!oldpkg->source)
        oldpkg->source = xstrdup(newpkg->source);

    if (nv_pair_list_empty(&oldpkg->conffiles)) {
        list_splice_init(&newpkg->conffiles.head, &oldpkg->conffiles.head);
    }

    if (!oldpkg->installed_files) {
        oldpkg->installed_files = newpkg->installed_files;
        oldpkg->installed_files_ref_cnt = newpkg->installed_files_ref_cnt;
        newpkg->installed_files = NULL;
    }

    if (!oldpkg->essential)
        oldpkg->essential = newpkg->essential;

    return 0;
}
#endif

void set_flags_from_control(pkg_t * pkg)
{
#if 0
    char *file_name;
    FILE *fp;
    int r;

    sprintf_alloc(&file_name, "%s/%s.control", pkg->dest->info_dir, pkg->name);

    fp = fopen(file_name, "r");
    if (fp == NULL) {
        opkg_perror(ERROR, "Failed to open %s", file_name);
        free(file_name);
        return;
    }

    free(file_name);

    r = pkg_parse_from_stream(pkg, fp, PFM_ALL ^ PFM_ESSENTIAL);
    if (r != 0) {
        opkg_msg(DEBUG, "Unable to read control file for %s. May be empty.\n",
                 pkg->name);
    }

    fclose(fp);

    return;
#endif
}

static const char *pkg_state_want_to_str(pkg_state_want_t sw)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_want_map); i++) {
        if (pkg_state_want_map[i].value == sw) {
            return pkg_state_want_map[i].str;
        }
    }

    opkg_msg(ERROR, "Internal error: state_want=%d\n", sw);
    return "<STATE_WANT_UNKNOWN>";
}

pkg_state_want_t pkg_state_want_from_str(char *str)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_want_map); i++) {
        if (strcmp(str, pkg_state_want_map[i].str) == 0) {
            return pkg_state_want_map[i].value;
        }
    }

    opkg_msg(ERROR, "Internal error: state_want=%s\n", str);
    return SW_UNKNOWN;
}

void pkg_add_conffile(pkg_t *pkg, const char *file_name, const char *md5sum)
{
    conffile_list_append(&pkg->conffiles, file_name, md5sum);
}

char *pkg_state_flag_to_str(pkg_state_flag_t sf)
{
    unsigned int i;
    unsigned int len;
    char *str;

    /* clear the temporary flags before converting to string */
    sf &= SF_NONVOLATILE_FLAGS;

    if (sf == 0)
        return xstrdup("ok");

    len = 0;
    for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
        if (sf & pkg_state_flag_map[i].value)
            len += strlen(pkg_state_flag_map[i].str) + 1;
    }

    str = xmalloc(len + 1);
    str[0] = '\0';

    for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
        if (sf & pkg_state_flag_map[i].value) {
            strncat(str, pkg_state_flag_map[i].str, len);
            strncat(str, ",", len);
        }
    }

    len = strlen(str);
    str[len - 1] = '\0';        /* squash last comma */

    return str;
}

pkg_state_flag_t pkg_state_flag_from_str(const char *str)
{
    unsigned int i;
    int sf = SF_OK;
    const char *sfname;
    unsigned int sfname_len;

    if (strcmp(str, "ok") == 0) {
        return SF_OK;
    }
    for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
        sfname = pkg_state_flag_map[i].str;
        sfname_len = strlen(sfname);
        if (strncmp(str, sfname, sfname_len) == 0) {
            sf |= pkg_state_flag_map[i].value;
            str += sfname_len;
            if (str[0] == ',') {
                str++;
            } else {
                break;
            }
        }
    }

    return sf;
}

const char *pkg_state_status_to_str(pkg_state_status_t ss)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_status_map); i++) {
        if (pkg_state_status_map[i].value == ss) {
            return pkg_state_status_map[i].str;
        }
    }

    opkg_msg(ERROR, "Internal error: state_status=%d\n", ss);
    return "<STATE_STATUS_UNKNOWN>";
}

pkg_state_status_t pkg_state_status_from_str(const char *str)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_status_map); i++) {
        if (strcmp(str, pkg_state_status_map[i].str) == 0) {
            return pkg_state_status_map[i].value;
        }
    }

    opkg_msg(ERROR, "Internal error: state_status=%s\n", str);
    return SS_NOT_INSTALLED;
}

void pkg_parse_status_str(pkg_t * pkg, const char *sstr)
{
    char sw_str[64], sf_str[64], ss_str[64];
    int r;

    r = sscanf(sstr, "%63s %63s %63s", sw_str, sf_str, ss_str);
    if (r != 3) {
        opkg_msg(ERROR, "Failed to parse status for %s\n", pkg->name);
        return;
    }

    pkg->state_want = pkg_state_want_from_str(sw_str);
    pkg->state_flag = pkg_state_flag_from_str(sf_str);
    pkg->state_status = pkg_state_status_from_str(ss_str);
}

void pkg_init_from_solvable(pkg_t *pkg, Solvable* s)
{
    const char *sstr;
    Dataiterator di;
    Pool *pool;
    int chksumtype;
    const char *chksum;

    pool = s->repo->pool;

    pkg->solvable = s;
    pkg->name = xstrdup(solvable_lookup_str(s, SOLVABLE_NAME));
    pkg->version = xstrdup(solvable_lookup_str(s, SOLVABLE_EVR));
    pkg->url = xstrdup(solvable_lookup_location(pkg->solvable, NULL));
    pkg->architecture = xstrdup(solvable_lookup_str(s, SOLVABLE_ARCH));
    pkg->description = xstrdup(solvable_lookup_str(s, SOLVABLE_DESCRIPTION));
    //      pkg->filename = strdup(solvable_lookup_str(s, SOLVABLE_));
    pkg->installed_size = solvable_lookup_num(s, SOLVABLE_INSTALLSIZE, 0);
    pkg->installed_time = solvable_lookup_num(s, SOLVABLE_INSTALLTIME, 0);
    pkg->maintainer = xstrdup(solvable_lookup_str(s, SOLVABLE_VENDOR));

    /* get md5 sum */
    chksumtype = REPOKEY_TYPE_MD5;
    chksum = solvable_lookup_checksum(pkg->solvable, SOLVABLE_CHECKSUM, &chksumtype);
    if (chksumtype)
        pkg->md5sum = xstrdup(chksum);

    /* get sha256 sum */
    chksumtype = REPOKEY_TYPE_SHA256;
    chksum = solvable_lookup_checksum(pkg->solvable, SOLVABLE_CHECKSUM, &chksumtype);
    if (chksumtype)
        pkg->sha256sum = xstrdup(chksum);

    /* get status */
    sstr = solvable_lookup_str(pkg->solvable, SOLVABLE_INSTALLSTATUS);
    if (sstr)
        pkg_parse_status_str(pkg, sstr);

    /* Auto-Installed flag */
    pkg->auto_installed = solvable_lookup_num(pkg->solvable, SOLVABLE_USERINSTALLED, -1);
    if (pkg->auto_installed >= 0)
        pkg->auto_installed = !pkg->auto_installed;

    /* add the conffiles */
    dataiterator_init(&di, pool, pkg->solvable->repo, pkg->solvable - pool->solvables, SOLVABLE_DEB_CONFFILES, 0, 0);
    while (dataiterator_step(&di)) {
        const char* filename = pool_id2str(pool, di.kv.id);
        chksum = repodata_chk2str(di.data, REPOKEY_TYPE_MD5, di.kv.str);
        pkg_add_conffile(pkg, filename, chksum);
    }
    dataiterator_free(&di);

    pkg->depends_str = get_solv_dep(pkg, s->requires);
    pkg->recommends_str = get_solv_dep(pkg, s->recommends);
    pkg->provides_str = get_solv_dep(pkg, s->provides);
    pkg->suggests_str = get_solv_dep(pkg, s->suggests);
    pkg->conflicts_str = get_solv_dep(pkg, s->conflicts);
    pkg->replaces_str = get_solv_dep(pkg, s->obsoletes);

    /* */
}

int pkg_restore_status(pkg_t *pkg, str_list_t *status_tmp)
{
    str_list_elt_t *pn;
    const char *line;
    size_t len;
    int res = -1;

    for (pn = str_list_first(status_tmp); pn; pn = str_list_next(status_tmp, pn)) {
        line = pn->data;
        len = strlen(pkg->name);
        if (strncmp(pkg->name, line, len))
            continue;

        line += len + 1;
        len = strlen(pkg->architecture);
        if (strncmp(pkg->architecture, line, len))
            continue;

        line += len + 1;
        len = strlen(pkg->version);
        if (strncmp(pkg->version, line, len))
            continue;

        line += len + 1;
        pkg_parse_status_str(pkg, line);
        res = 0;
    }
    return res;
}

char* get_solv_dep(pkg_t *pkg, Offset offset)
{
    Id d, *dp;
    int first = 1;
    const char *dep, *ver;
    int name_len;
    Pool *pool;
    int is_provides;
    char *res = NULL;
    dp = pkg->solvable->repo->idarraydata + offset;
    pool = pkg->solvable->repo->pool;
    is_provides = pkg->solvable->provides == offset;
    while ((d = *dp++) != 0) {
        if (is_provides && !*dp) {
            /* Don't get provides for package itself */
            break;
        }
        dep = pool_dep2str(pool, d);
        ver = strchr(dep, ' ');
        if (ver) {
            name_len = ver - dep;
            ver++;
        } else {
            name_len = strlen(dep);
        }
        if (first) {
            sprintf_alloc(&res, "%.*s", name_len, dep);
            first = 0;
        } else {
            res = xrealloc(res, strlen(res) + name_len + 3);
            sprintf(res + strlen(res), ", %.*s", name_len, dep);
        }
        if (ver) {
            res = xrealloc(res, strlen(res) + strlen(ver) + 4);
            sprintf(res + strlen(res), " (%s)", ver);
        }
    }
    return res;
}

#if 0
void print_solv_dep(FILE * fp, Pool *pool, Id *dp, int is_provides)
{
    Id d;
    int first = 1;
    const char *dep, *ver;
    int name_len;
    while ((d = *dp++) != 0) {
        if (is_provides && !*dp) {
            /* Don't print provides for package itself */
            break;
        }
        dep = pool_dep2str(pool, d);
        ver = strchr(dep, ' ');
        if (ver) {
            name_len = ver - dep;
            ver++;
        } else {
            name_len = strlen(dep);
        }
        if (first) {
            fprintf(fp, " %.*s", name_len, dep);
            first = 0;
        } else {
            fprintf(fp, ", %.*s", name_len, dep);
        }
        if (ver)
            fprintf(fp, " (%s)", ver);
    }
}
#endif

void pkg_formatted_field(FILE * fp, pkg_t * pkg, const char *field)
{
    if (strlen(field) < PKG_MINIMUM_FIELD_NAME_LEN) {
        goto UNKNOWN_FMT_FIELD;
    }

    switch (field[0]) {
    case 'a':
    case 'A':
        if (strcasecmp(field, "Architecture") == 0) {
            if (pkg->architecture) {
                fprintf(fp, "Architecture: %s\n", pkg->architecture);
            }
        } else if (strcasecmp(field, "Auto-Installed") == 0) {
            if (pkg->auto_installed)
                fprintf(fp, "Auto-Installed: yes\n");
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'c':
    case 'C':
        if (strcasecmp(field, "Conffiles") == 0) {
            conffile_list_elt_t *iter;

            if (nv_pair_list_empty(&pkg->conffiles))
                return;

            fprintf(fp, "Conffiles:\n");
            for (iter = nv_pair_list_first(&pkg->conffiles); iter;
                    iter = nv_pair_list_next(&pkg->conffiles, iter)) {
                conffile_t * cf = (conffile_t *) iter->data;
                if (cf->name && cf->value) {
                    fprintf(fp, " %s %s\n", ((conffile_t *) iter->data)->name,
                            ((conffile_t *) iter->data)->value);
                }
            }
        } else if (strcasecmp(field, "Conflicts") == 0) {
            if (pkg->conflicts_str) {
                fprintf(fp, "Conflicts: %s\n", pkg->conflicts_str);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'd':
    case 'D':
        if (strcasecmp(field, "Depends") == 0) {
            if (pkg->depends_str) {
                fprintf(fp, "Depends: %s\n", pkg->depends_str);
            }
        } else if (strcasecmp(field, "Description") == 0) {
            if (pkg->description) {
                fprintf(fp, "Description: %s\n", pkg->description);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'e':
    case 'E':
        if (pkg->essential) {
            fprintf(fp, "Essential: yes\n");
        }
        break;
    case 'f':
    case 'F':
        if (pkg->filename) {
            fprintf(fp, "Filename: %s\n", pkg->filename);
        }
        break;
    case 'i':
    case 'I':
        if (strcasecmp(field, "Installed-Size") == 0) {
            fprintf(fp, "Installed-Size: %ld\n", pkg->installed_size);
        } else if (strcasecmp(field, "Installed-Time") == 0
                   && pkg->installed_time) {
            fprintf(fp, "Installed-Time: %lu\n", pkg->installed_time);
        }
        break;
    case 'm':
    case 'M':
        if (strcasecmp(field, "Maintainer") == 0) {
            if (pkg->maintainer) {
                fprintf(fp, "Maintainer: %s\n", pkg->maintainer);
            }
        } else if (strcasecmp(field, "MD5sum") == 0) {
            if (pkg->md5sum) {
                fprintf(fp, "MD5Sum: %s\n", pkg->md5sum);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'p':
    case 'P':
        if (strcasecmp(field, "Package") == 0) {
            fprintf(fp, "Package: %s\n", pkg->name);
        } else if (strcasecmp(field, "Priority") == 0) {
            fprintf(fp, "Priority: %s\n", pkg->priority);
        } else if (strcasecmp(field, "Provides") == 0) {
            if (pkg->provides_str) {
                fprintf(fp, "Provides: %s\n", pkg->provides_str);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'r':
    case 'R':
        if (strcasecmp(field, "Replaces") == 0) {
            if (pkg->replaces_str) {
                fprintf(fp, "Replaces: %s\n", pkg->replaces_str);
            }
        } else if (strcasecmp(field, "Recommends") == 0) {
            if (pkg->recommends_str) {
                fprintf(fp, "Recommends: %s\n", pkg->recommends_str);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 's':
    case 'S':
        if (strcasecmp(field, "Section") == 0) {
            if (pkg->section) {
                fprintf(fp, "Section: %s\n", pkg->section);
            }
        } else if (strcasecmp(field, "SHA256sum") == 0) {
            if (pkg->sha256sum) {
                fprintf(fp, "SHA256sum: %s\n", pkg->sha256sum);
            }
        } else if (strcasecmp(field, "Size") == 0) {
            if (pkg->size) {
                fprintf(fp, "Size: %ld\n", pkg->size);
            }
        } else if (strcasecmp(field, "Source") == 0) {
            if (pkg->source) {
                fprintf(fp, "Source: %s\n", pkg->source);
            }
        } else if (strcasecmp(field, "Status") == 0) {
            char *pflag = pkg_state_flag_to_str(pkg->state_flag);
            fprintf(fp, "Status: %s %s %s\n",
                    pkg_state_want_to_str(pkg->state_want), pflag,
                    pkg_state_status_to_str(pkg->state_status));
            free(pflag);
        } else if (strcasecmp(field, "Suggests") == 0) {
            if (pkg->suggests_str) {
                fprintf(fp, "Suggests: %s\n", pkg->suggests_str);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 't':
    case 'T':
        if (strcasecmp(field, "Tags") == 0) {
            if (pkg->tags) {
                fprintf(fp, "Tags: %s\n", pkg->tags);
            }
        }
        break;
    case 'v':
    case 'V':
        if (pkg->version) {
            fprintf(fp, "Version: %s\n", pkg->version);
        }
        break;
    default:
        goto UNKNOWN_FMT_FIELD;
    }

    return;

 UNKNOWN_FMT_FIELD:
    opkg_msg(ERROR, "Internal error: field=%s\n", field);
}

void pkg_formatted_info(FILE * fp, pkg_t * pkg)
{
    pkg_formatted_field(fp, pkg, "Package");
    pkg_formatted_field(fp, pkg, "Version");
    pkg_formatted_field(fp, pkg, "Depends");
    pkg_formatted_field(fp, pkg, "Recommends");
    pkg_formatted_field(fp, pkg, "Suggests");
    pkg_formatted_field(fp, pkg, "Provides");
    pkg_formatted_field(fp, pkg, "Replaces");
    pkg_formatted_field(fp, pkg, "Conflicts");
    pkg_formatted_field(fp, pkg, "Status");
    pkg_formatted_field(fp, pkg, "Section");
    pkg_formatted_field(fp, pkg, "Essential");
    pkg_formatted_field(fp, pkg, "Architecture");
    pkg_formatted_field(fp, pkg, "Maintainer");
    pkg_formatted_field(fp, pkg, "MD5sum");
    pkg_formatted_field(fp, pkg, "Size");
    pkg_formatted_field(fp, pkg, "Filename");
    pkg_formatted_field(fp, pkg, "Conffiles");
    pkg_formatted_field(fp, pkg, "Source");
    pkg_formatted_field(fp, pkg, "Description");
    pkg_formatted_field(fp, pkg, "Installed-Time");
    pkg_formatted_field(fp, pkg, "Tags");
    fputs("\n", fp);
}

void pkg_print_status(pkg_t * pkg, FILE * file)
{
    if (pkg == NULL) {
        return;
    }

    pkg_formatted_field(file, pkg, "Package");
    pkg_formatted_field(file, pkg, "Version");
    pkg_formatted_field(file, pkg, "Depends");
    pkg_formatted_field(file, pkg, "Recommends");
    pkg_formatted_field(file, pkg, "Suggests");
    pkg_formatted_field(file, pkg, "Provides");
    pkg_formatted_field(file, pkg, "Replaces");
    pkg_formatted_field(file, pkg, "Conflicts");
    pkg_formatted_field(file, pkg, "Status");
    pkg_formatted_field(file, pkg, "Essential");
    pkg_formatted_field(file, pkg, "Architecture");
    pkg_formatted_field(file, pkg, "Conffiles");
    pkg_formatted_field(file, pkg, "Installed-Time");
    pkg_formatted_field(file, pkg, "Auto-Installed");
    fputs("\n", file);
}

/*
 * libdpkg - Debian packaging suite library routines
 * vercmp.c - comparison of version numbers
 *
 * Copyright (C) 1995 Ian Jackson <iwj10@cus.cam.ac.uk>
 */

/* assume ascii */
static int order(char x)
{
    if (x == '~')
        return -1;
    if (isdigit(x))
        return 0;
    if (!x)
        return 0;
    if (isalpha(x))
        return x;

    return 256 + (int)x;
}

static int verrevcmp(const char *val, const char *ref)
{
    if (!val)
        val = "";
    if (!ref)
        ref = "";

    while (*val || *ref) {
        int first_diff = 0;

        while ((*val && !isdigit(*val)) || (*ref && !isdigit(*ref))) {
            int vc = order(*val), rc = order(*ref);
            if (vc != rc)
                return vc - rc;
            val++;
            ref++;
        }

        while (*val == '0')
            val++;
        while (*ref == '0')
            ref++;
        while (isdigit(*val) && isdigit(*ref)) {
            if (!first_diff)
                first_diff = *val - *ref;
            val++;
            ref++;
        }
        if (isdigit(*val))
            return 1;
        if (isdigit(*ref))
            return -1;
        if (first_diff)
            return first_diff;
    }
    return 0;
}

int pkg_compare_versions_no_reinstall(const pkg_t * pkg, const pkg_t * ref_pkg)
{
#if 0
    int r;

    r = pkg->epoch - ref_pkg->epoch;
    if (r)
        return r;

    r = verrevcmp(pkg->version, ref_pkg->version);
    if (r)
        return r;

    r = verrevcmp(pkg->revision, ref_pkg->revision);
    return r;
#endif
}

int pkg_compare_versions(const pkg_t * pkg, const pkg_t * ref_pkg)
{
    int r;

    r = pkg_compare_versions_no_reinstall(pkg, ref_pkg);
    if (r)
        return r;

    /* Compare force_reinstall flags. */
    r = pkg->force_reinstall - ref_pkg->force_reinstall;
    return r;
}

int pkg_version_satisfied(pkg_t * it, pkg_t * ref, const char *op)
{
#if 0
    int r;

    r = pkg_compare_versions(it, ref);
    enum version_constraint constraint = str_to_constraint(&op);

    switch (constraint) {
    case EARLIER_EQUAL:
        return r <= 0;
    case LATER_EQUAL:
        return r >= 0;
    case EARLIER:
        return r < 0;
    case LATER:
        return r > 0;
    case EQUAL:
        return r == 0;
    case NONE:
        opkg_msg(ERROR, "Unknown operator: %s.\n", op);
    }
    return 0;
#endif
}

#if 0
int pkg_name_version_and_architecture_compare(const void *p1, const void *p2)
{
    const pkg_t *a = *(const pkg_t **)p1;
    const pkg_t *b = *(const pkg_t **)p2;
    int namecmp;
    int vercmp;
    if (!a->name || !b->name) {
        opkg_msg(ERROR, "Internal error: a->name=%p, b->name=%p.\n", a->name,
                 b->name);
        return 0;
    }

    namecmp = strcmp(a->name, b->name);
    if (namecmp)
        return namecmp;
    vercmp = pkg_compare_versions(a, b);
    if (vercmp)
        return vercmp;
    if (!a->arch_priority || !b->arch_priority) {
        opkg_msg(ERROR,
                 "Internal error: a->arch_priority=%i b->arch_priority=%i.\n",
                 a->arch_priority, b->arch_priority);
        return 0;
    }
    if (a->arch_priority > b->arch_priority)
        return 1;
    if (a->arch_priority < b->arch_priority)
        return -1;
    return 0;
}
#endif

/*
 * XXX: this should be broken into two functions
 */
str_list_t *pkg_get_installed_files(pkg_t * pkg)
{
    int err, fd;
    char *list_file_name = NULL;
    FILE *list_file = NULL;
    char *line;
    char *installed_file_name;
    int list_from_package;

    pkg->installed_files_ref_cnt++;

    if (pkg->installed_files) {
        return pkg->installed_files;
    }

    pkg->installed_files = str_list_alloc();

    /*
     * For installed packages, look at the package.list file in the database.
     * For uninstalled packages, get the file list directly from the package.
     */
    if (pkg->state_status == SS_NOT_INSTALLED || pkg->dest == NULL)
        list_from_package = 1;
    else
        list_from_package = 0;

    if (list_from_package) {
        if (pkg->local_filename == NULL) {
            return pkg->installed_files;
        }
        /* XXX: CLEANUP: Maybe rewrite this to avoid using a temporary
         * file. In other words, change deb_extract so that it can
         * simply return the file list as a char *[] rather than
         * insisting on writing it to a FILE * as it does now. */
        sprintf_alloc(&list_file_name, "%s/%s.list.XXXXXX",
                      opkg_config->tmp_dir, pkg->name);
        fd = mkstemp(list_file_name);
        if (fd == -1) {
            opkg_perror(ERROR, "Failed to make temp file %s.", list_file_name);
            free(list_file_name);
            return pkg->installed_files;
        }
        list_file = fdopen(fd, "r+");
        if (list_file == NULL) {
            opkg_perror(ERROR, "Failed to fdopen temp file %s.",
                        list_file_name);
            close(fd);
            unlink(list_file_name);
            free(list_file_name);
            return pkg->installed_files;
        }
        err = pkg_extract_data_file_names_to_stream(pkg, list_file);
        if (err) {
            opkg_msg(ERROR, "Error extracting file list from %s.\n",
                     pkg->local_filename);
            fclose(list_file);
            unlink(list_file_name);
            free(list_file_name);
            str_list_deinit(pkg->installed_files);
            pkg->installed_files = NULL;
            return NULL;
        }
        rewind(list_file);
    } else {
        sprintf_alloc(&list_file_name, "%s/%s.list", pkg->dest->info_dir,
                      pkg->name);
        list_file = fopen(list_file_name, "r");
        if (list_file == NULL) {
            opkg_perror(ERROR, "Failed to open %s", list_file_name);
            free(list_file_name);
            return pkg->installed_files;
        }
        free(list_file_name);
    }

    while (1) {
        char *file_name;

        line = file_read_line_alloc(list_file);
        if (line == NULL) {
            break;
        }
        file_name = line;

        if (list_from_package) {
            if (*file_name == '.') {
                file_name++;
            }
            if (*file_name == '/') {
                file_name++;
            }
            sprintf_alloc(&installed_file_name, "%s%s", pkg->dest->root_dir,
                          file_name);
        } else {
            int unmatched_offline_root = opkg_config->offline_root
                    && !str_starts_with(file_name, opkg_config->offline_root);
            if (unmatched_offline_root) {
                sprintf_alloc(&installed_file_name, "%s%s",
                              opkg_config->offline_root, file_name);
            } else {
                // already contains root_dir as header -> ABSOLUTE
                sprintf_alloc(&installed_file_name, "%s", file_name);
            }
        }
        str_list_append(pkg->installed_files, installed_file_name);
        free(installed_file_name);
        free(line);
    }

    fclose(list_file);

    if (list_from_package) {
        unlink(list_file_name);
        free(list_file_name);
    }

    return pkg->installed_files;
}

/* XXX: CLEANUP: This function and it's counterpart,
   (pkg_get_installed_files), do not match our init/deinit naming
   convention. Nor the alloc/free convention. But, then again, neither
   of these conventions currrently fit the way these two functions
   work. */
void pkg_free_installed_files(pkg_t * pkg)
{
    pkg->installed_files_ref_cnt--;

    if (pkg->installed_files_ref_cnt > 0)
        return;

    if (pkg->installed_files) {
        str_list_purge(pkg->installed_files);
    }

    pkg->installed_files = NULL;
}

void pkg_remove_installed_files_list(pkg_t * pkg)
{
    char *list_file_name;

    sprintf_alloc(&list_file_name, "%s/%s.list", pkg->dest->info_dir,
                  pkg->name);

    if (!opkg_config->noaction)
        (void)unlink(list_file_name);

    free(list_file_name);
}

conffile_t *pkg_get_conffile(pkg_t * pkg, const char *file_name)
{
    conffile_list_elt_t *iter;
    conffile_t *conffile;

    if (pkg == NULL) {
        return NULL;
    }

    for (iter = nv_pair_list_first(&pkg->conffiles); iter;
            iter = nv_pair_list_next(&pkg->conffiles, iter)) {
        conffile = (conffile_t *) iter->data;

        if (strcmp(conffile->name, file_name) == 0) {
            return conffile;
        }
    }

    return NULL;
}

int pkg_run_script(pkg_t * pkg, const char *script, const char *args)
{
    int err;
    char *path;
    char *cmd;

    if (opkg_config->noaction)
        return 0;

    if (opkg_config->offline_root && !opkg_config->force_postinstall) {
        opkg_msg(INFO, "Offline root mode: not running %s.%s.\n", pkg->name,
                 script);
        return 0;
    }

    /* Installed packages have scripts in pkg->dest->info_dir, uninstalled packages
     * have scripts in pkg->tmp_unpack_dir. */
    if (pkg->state_status == SS_INSTALLED || pkg->state_status == SS_UNPACKED) {
        if (pkg->dest == NULL) {
            opkg_msg(ERROR, "Internal error: %s has a NULL dest.\n", pkg->name);
            return -1;
        }
        sprintf_alloc(&path, "%s/%s.%s", pkg->dest->info_dir, pkg->name,
                      script);
    } else {
        if (pkg->tmp_unpack_dir == NULL) {
            opkg_msg(ERROR, "Internal error: %s has a NULL tmp_unpack_dir.\n",
                     pkg->name);
            return -1;
        }
        sprintf_alloc(&path, "%s/%s", pkg->tmp_unpack_dir, script);
    }

    opkg_msg(INFO, "Running script %s.\n", path);

    setenv("PKG_ROOT",
           pkg->dest ? pkg->dest->root_dir : opkg_config->default_dest->root_dir,
           1);

    if (!file_exists(path)) {
        free(path);
        return 0;
    }

    sprintf_alloc(&cmd, "%s %s", path, args);
    free(path);
    {
        const char *argv[] = { "sh", "-c", cmd, NULL };
        err = xsystem(argv);
    }
    free(cmd);

    if (err) {
        if (!opkg_config->offline_root)
            opkg_msg(ERROR, "package \"%s\" %s script returned status %d.\n",
                     pkg->name, script, err);
        return err;
    }

    return 0;
}

void pkg_info_preinstall_check(pkg_vec_t *installed_pkgs)
{
    unsigned int i;
    /* update the file owner data structure */
    opkg_msg(INFO, "Updating file owner list.\n");
    for (i = 0; i < installed_pkgs->len; i++) {
        pkg_t *pkg = installed_pkgs->pkgs[i];
        str_list_t *installed_files = pkg_get_installed_files(pkg);     /* this causes installed_files to be cached */
        str_list_elt_t *iter, *niter;
        if (installed_files == NULL) {
            opkg_msg(ERROR,
                     "Failed to determine installed " "files for pkg %s.\n",
                     pkg->name);
            break;
        }
        for (iter = str_list_first(installed_files), niter = str_list_next(installed_files, iter);
                iter;
                iter = niter, niter = str_list_next(installed_files, iter)) {
            char *installed_file = (char *)iter->data;
            file_hash_set_file_owner(installed_file, pkg);
        }
        pkg_free_installed_files(pkg);
    }
}

struct pkg_write_filelist_data {
    pkg_t *pkg;
    FILE *stream;
};

static void pkg_write_filelist_helper(const char *key, void *entry_,
                                      void *data_)
{
    struct pkg_write_filelist_data *data = data_;
    pkg_t *entry = entry_;
    if (entry == data->pkg) {
        fprintf(data->stream, "%s\n", key);
    }
}

int pkg_write_filelist(pkg_t * pkg)
{
    struct pkg_write_filelist_data data;
    char *list_file_name;

    sprintf_alloc(&list_file_name, "%s/%s.list", pkg->dest->info_dir,
                  pkg->name);

    opkg_msg(INFO, "Creating %s file for pkg %s.\n", list_file_name, pkg->name);

    data.stream = fopen(list_file_name, "w");
    if (!data.stream) {
        opkg_perror(ERROR, "Failed to open %s", list_file_name);
        free(list_file_name);
        return -1;
    }

    data.pkg = pkg;
    hash_table_foreach(&opkg_config->file_hash, pkg_write_filelist_helper,
                       &data);
    fclose(data.stream);
    free(list_file_name);

    pkg->state_flag &= ~SF_FILELIST_CHANGED;

    return 0;
}

int pkg_write_status(pkg_t * pkg)
{
    FILE *f;
    int ret;

    if (opkg_config->noaction)
        return 0;

    if (pkg->dest == NULL) {
        opkg_msg(ERROR, "Internal error: package %s has a NULL dest\n", pkg->name);
        return -1;
    }
    f = fopen(pkg->dest->status_file_name, "a");
    if (f == NULL) {
        opkg_perror(ERROR, "Can't open status file %s", pkg->dest->status_file_name);
        ret = -1;
    } else {
        pkg_print_status(pkg, f);
        fclose(f);
        ret = 0;
    }
    pkg->dest->changed = 1;
    return ret;
}



static int verify_checksum(const char *file, const unsigned char *chksum, Id chksumtype)
{   
	char buf[1024];
	const unsigned char *sum;
	Chksum *h;
	int l, err, fd;

	h = solv_chksum_create(chksumtype);
	if (!h)
	{
		opkg_msg(ERROR, "%s: unknown checksum type\n", file);
		return 0;
	}   
	fd = open(file, O_RDONLY);
	while ((l = read(fd, buf, sizeof(buf))) > 0)
		solv_chksum_add(h, buf, l);
	lseek(fd, 0, SEEK_SET);
	l = 0;
	sum = solv_chksum_get(h, &l);
	err = memcmp(sum, chksum, l);
	solv_chksum_free(h, 0);
	close(fd);
	return err;
}

int pkg_verify(pkg_t *pkg)
{
	const unsigned char *chksum;
	int chksumtype;

    if (!file_exists(pkg->local_filename))
        return -1;

	chksumtype = 0;
	chksum = solvable_lookup_bin_checksum(pkg->solvable, SOLVABLE_CHECKSUM, &chksumtype);
	if (chksumtype && verify_checksum(pkg->local_filename, chksum, chksumtype))
		goto fail;

    if (opkg_config->check_pkg_signature) {
        char *local_sig_filename = pkg_download_signature(pkg);
        if (!local_sig_filename)
            goto fail;

        int err = opkg_verify_signature(pkg->local_filename, local_sig_filename);
		free(local_sig_filename);
        if (err)
            goto fail;
    }

	return 0;

 fail:
    opkg_msg(NOTICE, "Removing corrupt package file %s.\n",
             pkg->local_filename);
    unlink(pkg->local_filename);
    return -1;
}