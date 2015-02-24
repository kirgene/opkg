/* vi: set expandtab sw=4 sts=4: */
/* opkg_cmd.c - the opkg package management system

   Yevhen Kyriukha

   Copyright (C) 2015

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <solv/solver.h>
#include <solv/selection.h>
#include <solv/solverdebug.h>
#include <solv/repo_deb.h>
#include <solv/evr.h>
#include <stdbool.h>
#include <solv/poolarch.h>
#include <dirent.h>
#include <assert.h>

#include "opkg_solv.h"
#include "opkg_conf.h"
#include "opkg_utils.h"
#include "opkg_message.h"
#include "pkg.h"
#include "opkg_download.h"
#include "opkg_install.h"
#include "sprintf_alloc.h"
#include "file_util.h"
#include "release.h"
#include "xfuncs.h"
#include "opkg_upgrade.h"
#include "opkg_configure.h"
#include "xsystem.h"
#include "opkg_remove.h"

typedef struct {
    char *arch;
    int priority;
} arch_t;

struct opkg_intercept {
    char *oldpath;
    char *statedir;
};
typedef struct opkg_intercept *opkg_intercept_t;

pkg_vec_t *opkg_solv_pkgs;
arch_t *opkg_solv_arch_vec;
int opkg_solv_arch_vec_size;
Pool *opkg_solv_pool;
Repo *unpacked_repo;

void opkg_solv_prepare_arch();
static void get_excludes(Queue *q);

void opkg_solv_init()
{
    opkg_solv_arch_vec = NULL;
    opkg_solv_arch_vec_size = 0;
    opkg_solv_pool = pool_create();
    opkg_solv_pkgs = pkg_vec_alloc();
    unpacked_repo = NULL;
    if (opkg_config->verbosity > DEBUG2)
        pool_setdebuglevel(opkg_solv_pool, opkg_config->verbosity - DEBUG2);
}

void opkg_solv_prepare()
{
    opkg_solv_prepare_arch();
}

void opkg_solv_prepare_arch()
{
    size_t i, size = 0;
    char *str = NULL;
    for (i = 0; i < opkg_solv_arch_vec_size; ++i) {
        size += strlen(opkg_solv_arch_vec[i].arch) + 2;
        str = xrealloc(str, size);
        if (i)
            strcat(str, ">");
        else
            str[0] = '\0';
        strcat(str, opkg_solv_arch_vec[i].arch);
    }
    pool_setarchpolicy(opkg_solv_pool, str);
    free(str);
}

void app_to_unpacked_repo()
{
#if 0
    if (!unpacked_repo)
        unpacked_repo = repo_create(opkg_solv_pool, NULL);
    repo_add_solvable(...);
#endif
}

static void print_pkg(pkg_t * pkg)
{
    if (pkg->description)
        printf("%s - %s - %s\n", pkg->name, pkg->version,
                pkg->description);
    else
        printf("%s - %s\n", pkg->name, pkg->version);
}

str_list_t* read_status_tmp(const char *file_name)
{
    str_list_t *status;
    char line[500];
    FILE *fp;
    fp = fopen(file_name, "r");
    if (fp == NULL)
        return NULL;

    //read line by line
    status = str_list_alloc();
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t ln = strlen(line) - 1;
        if (line[ln] == '\n')
            line[ln] = '\0';
        str_list_append(status, line);
    }
    fclose(fp);
    return status;
}

int opkg_solv_add_from_file(const char *file_name, pkg_src_t * src,
                            pkg_dest_t * dest, int is_status_file)
{
    FILE *fp;
    int ret = 0;
    Repo *repo;
    Solvable *s, *s2;
    int p, p2;
    pkg_t *pkg;

    opkg_msg(DEBUG, "%s\n", file_name);

    if (opkg_config->check_signature && src) {
        /* pkg_src_verify prints an error message so we don't have to. */
        ret = pkg_src_verify(src);
        if (ret)
            return ret;
    }

    fp = fopen(file_name, "r");
    if (fp == NULL) {
        opkg_perror(ERROR, "Failed to open %s", file_name);
        return -1;
    }

    if (is_status_file) {
        if (opkg_solv_pool->installed) {
            repo = opkg_solv_pool->installed;
        } else {
            repo = repo_create(opkg_solv_pool, "@System");
            pool_set_installed(opkg_solv_pool, repo);
        }
    } else {
        repo = repo_create(opkg_solv_pool, src->name);
    }

    if (repo_add_debpackages(repo, fp, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE)) {
        opkg_msg(ERROR, "Component %s: %s\n", file_name, pool_errstr(opkg_solv_pool));
        fclose(fp);
        return -1;
    }

    /* remove duplicate solvables (used for transaction) from status file
     * using only the last solvable
     */
    if (is_status_file) {
        repo_internalize(repo);
        FOR_REPO_SOLVABLES(repo, p, s)
                FOR_REPO_SOLVABLES(repo, p2, s2) {
                        if (s == s2)
                            break;
                        if (solvable_identical(s, s2)) {
                            repo_free_solvable(repo, s2 - opkg_solv_pool->solvables, 1);
                            dest->changed = 1;
                        }
                    }
    }

    FOR_REPO_SOLVABLES(repo, p, s) {
            if (pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p))
                continue; /* Already processed */

            if (src)
                solvable_set_str(s, SOLVABLE_MEDIADIR, src->value);
        }

    repo_internalize(repo);

    FOR_REPO_SOLVABLES(repo, p, s) {
            if (pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p))
                continue; /* Already processed */

            pkg = pkg_new(s);
            pkg->dest = dest;
            pkg_vec_insert(opkg_solv_pkgs, pkg);

            if (is_status_file) {
                int is_installed = pkg->state_status == SS_INSTALLED
                        || pkg->state_status == SS_UNPACKED;
                if (!is_installed) {
                    repo_free_solvable(repo, p, 1);
                    //TODO: move UNPACKED packages into separate repo for later configuring ????
                }
            }

            #if 0
            if (!opkg_config->no_install_recommends) {
                /* Currently libsolv doesn't respect RECOMMENDS as a strong dependency,
                 * so we should move all RECOMMENDS to REQUIRES (DEPENDS)
                 * */
                Id rec, *recp;
                recp = repo->idarraydata + s->recommends;
                while ((rec = *recp++) != 0)            /* go through all recommends */
                    solvable_add_deparray(s, SOLVABLE_REQUIRES, rec, -SOLVABLE_PREREQMARKER);
            }
            #endif
        }

    repo_internalize(repo); // CHECK IF NEEDED ???

    if (is_status_file) {
        // Assume that status file parsed first
        pkg_info_preinstall_check(opkg_solv_pkgs);
    }

#if 0
    do {
        pkg = pkg_new();
        pkg->src = src;
        pkg->dest = dest;

        ret = parse_from_stream_nomalloc(pkg_parse_line, pkg, fp, 0, &buf, len);
        if (pkg->name == NULL) {
            /* probably just a blank line */
            ret = 1;
        }
        if (ret) {
            pkg_deinit(pkg);
            free(pkg);
            if (ret == -1)
                break;
            if (ret == 1)
                /* Probably a blank line, continue parsing. */
                ret = 0;
            continue;
        }

        if (!pkg->architecture) {
            char *version_str = pkg_version_str_alloc(pkg);
            opkg_msg(NOTICE,
                     "Package %s version %s has no "
                     "valid architecture, ignoring.\n", pkg->name, version_str);
            free(version_str);
            continue;
        }
        if (!pkg->arch_priority) {
            char *version_str = pkg_version_str_alloc(pkg);
            opkg_msg(DEBUG,
                     "Package %s version %s is built for architecture %s "
                     "which cannot be installed here, ignoring.\n", pkg->name,
                     version_str, pkg->architecture);
            free(version_str);
            continue;
        }

        hash_insert_pkg(pkg, is_status_file);

    } while (!feof(fp));

    free(buf);

#endif
    fclose(fp);

    return ret;
}

int opkg_solv_add_from_dist(pkg_src_t * dist)
{
    char *list_file, *subname;
    int r, i;

    for (i = 0; i < opkg_solv_arch_vec_size; ++i) {
        sprintf_alloc(&subname, "%s-%s", dist->name, opkg_solv_arch_vec[i].arch);
        sprintf_alloc(&list_file, "%s/%s", opkg_config->lists_dir, subname);

        if (file_exists(list_file)) {
            r = opkg_solv_add_from_file(list_file, dist, NULL, 0);
            if (r != 0) {
                free(list_file);
                return -1;
            }
            pkg_src_list_append(&opkg_config->pkg_src_list, subname,
                    dist->value, "__dummy__", 0);
        }

        free(list_file);
    }

    return 0;
}

/*
 * Load in feed files from the cached "src" and/or "src/gz" locations.
 */
int opkg_solv_load_feeds(void)
{
    pkg_src_list_elt_t *iter;
    pkg_src_t *src, *subdist;
    char *list_file;
    int r;

    opkg_msg(INFO, "\n");

    for (iter = void_list_first(&opkg_config->dist_src_list); iter;
         iter = void_list_next(&opkg_config->dist_src_list, iter)) {

        src = (pkg_src_t *) iter->data;

        sprintf_alloc(&list_file, "%s/%s", opkg_config->lists_dir, src->name);

        if (file_exists(list_file)) {
            unsigned int i;
            release_t *release = release_new();
            r = release_init_from_file(release, list_file);
            if (r != 0) {
                free(list_file);
                return -1;
            }

            unsigned int ncomp;
            const char **comps = release_comps(release, &ncomp);
            subdist = (pkg_src_t *) xmalloc(sizeof(pkg_src_t));
            memcpy(subdist, src, sizeof(pkg_src_t));

            for (i = 0; i < ncomp; i++) {
                subdist->name = NULL;
                sprintf_alloc(&subdist->name, "%s-%s", src->name, comps[i]);
                r = opkg_solv_add_from_dist(subdist);
                if (r != 0) {
                    free(subdist->name);
                    free(subdist);
                    free(list_file);
                    return -1;
                }
            }
            free(subdist->name);
            free(subdist);
        }
        free(list_file);
    }

    for (iter = void_list_first(&opkg_config->pkg_src_list); iter;
         iter = void_list_next(&opkg_config->pkg_src_list, iter)) {

        src = (pkg_src_t *) iter->data;

        sprintf_alloc(&list_file, "%s/%s", opkg_config->lists_dir, src->name);

        if (file_exists(list_file)) {
            r = opkg_solv_add_from_file(list_file, src, NULL, 0);
            if (r != 0) {
                free(list_file);
                return -1;
            }
        }
        free(list_file);
    }

    return 0;
}

/*
 * Load in status files from the configured "dest"s.
 */
int opkg_solv_load_status_files(void)
{
    pkg_dest_list_elt_t *iter;
    pkg_dest_t *dest;

    opkg_msg(INFO, "\n");

    for (iter = void_list_first(&opkg_config->pkg_dest_list); iter;
         iter = void_list_next(&opkg_config->pkg_dest_list, iter)) {

        dest = (pkg_dest_t *) iter->data;

        if (file_exists(dest->status_file_name)) {
            int r = opkg_solv_add_from_file(dest->status_file_name, NULL, dest, 1);
            if (r != 0)
                return -1;
        }
    }

    return 0;
}

/*
 * Adds architecture to internal list with sorting by priority
 */
void opkg_solv_add_arch(const char* arch, int priority)
{
    int i;
    opkg_solv_arch_vec = xrealloc(opkg_solv_arch_vec,
            (opkg_solv_arch_vec_size + 1) * sizeof(arch_t));
    for (i = 0; i < opkg_solv_arch_vec_size; ++i) {
        if (priority < opkg_solv_arch_vec[i].priority) {
            memmove(&opkg_solv_arch_vec[i + 1], &opkg_solv_arch_vec[i],
                    (opkg_solv_arch_vec_size - i) * sizeof(arch_t));
            opkg_solv_arch_vec[i].arch = xstrdup(arch);
            opkg_solv_arch_vec[i].priority = priority;
            break;
        }
    }
    if (i == opkg_solv_arch_vec_size) {
        opkg_solv_arch_vec[i].arch = xstrdup(arch);
        opkg_solv_arch_vec[i].priority = priority;
    }
    opkg_solv_arch_vec_size++;
}

static opkg_intercept_t opkg_prep_intercepts(void)
{
    opkg_intercept_t ctx;
    char *newpath;
    char *dtemp;

    ctx = xcalloc(1, sizeof(*ctx));
    ctx->oldpath = xstrdup(getenv("PATH"));
    sprintf_alloc(&newpath, "%s/opkg/intercept:%s", DATADIR, ctx->oldpath);
    sprintf_alloc(&ctx->statedir, "%s/opkg-intercept-XXXXXX",
            opkg_config->tmp_dir);

    dtemp = mkdtemp(ctx->statedir);
    if (dtemp == NULL) {
        opkg_perror(ERROR, "Failed to make temp dir %s", ctx->statedir);
        free(ctx->oldpath);
        free(ctx->statedir);
        free(newpath);
        free(ctx);
        return NULL;
    }

    setenv("OPKG_INTERCEPT_DIR", ctx->statedir, 1);
    setenv("PATH", newpath, 1);
    free(newpath);

    return ctx;
}

static int opkg_finalize_intercepts(opkg_intercept_t ctx)
{
    DIR *dir;
    int err = 0;

    setenv("PATH", ctx->oldpath, 1);
    free(ctx->oldpath);

    dir = opendir(ctx->statedir);
    if (dir) {
        struct dirent *de;
        while (de = readdir(dir), de != NULL) {
            char *path;

            if (de->d_name[0] == '.')
                continue;

            sprintf_alloc(&path, "%s/%s", ctx->statedir, de->d_name);
            if (access(path, X_OK) == 0) {
                const char *argv[] = { "sh", "-c", path, NULL };
                xsystem(argv);
            }
            free(path);
        }
        closedir(dir);
    } else
        opkg_perror(ERROR, "Failed to open dir %s", ctx->statedir);

    rm_r(ctx->statedir);
    free(ctx->statedir);
    free(ctx);

    return err;
}

int write_status_files(void)
{
    pkg_dest_list_elt_t *iter;
    pkg_dest_t *dest;
    pkg_t *pkg;
    unsigned int i;
    int ret = 0;
    int r;

    if (opkg_config->noaction)
        return 0;

    list_for_each_entry(iter, &opkg_config->pkg_dest_list.head, node) {
        dest = (pkg_dest_t *) iter->data;

        if (!dest->changed)
            continue;

        dest->status_fp = fopen(dest->status_file_name, "w");
        if (dest->status_fp == NULL && errno != EROFS) {
            opkg_perror(ERROR, "Can't open status file %s",
                    dest->status_file_name);
            ret = -1;
        }
    }

    //   pkg_hash_fetch_available(all);

    for (i = 0; i < opkg_solv_pkgs->len; i++) {
        pkg = opkg_solv_pkgs->pkgs[i];
        /* We don't need most uninstalled packages in the status file */
        int is_not_wanted = (pkg->state_status == SS_NOT_INSTALLED
                && (pkg->state_want == SW_UNKNOWN
                || (pkg->state_want == SW_DEINSTALL
                && !(pkg->state_flag & SF_HOLD))
                || pkg->state_want == SW_PURGE));
        if (is_not_wanted) {
            continue;
        }
        if (pkg->dest == NULL) {
            opkg_msg(ERROR, "Internal error: package %s has a NULL dest\n",
                    pkg->name);
            continue;
        }
        if (pkg->dest->status_fp)
            pkg_print_status(pkg, pkg->dest->status_fp);
    }

    list_for_each_entry(iter, &opkg_config->pkg_dest_list.head, node) {
        dest = (pkg_dest_t *) iter->data;
        if (dest->status_fp) {
            r = fclose(dest->status_fp);
            if (r == EOF) {
                opkg_perror(ERROR, "Couldn't close %s", dest->status_file_name);
                ret = -1;
            }
        }
    }

    return ret;
}

int write_changed_filelists(void)
{
    unsigned int i;
    int err, ret = 0;

    if (opkg_config->noaction)
        return 0;

    opkg_msg(INFO, "Saving changed filelists.\n");

    for (i = 0; i < opkg_solv_pkgs->len; i++) {
        pkg_t *pkg = opkg_solv_pkgs->pkgs[i];
        int is_installed = pkg->state_status == SS_INSTALLED
                || pkg->state_status == SS_UNPACKED;
        if (!is_installed)
            continue;
        if (pkg->state_flag & SF_FILELIST_CHANGED) {
            err = pkg_write_filelist(pkg);
            if (err)
                ret = -1;
        }
    }
    return ret;
}

static void write_all_status_files(void)
{
    if (!opkg_config->noaction) {
        opkg_msg(INFO, "Writing status file.\n");
        write_status_files();
        write_changed_filelists();
        if (!opkg_config->offline_root)
            sync();
    } else {
        opkg_msg(DEBUG, "Nothing to be done.\n");
    }
}

static void sigint_handler(int sig)
{
    signal(sig, SIG_DFL);
    opkg_msg(NOTICE, "Interrupted. Writing out status database.\n");
    write_all_status_files();
    exit(128 + sig);
}

void print_pkg_trans(Id type, pkg_t *pkg)
{
    const char *s;
    switch (type) {
        case SOLVER_TRANSACTION_DOWNGRADED:
            s = "Downgrading";
            break;
        case SOLVER_TRANSACTION_UPGRADED:
            s = "Upgrading";
            break;
        case SOLVER_TRANSACTION_ERASE:
            s = "Removing";
            break;
        case SOLVER_TRANSACTION_INSTALL:
            s = "Installing";
            break;
        case SOLVER_TRANSACTION_REINSTALLED:
        case SOLVER_TRANSACTION_CHANGED:
            s = "Reinstalling";
            break;
        default:
            s = NULL;
    }
    if (s)
        opkg_msg(NOTICE, "%s %s (%s) on %s.\n", s, pkg->name,
                pkg->version, pkg->dest->name);
}

int process_job(Solver *solver, Queue *job)
{
    Transaction *trans;
    Queue checkq;
    int newpkgs, i;
    Id p;
    pkg_t *pkg;
    int mode, err, r;
    opkg_intercept_t ic;

    for (;;)
    {
        Id problem, solution;
        int pcnt, scnt;

        if (!solver_solve(solver, job))
            break;
        if (!opkg_config->batch) {
            pcnt = solver_problem_count(solver);
            printf("Found %d problems:\n", pcnt);
            for (problem = 1; problem <= pcnt; problem++) {
                printf("Problem %d/%d:\n", problem, pcnt);
                solver_printprobleminfo(solver, problem);
                printf("\n");
                scnt = solver_solution_count(solver, problem);
                int take = 0;

                for (solution = 1; solution <= scnt; solution++) {
                    printf("Solution %d:\n", solution);
                    solver_printsolution(solver, problem, solution);
                    printf("\n");
                }
                for (; ;) {
                    char inbuf[128], *ip;
                    printf("Please choose a solution: ");
                    fflush(stdout);
                    *inbuf = 0;
                    if (!(ip = fgets(inbuf, sizeof(inbuf), stdin))) {
                        printf("Abort.\n");
                        exit(1);
                    }
                    while (*ip == ' ' || *ip == '\t')
                        ip++;
                    if (*ip >= '0' && *ip <= '9') {
                        take = atoi(ip);
                        if (take >= 1 && take <= scnt)
                            break;
                    }
                    if (*ip == 's') {
                        take = 0;
                        break;
                    }
                    if (*ip == 'q') {
                        printf("Abort.\n");
                        exit(1);
                    }
                }
                if (!take)
                    continue;
                solver_take_solution(solver, problem, take, job);
            }
        }
        else {
            //         solver_printsolution(solver, problem, 1);
            //          solver_take_solution(solver, problem, 1, job);
            solver_printallsolutions(solver);
            return 0;
        }
    }

    trans = solver_create_transaction(solver);
    if (!trans->steps.count)
    {
        printf("Nothing to do.\n");
        transaction_free(trans);
        return 0;
    }
    /* display transaction to the user and ask for confirmation */
    printf("\n");

    if (!opkg_config->batch) {
        printf("Transaction summary:\n\n");
        transaction_print(trans);

        if (!yesno("Continue (y/n)? "))
        {
            printf("Abort.\n");
            transaction_free(trans);
            return -1;
        }
    }

    /* download all new packages */
    queue_init(&checkq);
    newpkgs = transaction_installedresult(trans, &checkq);
    for (i = 0; i < newpkgs; i++)
    {
        p = checkq.elements[i];
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p);
        assert(pkg != NULL);
        if (pkg->provided_by_hand)
            continue;

        if (opkg_download_pkg(pkg)) {
            opkg_msg(ERROR, "Failed to download %s. "
                    "Perhaps you need to run 'opkg update'?\n", pkg->name);
            return -1;
        }
        fflush(stdout);
    }
    queue_free(&checkq);

    if (opkg_config->download_only) {
        transaction_free(trans);
        return 0;
    }

    /* and finally commit the transaction */
    printf("Committing transaction:\n\n");
    transaction_order(trans, 0);
    mode = SOLVER_TRANSACTION_SHOW_OBSOLETES | SOLVER_TRANSACTION_OBSOLETE_IS_UPGRADE;
    for (i = 0; i < trans->steps.count; i++)
    {
        pkg_t *pkg2;
        Id type;

        p = trans->steps.elements[i];
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p);

        type = transaction_type(trans, p, mode);
        switch(type)
        {
            case SOLVER_TRANSACTION_DOWNGRADED:
            case SOLVER_TRANSACTION_UPGRADED:
            case SOLVER_TRANSACTION_REINSTALLED:
            case SOLVER_TRANSACTION_CHANGED:
                pkg2 = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, transaction_obs_pkg(trans, p));
                pkg2->dest = pkg->dest;
                print_pkg_trans(type, pkg2);
                if (opkg_upgrade_pkg(pkg, pkg2)) {
                    return -1;
                }
                break;
            case SOLVER_TRANSACTION_ERASE:
                print_pkg_trans(type, pkg);
                opkg_remove_pkg(pkg);
                break;
            case SOLVER_TRANSACTION_INSTALL:
            case SOLVER_TRANSACTION_MULTIINSTALL:
                pkg->dest = opkg_config->default_dest;
                print_pkg_trans(type, pkg);
                if (opkg_install_pkg(NULL, pkg)) {
                    return -1;
                }
                break;
            default:
                break;
        }
    }

    if (opkg_config->offline_root && !opkg_config->force_postinstall) {
        opkg_msg(INFO,
                "Offline root mode: not configuring unpacked packages.\n");
        transaction_free(trans);
        return 0;
    }
    opkg_msg(INFO, "Configuring unpacked packages.\n");

    /* Configuring packages */
    ic = opkg_prep_intercepts();
    if (ic == NULL) {
        transaction_free(trans);
        return -1;
    }
    err = 0;

    for (i = 0; i < trans->steps.count; i++) {
        Id type;

        p = trans->steps.elements[i];
        type = transaction_type(trans, p, mode);
        switch (type) {
            case SOLVER_TRANSACTION_DOWNGRADED:
            case SOLVER_TRANSACTION_UPGRADED:
            case SOLVER_TRANSACTION_REINSTALLED:
            case SOLVER_TRANSACTION_CHANGED:
                pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, transaction_obs_pkg(trans, p));
                break;
            case SOLVER_TRANSACTION_INSTALL:
            case SOLVER_TRANSACTION_MULTIINSTALL:
                pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p);
                break;
            default:
                continue;
        }

        if (pkg->state_status == SS_UNPACKED) {
            opkg_msg(NOTICE, "Configuring %s.\n", pkg->name);
            r = opkg_configure(pkg);
            if (r == 0) {
                pkg->state_status = SS_INSTALLED;
                pkg->state_flag &= ~SF_PREFER;
                pkg->state_flag |= SF_CHANGED;
                pkg_write_status(pkg);
            } else {
                if (!opkg_config->offline_root)
                    err = -1;
            }
        }
    }
    r = opkg_finalize_intercepts(ic);
    if (r != 0)
        err = -1;

    transaction_free(trans);
    return err;
}

static int add_pkgs(Queue *job, str_list_t *pkg_names)
{
    Repo *commandlinerepo = NULL;
    str_list_elt_t *pn;
    const char *name;
    int i;
    Id p;
    int is_pkg_name = 0;
    Queue q;

    /* Search packages and add them to job */
    for (pn = str_list_first(pkg_names); pn; pn = str_list_next(pkg_names, pn)) {
        char *file_location = NULL;
        name = pn->data;

        /* First heuristic: Maybe it's a remote URL. */
        if (url_has_remote_protocol(name)) {
            file_location = opkg_download_cache(name, NULL, NULL);
            if (!file_location)
                continue;
        } else {
            /* Second heuristic: Maybe it's a package name. */
            Queue job2;
            int flags, rflags;

            queue_init(&job2);
            flags = SELECTION_NAME|SELECTION_PROVIDES|SELECTION_GLOB;
            flags |= SELECTION_CANON|SELECTION_DOTARCH|SELECTION_REL;
            selection_make(opkg_solv_pool, &job2, name, flags);
            /* queue_push2(&job, SOLVER_SOLVABLE_NAME, pool_str2id(opkg_solv_pool, argv[0], 1)); */
            for (i = 0; i < job2.count; i++)
                queue_push(job, job2.elements[i]);
            is_pkg_name = job2.count;
            queue_free(&job2);
        }

        if (!file_location && !is_pkg_name) {
            if (file_exists(name))
                file_location = strdup(name);
        }

        if (file_location) {
            if (!commandlinerepo)
                commandlinerepo = repo_create(opkg_solv_pool, "@commandline");
            p = repo_add_deb(commandlinerepo, file_location,
                    REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
            if (!p)
                opkg_msg(ERROR, "Failed to open package %s.\n", file_location);
            else /*if (solvable_allowed(pool_id2solvable(opkg_solv_pool, p))) */
                queue_push2(job, SOLVER_SOLVABLE, p);
            free(file_location);
        } else if (!is_pkg_name) {
            opkg_msg(ERROR, "Nothing matches %s.\n", name);
        }
    }
    if (commandlinerepo)
        repo_internalize(commandlinerepo);

    pool_createwhatprovides(opkg_solv_pool);
    /* add found packages to cache with Auto-Installed property */
    queue_init(&q);
    selection_solvables(opkg_solv_pool, job, &q);
    for (i = 0; i < q.count; i++) {
        Solvable *s = pool_id2solvable(opkg_solv_pool, q.elements[i]);
        pkg_t *pkg;
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, q.elements[i]);
        if (!pkg) {
            pkg = pkg_new(s);
            pkg_vec_insert(opkg_solv_pkgs, pkg);
        }
        if (s->repo == commandlinerepo) {
            pkg->dest = opkg_config->default_dest;
            pkg->state_status = SS_NOT_INSTALLED;
            pkg->provided_by_hand = 1;
            pkg->local_filename = strdup(solvable_lookup_location(s, NULL));
        }

        int is_installed = pkg->state_status == SS_INSTALLED
                || pkg->state_status == SS_UNPACKED;
        if (!is_installed) {
            pkg->auto_installed = 0;
        }
    }
    return 0;
}

static void get_excludes(Queue *q)
{
    str_list_elt_t *pn;
    const char *name;
    int i;
    Queue job;

    queue_init(&job);
    /* Search packages and add them to job */
    for (pn = str_list_first(&opkg_config->exclude_list); pn; pn = str_list_next(&opkg_config->exclude_list, pn)) {
        name = pn->data;
        Queue job2;
        int flags, rflags;

        queue_init(&job2);
        flags = SELECTION_NAME|SELECTION_PROVIDES|SELECTION_GLOB;
        flags |= SELECTION_CANON|SELECTION_DOTARCH|SELECTION_REL;
        selection_make(opkg_solv_pool, &job2, name, flags);
        /* queue_push2(&job, SOLVER_SOLVABLE_NAME, pool_str2id(opkg_solv_pool, argv[0], 1)); */
        for (i = 0; i < job2.count; i++)
            queue_push(&job, job2.elements[i]);
        queue_free(&job2);
    }

    queue_empty(q);
    selection_solvables(opkg_solv_pool, &job, q);
}

/*
 *   sort by name
 */
static int cmp_pkgs(const void *ap, const void *bp, void *dp)
{
    Pool *pool = dp;
    Id a = *(Id *)ap;
    Id b = *(Id *)bp;
    Solvable *sa, *sb;

    sa = pool->solvables + a;
    sb = pool->solvables + b;
    if (sa->name != sb->name)
        return strcmp(pool_id2str(pool, sa->name), pool_id2str(pool, sb->name));
    if (sa->evr != sb->evr)
    {
        int r = pool_evrcmp(pool, sa->evr, sb->evr, EVRCMP_COMPARE);
        if (r)
            return r;
    }
    return a - b;
}

void queue_remove_duplicates()
{

}

int configure_old_pkgs()
{
    Id p;
    pkg_t *pkg;
    int err, r;
    opkg_intercept_t ic;
    Solvable *s;

    if (opkg_config->offline_root && !opkg_config->force_postinstall) {
        opkg_msg(INFO,
                "Offline root mode: not configuring unpacked packages.\n");
        return 0;
    }

    if (!opkg_solv_pool->installed)
        return 0;

    opkg_msg(INFO, "Configuring unpacked packages.\n");

    /* Configuring packages */
    ic = opkg_prep_intercepts();
    if (ic == NULL) {
        return -1;
    }

    err = 0;
    FOR_REPO_SOLVABLES(opkg_solv_pool->installed, p, s) {
            pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p);
            if (pkg->state_want != SW_INSTALL || pkg->state_status != SS_UNPACKED)
                continue;
            opkg_msg(NOTICE, "Configuring %s.\n", pkg->name);
            r = opkg_configure(pkg);
            if (r == 0) {
                pkg->state_status = SS_INSTALLED;
                pkg->state_flag &= ~SF_PREFER;
                pkg->state_flag |= SF_CHANGED;
                pkg_write_status(pkg);
            } else {
                if (!opkg_config->offline_root)
                    err = -1;
            }
        }

    r = opkg_finalize_intercepts(ic);
    if (r != 0)
        err = -1;
    return err;
}

void prepare_recommends()
{
    int i;
    Id rec, *recp;
    Id p2, pp2, p;
    Solvable *s;
    Queue excludes;

    Pool *pool = opkg_solv_pool;

    queue_init(&excludes);
    get_excludes(&excludes);

    if (!opkg_solv_pool->considered) {
        opkg_solv_pool->considered = solv_calloc(1, sizeof(Map));
        map_init(opkg_solv_pool->considered, opkg_solv_pool->nsolvables);
        map_setall(opkg_solv_pool->considered);
    }

    for (i = 0; i < excludes.count; i++)
        MAPCLR(pool->considered, excludes.elements[i]);

    FOR_POOL_SOLVABLES(p) {
            s = pool_id2solvable(pool, p);
            recp = s->repo->idarraydata + s->recommends;
            while ((rec = *recp++) != 0) {           /* go through all recommends */
                int exlude = 0;
                int exists = 0;
                FOR_PROVIDES(p2, pp2, rec) {
                    exists = 1;
                    for (i = 0; i < excludes.count; i++) {
                        if (p2 == excludes.elements[i]) {
                            exlude = 1;
                            break;
                        }
                    }
                    if (exlude)
                        break;
                }
                if (!exlude && exists) {
                    /* Currently libsolv doesn't respect RECOMMENDS as a strong dependency,
                     * so we should move all RECOMMENDS to REQUIRES (DEPENDS)
                     * Also as RECOMMENDS is not an absolute dependency we should check it
                     * before we move it into REQUIRES */
                    solvable_add_deparray(s, SOLVABLE_REQUIRES, rec, -SOLVABLE_PREREQMARKER);
                }
            }
        }
    queue_free(&excludes);
}

void clear_dependencies() {
    Id p;
    Solvable *s;
    Queue empty;

    queue_init(&empty);
    Pool *pool = opkg_solv_pool;
    FOR_POOL_SOLVABLES(p) {
            s = pool_id2solvable(pool, p);
            solvable_set_deparray(s, SOLVABLE_REQUIRES, &empty, -SOLVABLE_PREREQMARKER);
        }
    queue_free(&empty);
}

void prepare_job(Queue *job) {
    Repo *repo;
    pkg_t *pkg;
    Id p;
    Solvable *s;

    repo = opkg_solv_pool->installed;
    if (!repo)
        return;

    FOR_REPO_SOLVABLES(repo, p, s) {
            pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, p);
            if (!pkg)
                continue;

            /* Search for HOLD installed packages and
             * remove potential candidates for upgrade/downgrade
             */
            if (pkg->state_flag & SF_HOLD) {
                queue_push2(job, SOLVER_SOLVABLE | SOLVER_LOCK, p);
            }
        }
}

void get_packages_from_selection(Queue *selection, Queue *packages)
{
    int i, j;

    queue_empty(packages);
    selection_solvables(opkg_solv_pool, selection, packages);

    /* remove duplicates from installed repo */
    for (i = 0; i < packages->count - 1; i++) {
        for (j = i + 1; j < packages->count; j++) {
            Solvable *s1 = pool_id2solvable(opkg_solv_pool, packages->elements[i]);
            Solvable *s2 = pool_id2solvable(opkg_solv_pool, packages->elements[j]);
            if (solvable_identical(s1, s2)) {
                if (s1->repo != opkg_solv_pool->installed) {
                    queue_delete(packages, j);
                    j--;
                } else {
                    queue_delete(packages, i);
                    i--;
                }
            }
        }
    }

    solv_sort(packages->elements, packages->count, sizeof(Id), cmp_pkgs, opkg_solv_pool);
}

void list_packages(Queue *selection)
{
    Queue packages;
    pkg_t *pkg;
    int i;

    queue_init(&packages);
    get_packages_from_selection(selection, &packages);
    for (i = 0; i < packages.count; i++) {
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, packages.elements[i]);
        print_pkg(pkg);
    }
    queue_free(&packages);
}

void print_pkg_status(pkg_t *pkg)
{
    pkg_formatted_info(stdout, pkg);

    if (opkg_config->verbosity >= NOTICE) {
        conffile_list_elt_t *iter;
        for (iter = nv_pair_list_first(&pkg->conffiles); iter;
             iter = nv_pair_list_next(&pkg->conffiles, iter)) {
            conffile_t *cf = (conffile_t *) iter->data;
            int modified = conffile_has_been_modified(cf);
            if (cf->value)
                opkg_msg(INFO, "conffile=%s md5sum=%s modified=%d.\n",
                        cf->name, cf->value, modified);
        }
    }
}

void list_pkg_status(Queue *selection)
{
    Queue packages;
    pkg_t *pkg;
    int i;

    queue_init(&packages);
    get_packages_from_selection(selection, &packages);
    for (i = 0; i < packages.count; i++) {
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, packages.elements[i]);
        print_pkg_status(pkg);
    }
    queue_free(&packages);
}

void list_files(Queue *selection)
{
    Queue packages;
    pkg_t *pkg;
    int i;
    str_list_t *files;
    str_list_elt_t *iter;

    queue_init(&packages);
    get_packages_from_selection(selection, &packages);

    for (i = 0; i < packages.count; i++) {
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, packages.elements[i]);

        files = pkg_get_installed_files(pkg);

        printf("Package %s (%s) is installed on %s and has the following files:\n",
                pkg->name, pkg->version, pkg->dest->name);

        for (iter = str_list_first(files); iter; iter = str_list_next(files, iter))
            printf("%s\n", (char *)iter->data);

        pkg_free_installed_files(pkg);
    }
    queue_free(&packages);
}

int pkg_state_from_mode(opkg_solv_mode_t mode)
{
    int i;
    const int mode_map[][2] = {
            {MODE_FLAG_HOLD, SF_HOLD},
            {MODE_FLAG_NOPRUNE, SF_NOPRUNE},
            {MODE_FLAG_USER, SF_USER},
            {MODE_FLAG_OK, SF_OK},
            {MODE_FLAG_INSTALLED, SS_INSTALLED},
            {MODE_FLAG_UNPACKED, SS_UNPACKED}
    };
    for (i = 0; i < ARRAY_SIZE(mode_map); i++) {
        if (mode == mode_map[i][0]) {
            return mode_map[i][1];
        }
    }
    return -1;
}

void set_installed_packages_flag(Queue *selection, opkg_solv_mode_t mode)
{
    Queue packages;
    pkg_t *pkg;
    int i;
    int pkg_state;
    char *pkg_state_str;

    queue_init(&packages);
    selection_solvables(opkg_solv_pool, selection, &packages);

    pkg_state = pkg_state_from_mode(mode);
    if ((mode == MODE_FLAG_INSTALLED) || (mode == MODE_FLAG_UNPACKED))
        pkg_state_str = strdup(pkg_state_status_to_str(pkg_state));
    else
        pkg_state_str = pkg_state_flag_to_str(pkg_state);

    for (i = 0; i < packages.count; i++) {
        Solvable *s = pool_id2solvable(opkg_solv_pool, packages.elements[i]);
        if (s->repo != opkg_solv_pool->installed)
            continue;
        pkg = pkg_vec_get_pkg_by_id(opkg_solv_pkgs, packages.elements[i]);
        if (!pkg)
            continue;

        if ((mode == MODE_FLAG_INSTALLED) || (mode == MODE_FLAG_UNPACKED))
            pkg->state_status = pkg_state;
        else
            pkg->state_flag = pkg_state;
        opkg_msg(NOTICE, "Setting flags for package %s to %s.\n", pkg->name,
                pkg_state_str);
        pkg->dest->changed = 1;
    }
    free(pkg_state_str);
    queue_free(&packages);
}

void prepare_reinstall(Queue *job)
{
    Queue pkgs;
    Solvable *si, *s;
    int i;
    Id pi;
    if (opkg_solv_pool->installed) {
        queue_init(&pkgs);
        selection_solvables(opkg_solv_pool, job, &pkgs);
        for (i = 0; i < pkgs.count; i++) {
            s = pool_id2solvable(opkg_solv_pool, pkgs.elements[i]);
            FOR_REPO_SOLVABLES(opkg_solv_pool->installed, pi, si) {
                    if (solvable_identical(s, si)) {
                        queue_push2(job, SOLVER_SOLVABLE | SOLVER_ERASE, pi);
                    }
                }
        }
        queue_free(&pkgs);
    }
}

int opkg_solv_process(str_list_t *pkg_names, opkg_solv_mode_t mode)
{
    int i;
    int err = 0;
    int opmode;
    int count;

	Solver *solv = 0;
	Queue job;

    signal(SIGINT, sigint_handler);

	pool_addfileprovides(opkg_solv_pool);
    pool_createwhatprovides(opkg_solv_pool);

    queue_init(&job);
    if (pkg_names)
        add_pkgs(&job, pkg_names);
    else
        queue_push2(&job, SOLVER_SOLVABLE_ALL, 0);

    if (mode == MODE_LIST || mode == MODE_LIST_INSTALLED)
    {
        list_packages(&job);
        queue_free(&job);
        return 0;
    }
    if (mode == MODE_STATUS || mode == MODE_STATUS_INSTALLED)
    {
        list_pkg_status(&job);
        queue_free(&job);
        return 0;
    }
    if (mode == MODE_STATUS_INSTALLED)
    {
        list_packages(&job);
        queue_free(&job);
        return 0;
    }
    if (mode == MODE_FILES) {
        list_files(&job);
        queue_free(&job);
        return 0;
    }
    if ((mode >= MODE_FLAG_HOLD) && (mode <= MODE_FLAG_UNPACKED)) {
        set_installed_packages_flag(&job, mode);
        write_all_status_files();
        queue_free(&job);
        return 0;
    }

    if (mode == MODE_INSTALL)
        opmode = SOLVER_INSTALL;
    else if (mode == MODE_REMOVE)
        opmode = SOLVER_ERASE;
    else if (mode == MODE_UPGRADE)
        opmode = SOLVER_UPDATE;
    else if (mode == MODE_DIST_UPGRADE)
        opmode = SOLVER_DISTUPGRADE;
    else {
        opkg_msg(ERROR, "Unknown mode %d.\n", mode);
        queue_free(&job);
        return -1;
    }

    solv = solver_create(opkg_solv_pool);
    solver_set_flag(solv, SOLVER_FLAG_ALLOW_UNINSTALL, 1);

    if (!opkg_config->force_depends) {
        if (!opkg_config->no_install_recommends) {
            prepare_recommends();
        } else {
            solver_set_flag(solv, SOLVER_FLAG_IGNORE_RECOMMENDED, 1);
        }
    } else {
        clear_dependencies();
    }

    count = job.count;
    for (i = 0; i < count; i += 2) {
        job.elements[i] |= opmode;
        if (opmode & SOLVER_UPDATE && pool_isemptyupdatejob(opkg_solv_pool, job.elements[i], job.elements[i + 1]))
            job.elements[i] ^= SOLVER_UPDATE ^ SOLVER_INSTALL;
        if (opkg_config->autoremove)
            job.elements[i] |= SOLVER_CLEANDEPS;
        if (!opkg_config->force_reinstall)
            job.elements[i] |= SOLVER_FORCEBEST;
    }

    if (opmode & SOLVER_INSTALL && opkg_config->force_reinstall)
        prepare_reinstall(&job);

    prepare_job(&job);

    if (configure_old_pkgs())
        err = -1;
	if (process_job(solv, &job))
		err = -1;

    write_all_status_files();

    solver_free(solv);
    queue_free(&job);

    return err;
}

opkg_solv_mode_t opkg_solv_mode_from_flag_str(const char *str)
{
    typedef struct {
        opkg_solv_mode_t mode;
        const char *str;
    } enum_map_t;

    static const enum_map_t mode_flag_map[] = {
            {MODE_FLAG_HOLD, "hold"},
            {MODE_FLAG_NOPRUNE, "noprune"},
            {MODE_FLAG_USER, "user"},
            {MODE_FLAG_INSTALLED, "installed"},
            {MODE_FLAG_OK, "ok"},
            {MODE_FLAG_UNPACKED, "unpacked"}
    };

    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mode_flag_map); i++) {
        if (strcmp(str, mode_flag_map[i].str) == 0) {
            return mode_flag_map[i].mode;
        }
    }
    return MODE_UNKNOWN;
}
