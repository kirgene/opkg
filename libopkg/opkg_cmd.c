/* vi: set expandtab sw=4 sts=4: */
/* opkg_cmd.c - the opkg package management system

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
#include <dirent.h>
#include <glob.h>
#include <fnmatch.h>
#include <signal.h>
#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>

#include <solv/solver.h>
#include <solv/selection.h>
#include <solv/solverdebug.h>

#include "opkg_conf.h"
#include "opkg_cmd.h"
#include "opkg_message.h"
#include "release.h"
#include "pkg_dest.h"
#include "sprintf_alloc.h"
#include "file_util.h"
#include "opkg_utils.h"
#include "opkg_download.h"
#include "opkg_install.h"
#include "opkg_upgrade.h"
#include "opkg_configure.h"
#include "opkg_verify.h"
#include "xsystem.h"
#include "xfuncs.h"
#include "opkg_solv.h"

void populate_arch_list()
{
    nv_pair_list_elt_t *l;
    list_for_each_entry(l, &opkg_config->arch_list.head, node) {
        nv_pair_t *nv = (nv_pair_t *) l->data;
        opkg_solv_add_arch(nv->name, atoi(nv->value));
    }
}

str_list_t* pkg_names_from_args(int argc, char **argv)
{
    str_list_t *pkg_names;
    int i;
    if (!argc)
        return NULL;
    pkg_names = str_list_alloc();
    for (i = 0; i < argc; i++) {
        str_list_append(pkg_names, argv[i]);
    }
    return pkg_names;
}

static int opkg_update_cmd(int argc, char **argv)
{
    char *tmp, *dtemp;
    int err;
    int failures;
    pkg_src_list_elt_t *iter;
    pkg_src_t *src;

    if (!file_is_dir(opkg_config->lists_dir)) {
        if (file_exists(opkg_config->lists_dir)) {
            opkg_msg(ERROR, "%s exists, but is not a directory.\n",
                     opkg_config->lists_dir);
            return -1;
        }
        err = file_mkdir_hier(opkg_config->lists_dir, 0755);
        if (err)
            return -1;
    }

    failures = 0;

    sprintf_alloc(&tmp, "%s/update-XXXXXX", opkg_config->tmp_dir);
    dtemp = mkdtemp(tmp);
    if (dtemp == NULL) {
        opkg_perror(ERROR, "Failed to make temp dir %s", opkg_config->tmp_dir);
        return -1;
    }

    for (iter = void_list_first(&opkg_config->dist_src_list); iter;
            iter = void_list_next(&opkg_config->dist_src_list, iter)) {
        char *url, *list_file_name;

        src = (pkg_src_t *) iter->data;

        sprintf_alloc(&url, "%s/dists/%s/Release", src->value, src->name);

        sprintf_alloc(&list_file_name, "%s/%s", opkg_config->lists_dir,
                      src->name);
        err = opkg_download(url, list_file_name, NULL, NULL);
        if (!err) {
            opkg_msg(NOTICE, "Downloaded release files for dist %s.\n",
                     src->name);
            release_t *release = release_new();
            err = release_init_from_file(release, list_file_name);
            if (!err) {
                if (!release_comps_supported(release, src->extra_data))
                    err = -1;
            }
            if (!err) {
                err = release_download(release, src, opkg_config->lists_dir, tmp);
            }
            release_deinit(release);
            if (err)
                unlink(list_file_name);
        }

        if (err)
            failures++;

        free(list_file_name);
        free(url);
    }

    for (iter = void_list_first(&opkg_config->pkg_src_list); iter;
            iter = void_list_next(&opkg_config->pkg_src_list, iter)) {
        src = (pkg_src_t *) iter->data;

        if (src->extra_data && !strcmp(src->extra_data, "__dummy__ "))
            continue;

        err = pkg_src_update(src);
        if (err)
            failures++;
    }
    rmdir(tmp);
    free(tmp);

    return failures;
}

struct opkg_intercept {
    char *oldpath;
    char *statedir;
};

typedef struct opkg_intercept *opkg_intercept_t;

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

static int opkg_configure_packages(char *pkg_name)
{
    //TODO: implement through SOLV during install
#if 0
    pkg_vec_t *all, *ordered, *visited;
    unsigned int i;
    pkg_t *pkg;
    opkg_intercept_t ic;
    int r, err = 0;

    if (opkg_config->offline_root && !opkg_config->force_postinstall) {
        opkg_msg(INFO,
                 "Offline root mode: not configuring unpacked packages.\n");
        return 0;
    }
    opkg_msg(INFO, "Configuring unpacked packages.\n");

    all = pkg_vec_alloc();

    pkg_hash_fetch_available(all);

    /* Reorder pkgs in order to be configured according to the Depends: tag
     * order */
    opkg_msg(INFO, "Reordering packages before configuring them...\n");
    ordered = pkg_vec_alloc();
    visited = pkg_vec_alloc();
    for (i = 0; i < all->len; i++) {
        pkg = all->pkgs[i];
        opkg_recurse_pkgs_in_order(pkg, all, visited, ordered);
    }

    ic = opkg_prep_intercepts();
    if (ic == NULL) {
        err = -1;
        goto error;
    }

    for (i = 0; i < ordered->len; i++) {
        pkg = ordered->pkgs[i];

        if (pkg_name && fnmatch(pkg_name, pkg->name, 0))
            continue;

        if (pkg->state_status == SS_UNPACKED) {
            opkg_msg(NOTICE, "Configuring %s.\n", pkg->name);
            r = opkg_configure(pkg);
            if (r == 0) {
                pkg->state_status = SS_INSTALLED;
                pkg->parent->state_status = SS_INSTALLED;
                pkg->state_flag &= ~SF_PREFER;
                opkg_state_changed++;
            } else {
                if (!opkg_config->offline_root)
                    err = -1;
            }
        }
    }

    r = opkg_finalize_intercepts(ic);
    if (r != 0)
        err = -1;

 error:
    pkg_vec_free(all);
    pkg_vec_free(ordered);
    pkg_vec_free(visited);

    return err;
#endif
    return 0;
}

static int opkg_remove_cmd(int argc, char **argv);

static int opkg_install_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
    err = opkg_solv_process(pkg_names, MODE_INSTALL);
    return err;
#if 0
    int i;
    char *arg;
    int err = 0;
    str_list_t *pkg_names_to_install = NULL;
    int r;

    Queue job;


#if 0
	{
		/* list mode, no solver needed */
		Queue q;
		queue_init(&job);
		queue_init(&q);
		for (i = 0; i < job.count; i += 2)
		{
			int j;
			queue_empty(&q);
			pool_job2solvables(opkg_config->pool, &q, job.elements[i], job.elements[i + 1]);
			for (j = 0; j < q.count; j++)
			{
				Solvable *s = pool_id2solvable(opkg_config->pool, q.elements[j]);
				{
					const char *str;
					printf("Name:        %s\n", pool_solvable2str(opkg_config->pool, s));
					printf("Repo:        %s\n", s->repo->name);
					printf("Summary:     %s\n", solvable_lookup_str(s, SOLVABLE_SUMMARY));
					str = solvable_lookup_str(s, SOLVABLE_URL);
					if (str)
						printf("Url:         %s\n", str);
					str = solvable_lookup_str(s, SOLVABLE_LICENSE);
					if (str)
						printf("License:     %s\n", str);
#if 0
					str = solvable_lookup_sourcepkg(s);
					if (str)
						printf("Source:      %s\n", str);
#endif
					printf("Description:\n%s\n", solvable_lookup_str(s, SOLVABLE_DESCRIPTION));
					printf("\n");
				}
			}
		}
	}
	exit(1);
#endif




	{
    int err;
    unsigned int i;
    char *package_name = "udev";
	Transaction *trans;
    Id p;
	Solver *solv = 0;
/*
    repo = repo_create(opkg_config->pool, "@commandline");
    p = repo_add_deb(repo, (const char *)package_name, REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
    if (!p)
    {
        fprintf(stderr, "could not add '%s'\n", argv[i]);
        exit(1);
    }
	*/
	pool_addfileprovides(opkg_config->pool);
    pool_createwhatprovides(opkg_config->pool);
    queue_init(&job);
    queue_push2(&job, SOLVER_SOLVABLE_ALL, 0);
	int mode = SOLVER_UPDATE;
	
	for (i = 0; i < job.count; i += 2)
	{
		job.elements[i] |= mode;
		if (mode == SOLVER_UPDATE && pool_isemptyupdatejob(opkg_config->pool, job.elements[i], job.elements[i + 1]))
			job.elements[i] ^= SOLVER_UPDATE ^ SOLVER_INSTALL;
	}
	
    solv = solver_create(opkg_config->pool);
//    solver_set_flag(solv, SOLVER_FLAG_SPLITPROVIDES, 1);
//
        solver_solve(solv, &job);
/*
    for (;;)
    {
        Id problem, solution;
        int pcnt, scnt;

        if (!solver_solve(solv, &job))
            break;
        pcnt = solver_problem_count(solv);
        printf("Found %d problems:\n", pcnt);
        for (problem = 1; problem <= pcnt; problem++)
        {
            int take = 0;
            printf("Problem %d/%d:\n", problem, pcnt);
            solver_printprobleminfo(solv, problem);
            printf("\n");
            scnt = solver_solution_count(solv, problem);
            for (solution = 1; solution <= scnt; solution++)
            {
                printf("Solution %d:\n", solution);
                solver_printsolution(solv, problem, solution);
                printf("\n");
            }
            for (;;)
            {
                char inbuf[128], *ip;
                printf("Please choose a solution: ");
                fflush(stdout);
                *inbuf = 0;
                if (!(ip = fgets(inbuf, sizeof(inbuf), stdin)))
                {
                    printf("Abort.\n");
                    exit(1);
                }
                while (*ip == ' ' || *ip == '\t')
                    ip++;
                if (*ip >= '0' && *ip <= '9')
                {
                    take = atoi(ip);
                    if (take >= 1 && take <= scnt)
                        break;
                }
                if (*ip == 's')
                {
                    take = 0;
                    break;
                }
                if (*ip == 'q')
                {
                    printf("Abort.\n");
                    exit(1);
                }
            }
            if (!take)
                continue;
            solver_take_solution(solv, problem, take, &job);
        }
    }
	*/

    trans = solver_create_transaction(solv);
    if (!trans->steps.count)
    {
        printf("Nothing to do.\n");
        transaction_free(trans);
        solver_free(solv);
        queue_free(&job);
//        pool_free(pool);
//        free_repoinfos(repoinfos, nrepoinfos);
//        solv_free(commandlinepkgs);
        exit(1);
    }
    /* display transaction to the user and ask for confirmation */
    printf("\n");
    printf("Transaction summary:\n\n");
    transaction_print(trans);
    exit(1);


	}

    signal(SIGINT, sigint_handler);

    /*
     * Now scan through package names and install
     */
    for (i = 0; i < argc; i++) {
        arg = argv[i];

        opkg_msg(DEBUG2, "%s\n", arg);
        r = opkg_prepare_url_for_install(arg, &argv[i]);
        if (r != 0)
            return -1;
    }
    pkg_info_preinstall_check();

    if (opkg_config->combine)
        pkg_names_to_install = str_list_alloc();

    for (i = 0; i < argc; i++) {
        arg = argv[i];
        if (opkg_config->combine) {
            str_list_append(pkg_names_to_install, arg);
        } else {
            r = opkg_install_by_name(arg);
            if (r != 0) {
                opkg_msg(ERROR, "Cannot install package %s.\n", arg);
                err = -1;
            }
        }
    }

    if (opkg_config->combine) {
        r = opkg_install_multiple_by_name(pkg_names_to_install);
        if (r != 0)
            err = -1;

        str_list_purge(pkg_names_to_install);
    }

    r = opkg_configure_packages(NULL);
    if (r != 0)
        err = -1;

    write_status_files_if_changed();

    return err;
#endif
}

static int opkg_upgrade_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
#if 0
    err = opkg_solv_process(pkg_names, MODE_UPGRADE);
#else
    err = opkg_solv_process(pkg_names, MODE_DIST_UPGRADE);
#endif
    return err;

#if 0
    if (argc) {
        pkg_info_preinstall_check();

        if (opkg_config->combine)
            pkgs_to_upgrade = pkg_vec_alloc();

        for (i = 0; i < argc; i++) {
            if (opkg_config->restrict_to_default_dest) {
                pkg = pkg_hash_fetch_installed_by_name_dest(argv[i],
                        opkg_config->default_dest);
                if (pkg == NULL) {
                    opkg_msg(ERROR, "Package %s not installed in %s.\n",
                             argv[i], opkg_config->default_dest->name);
                    continue;
                }
            } else {
                pkg = pkg_hash_fetch_installed_by_name(argv[i]);
                if (pkg == NULL) {
                    opkg_msg(ERROR, "Package %s not installed.\n", argv[i]);
                    continue;
                }
            }
            if (opkg_config->combine) {
                pkg_vec_insert(pkgs_to_upgrade, pkg);
            } else {
                r = opkg_upgrade_pkg(pkg);
                if (r != 0)
                    err = -1;
            }
        }

        if (opkg_config->combine) {
            r = opkg_upgrade_multiple_pkgs(pkgs_to_upgrade);
            if (r != 0)
                err = -1;

            pkg_vec_free(pkgs_to_upgrade);
        }
    } else {
        pkg_vec_t *installed = pkg_vec_alloc();

        pkg_info_preinstall_check();

        pkg_hash_fetch_all_installed(installed);

        if (opkg_config->combine) {
            err = opkg_upgrade_multiple_pkgs(installed);
        } else {
            for (j = 0; j < installed->len; j++) {
                pkg = installed->pkgs[j];
                r = opkg_upgrade_pkg(pkg);
                if (r != 0)
                    err = -1;
            }
        }
        pkg_vec_free(installed);
    }

    r = opkg_configure_packages(NULL);
    if (r != 0)
        err = -1;

    write_status_files_if_changed();

    return err;
#endif
}

static int opkg_download_cmd(int argc, char **argv)
{
    //TODO: IMPLEMENT
#if 0
    int i, err = 0;
    char *arg;
    pkg_t *pkg;
    int r;

    pkg_info_preinstall_check();
    for (i = 0; i < argc; i++) {
        arg = argv[i];

        pkg = pkg_hash_fetch_best_installation_candidate_by_name(arg);
        if (pkg == NULL) {
            opkg_msg(ERROR, "Cannot find package %s.\n", arg);
            continue;
        }

        r = opkg_download_pkg_to_dir(pkg, ".");
        if (r != 0) {
            err = -1;
            opkg_msg(ERROR, "Failed to download %s.\n", pkg->name);
        } else {
            opkg_msg(NOTICE, "Downloaded %s as %s.\n", pkg->name,
                     pkg->filename);
        }
    }

    return err;
#endif
}

static int opkg_list_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
    err = opkg_solv_process(pkg_names, MODE_LIST);
    return err;
}

static int opkg_list_installed_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
    err = opkg_solv_process(pkg_names, MODE_LIST_INSTALLED);
    return err;
}

static int opkg_list_changed_conffiles_cmd(int argc, char **argv)
{
    unsigned int i;
    pkg_vec_t *available;
    pkg_t *pkg;
    char *pkg_name = NULL;
    conffile_list_elt_t *iter;
    conffile_t *cf;

    if (argc > 0) {
        pkg_name = argv[0];
    }
    available = pkg_vec_alloc();
//    pkg_hash_fetch_all_installed(available);
//    pkg_vec_sort(available, pkg_compare_names);
    for (i = 0; i < available->len; i++) {
        pkg = available->pkgs[i];
        /* if we have package name or pattern and pkg does not match, then skip it */
        if (pkg_name && fnmatch(pkg_name, pkg->name, 0))
            continue;
        if (nv_pair_list_empty(&pkg->conffiles))
            continue;
        for (iter = nv_pair_list_first(&pkg->conffiles); iter;
                iter = nv_pair_list_next(&pkg->conffiles, iter)) {
            cf = (conffile_t *) iter->data;
            if (cf->name && cf->value && conffile_has_been_modified(cf))
                printf("%s\n", cf->name);
        }
    }
    pkg_vec_free(available);
    return 0;
}

static int opkg_list_upgradable_cmd(int argc, char **argv)
{
#if 0
    struct active_list *head = prepare_upgrade_list();
    struct active_list *node = NULL;
    pkg_t *_old_pkg, *_new_pkg;
    char *old_v, *new_v;
    for (node = active_list_next(head, head); node;
            node = active_list_next(head, node)) {
        _old_pkg = list_entry(node, pkg_t, list);
   //     _new_pkg = pkg_hash_fetch_best_installation_candidate_by_name(_old_pkg->name);
        if (_new_pkg == NULL)
            continue;
        old_v = pkg_version_str_alloc(_old_pkg);
        new_v = pkg_version_str_alloc(_new_pkg);
        printf("%s - %s - %s\n", _old_pkg->name, old_v, new_v);
        free(old_v);
        free(new_v);
    }
    active_list_head_delete(head);
    return 0;
#endif
}

static int opkg_info_status_cmd(int argc, char **argv, int installed_only)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
    err = opkg_solv_process(pkg_names, MODE_STATUS);
    return err;
    //TODO: Implement
    #if 0
    unsigned int i, err;
    pkg_vec_t *available;
    pkg_t *pkg;
    char *pkg_name = NULL;
    char b_match = 0;

    if (argc > 0) {
        pkg_name = argv[0];
    }

    available = pkg_vec_alloc();
    /*
    if (installed_only)
        pkg_hash_fetch_all_installed(available);
    else
        pkg_hash_fetch_available(available);
*/
    for (i = 0; i < available->len; i++) {
        pkg = available->pkgs[i];
        if (pkg_name && fnmatch(pkg_name, pkg->name, 0)) {
            continue;
        }

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
        b_match = 1;
    }
    pkg_vec_free(available);

    if (!b_match && pkg_name && file_exists(pkg_name)) {
        pkg = pkg_new();
        err = pkg_init_from_file(pkg, pkg_name);
        if (err)
            return err;
   //     hash_insert_pkg(pkg, 0);
        pkg_formatted_info(stdout, pkg);
    }

    return 0;
#endif
}

static int opkg_info_cmd(int argc, char **argv)
{
    return opkg_info_status_cmd(argc, argv, 0);
}

static int opkg_status_cmd(int argc, char **argv)
{
    return opkg_info_status_cmd(argc, argv, 1);
}

static int opkg_configure_cmd(int argc, char **argv)
{
#if 0
    //TODO: IMPLEMENT (do we need this???)
    int err;
    char *pkg_name = NULL;

    if (argc > 0)
        pkg_name = argv[0];

    err = opkg_configure_packages(pkg_name);

    write_status_files_if_changed();

    return err;
#endif
}

static int opkg_clean_cmd(int argc, char **argv)
{
    int err;
    err = rm_r(opkg_config->cache_dir);
    return err;
}

static int opkg_remove_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
    err = opkg_solv_process(pkg_names, MODE_REMOVE);
    return err;
#if 0
    int i, done, err = 0;
    unsigned int a;
    pkg_t *pkg;
    pkg_t *pkg_to_remove;
    pkg_vec_t *available;
    int r;

    done = 0;

    signal(SIGINT, sigint_handler);

    pkg_info_preinstall_check();

    available = pkg_vec_alloc();
    pkg_hash_fetch_all_installed(available);

    for (i = 0; i < argc; i++) {
        for (a = 0; a < available->len; a++) {
            pkg = available->pkgs[a];
            if (fnmatch(argv[i], pkg->name, 0)) {
                continue;
            }
            if (opkg_config->restrict_to_default_dest) {
                pkg_to_remove = pkg_hash_fetch_installed_by_name_dest(pkg->name,
                        opkg_config->default_dest);
            } else {
                pkg_to_remove = pkg_hash_fetch_installed_by_name(pkg->name);
            }

            if (pkg_to_remove == NULL) {
                opkg_msg(ERROR, "Package %s is not installed.\n", pkg->name);
                continue;
            }
            if (pkg->state_status == SS_NOT_INSTALLED) {
                opkg_msg(ERROR, "Package %s not installed.\n", pkg->name);
                continue;
            }

            r = opkg_remove_pkg(pkg_to_remove);
            if (r != 0)
                err = -1;
            else
                done = 1;
        }
    }

    pkg_vec_free(available);

    if (done == 0)
        opkg_msg(NOTICE, "No packages removed.\n");

    write_status_files_if_changed();
    return err;
#endif
}

static int opkg_flag_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    opkg_solv_mode_t mode = opkg_solv_mode_from_flag_str(argv[0]);
    if (mode == MODE_UNKNOWN) {
        opkg_msg(ERROR, "Unknown flag '%s'.\n", argv[0]);
        err = -1;
    } else {
        populate_arch_list();
        opkg_solv_prepare();
        pkg_names = pkg_names_from_args(argc - 1, argv + 1);
        err = opkg_solv_process(pkg_names, mode);
    }
    return err;
    //TODO: IMPLEMENT
#if 0
    int i;
    pkg_t *pkg;


    signal(SIGINT, sigint_handler);

    for (i = 1; i < argc; i++) {
        if (opkg_config->restrict_to_default_dest) {
            pkg = pkg_hash_fetch_installed_by_name_dest(argv[i],
                    opkg_config->default_dest);
        } else {
            pkg = pkg_hash_fetch_installed_by_name(argv[i]);
        }

        if (pkg == NULL) {
            opkg_msg(ERROR, "Package %s is not installed.\n", argv[i]);
            continue;
        }
        int is_state_flag = ((strcmp(flags, "hold") == 0) || (strcmp(flags, "noprune") == 0)
                || (strcmp(flags, "user") == 0) || (strcmp(flags, "ok") == 0));
        if (is_state_flag) {
            pkg->state_flag = pkg_state_flag_from_str(flags);
        }

        /*
         * Useful if a package is installed in an offline_root, and
         * should be configured by opkg configure at a later date.
         */
        int is_state_status = ((strcmp(flags, "installed") == 0)
                || (strcmp(flags, "unpacked") == 0));
        if (is_state_status) {
            pkg->state_status = pkg_state_status_from_str(flags);
        }

        opkg_state_changed++;
        opkg_msg(NOTICE, "Setting flags for package %s to %s.\n", pkg->name,
                 flags);
    }

    write_status_files_if_changed();
    return 0;
#endif
}

static int opkg_files_cmd(int argc, char **argv)
{
    int err;
    str_list_t *pkg_names;
    populate_arch_list();
    opkg_solv_prepare();
    pkg_names = pkg_names_from_args(argc, argv);
    err = opkg_solv_process(pkg_names, MODE_FILES);
    return err;
//TODO: IMPLEMENT
#if 0
    pkg_t *pkg;
    str_list_t *files;
    str_list_elt_t *iter;
    char *pkg_version;

    if (argc < 1) {
        return -1;
    }

    pkg = pkg_hash_fetch_installed_by_name(argv[0]);
    if (pkg == NULL) {
        opkg_msg(ERROR, "Package %s not installed.\n", argv[0]);
        return 0;
    }

    files = pkg_get_installed_files(pkg);
    pkg_version = pkg_version_str_alloc(pkg);

    printf("Package %s (%s) is installed on %s and has the following files:\n",
           pkg->name, pkg_version, pkg->dest->name);

    for (iter = str_list_first(files); iter; iter = str_list_next(files, iter))
        printf("%s\n", (char *)iter->data);

    free(pkg_version);
    pkg_free_installed_files(pkg);

    return 0;
#endif
}

static int opkg_depends_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    int i, k;
    unsigned int j;
    int depends_count;
    pkg_vec_t *available_pkgs;
    compound_depend_t *cdep;
    pkg_t *pkg;
    char *str;

    pkg_info_preinstall_check();

    available_pkgs = pkg_vec_alloc();
    if (opkg_config->query_all)
        pkg_hash_fetch_available(available_pkgs);
    else
        pkg_hash_fetch_all_installed(available_pkgs);

    for (i = 0; i < argc; i++) {
        for (j = 0; j < available_pkgs->len; j++) {
            pkg = available_pkgs->pkgs[j];

            if (fnmatch(argv[i], pkg->name, 0) != 0)
                continue;

            depends_count = pkg->depends_count + pkg->pre_depends_count +
                pkg->recommends_count + pkg->suggests_count;

            opkg_msg(NOTICE, "%s depends on:\n", pkg->name);

            for (k = 0; k < depends_count; k++) {
                cdep = &pkg->depends[k];

                if (cdep->type != DEPEND)
                    continue;

                str = pkg_depend_str(pkg, k);
                opkg_msg(NOTICE, "\t%s\n", str);
                free(str);
            }

        }
    }

    pkg_vec_free(available_pkgs);
    return 0;
#endif
}

/*
static int pkg_mark_provides(pkg_t * pkg)
{
    int provides_count = pkg->provides_count;
    abstract_pkg_t **provides = pkg->provides;
    int i;
    pkg->parent->state_flag |= SF_MARKED;
    for (i = 0; i < provides_count; i++) {
        provides[i]->state_flag |= SF_MARKED;
    }
    return 0;
}
*/

enum what_field_type {
    WHATDEPENDS,
    WHATCONFLICTS,
    WHATPROVIDES,
    WHATREPLACES,
    WHATRECOMMENDS,
    WHATSUGGESTS
};

static int opkg_what_depends_conflicts_cmd(int what_field_type,
                                           int recursive, int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    depend_t *possibility;
    compound_depend_t *cdep;
    pkg_vec_t *available_pkgs;
    pkg_t *pkg;
    int i, k, l;
    unsigned int j;
    int changed, count;
    const char *rel_str = NULL;
    char *ver;

    switch (what_field_type) {
    case DEPEND:
        rel_str = "depends on";
        break;
    case CONFLICTS:
        rel_str = "conflicts with";
        break;
    case SUGGEST:
        rel_str = "suggests";
        break;
    case RECOMMEND:
        rel_str = "recommends";
        break;
    default:
        return -1;
    }

    available_pkgs = pkg_vec_alloc();

    if (opkg_config->query_all)
        pkg_hash_fetch_available(available_pkgs);
    else
        pkg_hash_fetch_all_installed(available_pkgs);

    /* mark the root set */
    pkg_vec_clear_marks(available_pkgs);
    opkg_msg(NOTICE, "Root set:\n");
    for (i = 0; i < argc; i++)
        pkg_vec_mark_if_matches(available_pkgs, argv[i]);

    for (j = 0; j < available_pkgs->len; j++) {
        pkg = available_pkgs->pkgs[j];
        if (pkg->state_flag & SF_MARKED) {
            /* mark the parent (abstract) package */
            pkg_mark_provides(pkg);
            opkg_msg(NOTICE, "  %s\n", pkg->name);
        }
    }

    opkg_msg(NOTICE, "What %s root set\n", rel_str);
    do {
        changed = 0;

        for (j = 0; j < available_pkgs->len; j++) {
            pkg = available_pkgs->pkgs[j];
            count = (what_field_type == CONFLICTS) ? pkg->conflicts_count :
                    (pkg->pre_depends_count + pkg->depends_count + pkg->recommends_count + pkg->suggests_count);

            /* skip this package if it is already marked */
            if (pkg->parent->state_flag & SF_MARKED)
                continue;

            for (k = 0; k < count; k++) {
                cdep = (what_field_type == CONFLICTS) ? &pkg->conflicts[k] : &pkg->depends[k];

                if (what_field_type != cdep->type)
                    continue;

                for (l = 0; l < cdep->possibility_count; l++) {
                    possibility = cdep->possibilities[l];

                    if ((possibility->pkg->state_flag & SF_MARKED) != SF_MARKED)
                        continue;

                    /* mark the depending package so we
                     * won't visit it again */
                    pkg->state_flag |= SF_MARKED;
                    pkg_mark_provides(pkg);
                    changed++;

                    ver = pkg_version_str_alloc(pkg);
                    opkg_msg(NOTICE, "\t%s %s\t%s %s", pkg->name, ver, rel_str,
                             possibility->pkg->name);
                    free(ver);
                    if (possibility->version) {
                        opkg_msg(NOTICE, " (%s%s)",
                                 constraint_to_str(possibility->constraint),
                                 possibility->version);
                    }
                    if (!pkg_dependence_satisfiable(possibility))
                        opkg_msg(NOTICE, " unsatisfiable");
                    opkg_message(NOTICE, "\n");
                    goto next_package;
                }
            }
 next_package:
            ;
        }
    } while (changed && recursive);

    pkg_vec_free(available_pkgs);

    return 0;
#endif
}

static int opkg_whatdepends_recursively_cmd(int argc, char **argv)
{//TODO: IMPLEMENT
#if 0
    return opkg_what_depends_conflicts_cmd(DEPEND, 1, argc, argv);
#endif
}

static int opkg_whatdepends_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    return opkg_what_depends_conflicts_cmd(DEPEND, 0, argc, argv);
#endif
}

static int opkg_whatsuggests_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    return opkg_what_depends_conflicts_cmd(SUGGEST, 0, argc, argv);
#endif
}

static int opkg_whatrecommends_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    return opkg_what_depends_conflicts_cmd(RECOMMEND, 0, argc, argv);
#endif
}

static int opkg_whatconflicts_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    return opkg_what_depends_conflicts_cmd(CONFLICTS, 0, argc, argv);
#endif
}

static int opkg_what_provides_replaces_cmd(enum what_field_type what_field_type,
                                           int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    if (argc > 0) {
        pkg_vec_t *available_pkgs = pkg_vec_alloc();
        const char *rel_str = (what_field_type == WHATPROVIDES ? "provides" : "replaces");
        int i;

        pkg_info_preinstall_check();

        if (opkg_config->query_all)
            pkg_hash_fetch_available(available_pkgs);
        else
            pkg_hash_fetch_all_installed(available_pkgs);
        for (i = 0; i < argc; i++) {
            const char *target = argv[i];
            unsigned int j;

            opkg_msg(NOTICE, "What %s %s\n", rel_str, target);
            for (j = 0; j < available_pkgs->len; j++) {
                pkg_t *pkg = available_pkgs->pkgs[j];
                int k;
                int count = (what_field_type == WHATPROVIDES) ? pkg->provides_count : pkg->replaces_count;
                for (k = 0; k < count; k++) {
                    abstract_pkg_t *apkg = ((what_field_type == WHATPROVIDES) ? pkg->provides[k] : pkg->replaces[k]);
                    if (fnmatch(target, apkg->name, 0) == 0) {
                        opkg_msg(NOTICE, "    %s", pkg->name);
                        if (strcmp(target, apkg->name) != 0)
                            opkg_msg(NOTICE, "\t%s %s\n", rel_str, apkg->name);
                        opkg_message(NOTICE, "\n");
                    }
                }
            }
        }
        pkg_vec_free(available_pkgs);
    }
    return 0;
#endif
}

static int opkg_whatprovides_cmd(int argc, char **argv)
{
    return opkg_what_provides_replaces_cmd(WHATPROVIDES, argc, argv);
}

static int opkg_whatreplaces_cmd(int argc, char **argv)
{
    return opkg_what_provides_replaces_cmd(WHATREPLACES, argc, argv);
}

static int opkg_search_cmd(int argc, char **argv)
{
    //TODO: implement through SOLV
    /*
    unsigned int i;

    pkg_vec_t *installed;
    pkg_t *pkg;
    str_list_t *installed_files;
    str_list_elt_t *iter;
    char *installed_file;

    if (argc < 1) {
        return -1;
    }

    installed = pkg_vec_alloc();
    pkg_hash_fetch_all_installed(installed);
    pkg_vec_sort(installed, pkg_compare_names);

    for (i = 0; i < installed->len; i++) {
        pkg = installed->pkgs[i];

        installed_files = pkg_get_installed_files(pkg);

        for (iter = str_list_first(installed_files); iter;
                iter = str_list_next(installed_files, iter)) {
            installed_file = (char *)iter->data;
            if (fnmatch(argv[0], installed_file, 0) == 0)
                print_pkg(pkg);
        }

        pkg_free_installed_files(pkg);
    }

    pkg_vec_free(installed);
*/
    return 0;
}

static int opkg_compare_versions_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    if (argc == 3) {
        /* this is a bit gross */
        struct pkg p1, p2;
        parse_version(&p1, argv[0]);
        parse_version(&p2, argv[2]);
        return pkg_version_satisfied(&p1, &p2, argv[1]);
    } else {
        opkg_msg(ERROR,
                 "opkg compare_versions <v1> <op> <v2>\n"
                 "<op> is one of <= >= << >> =\n");
        return -1;
    }
#endif
}

static int opkg_print_architecture_cmd(int argc, char **argv)
{
//TODO: IMPLEMENT
#if 0
    nv_pair_list_elt_t *l;

    list_for_each_entry(l, &opkg_config->arch_list.head, node) {
        nv_pair_t *nv = (nv_pair_t *) l->data;
        printf("arch %s %s\n", nv->name, nv->value);
    }
    return 0;
#endif
}

/* XXX: CLEANUP: The usage strings should be incorporated into this
   array for easier maintenance */
static opkg_cmd_t cmds[] = {
    {"update", 0, (opkg_cmd_fun_t) opkg_update_cmd},
    {"upgrade", 0, (opkg_cmd_fun_t) opkg_upgrade_cmd},
    {"list", 0, (opkg_cmd_fun_t) opkg_list_cmd},
    {"list_installed", 0, (opkg_cmd_fun_t) opkg_list_installed_cmd},
    {"list-installed", 0, (opkg_cmd_fun_t) opkg_list_installed_cmd},
    {"list_upgradable", 0, (opkg_cmd_fun_t) opkg_list_upgradable_cmd},
    {"list-upgradable", 0, (opkg_cmd_fun_t) opkg_list_upgradable_cmd},
    {"list_changed_conffiles", 0,
        (opkg_cmd_fun_t) opkg_list_changed_conffiles_cmd},
    {"list-changed-conffiles", 0,
        (opkg_cmd_fun_t) opkg_list_changed_conffiles_cmd},
    {"info", 0, (opkg_cmd_fun_t) opkg_info_cmd},
    {"flag", 1, (opkg_cmd_fun_t) opkg_flag_cmd},
    {"status", 0, (opkg_cmd_fun_t) opkg_status_cmd},
    {"install", 1, (opkg_cmd_fun_t) opkg_install_cmd},
    {"remove", 1, (opkg_cmd_fun_t) opkg_remove_cmd},
    {"clean", 0, (opkg_cmd_fun_t) opkg_clean_cmd},
    {"configure", 0, (opkg_cmd_fun_t) opkg_configure_cmd},
    {"files", 1, (opkg_cmd_fun_t) opkg_files_cmd},
    {"search", 1, (opkg_cmd_fun_t) opkg_search_cmd},
    {"download", 1, (opkg_cmd_fun_t) opkg_download_cmd},
    {"compare_versions", 1, (opkg_cmd_fun_t) opkg_compare_versions_cmd},
    {"compare-versions", 1, (opkg_cmd_fun_t) opkg_compare_versions_cmd},
    {"print-architecture", 0, (opkg_cmd_fun_t) opkg_print_architecture_cmd},
    {"print_architecture", 0, (opkg_cmd_fun_t) opkg_print_architecture_cmd},
    {"print-installation-architecture", 0,
        (opkg_cmd_fun_t) opkg_print_architecture_cmd},
    {"print_installation_architecture", 0,
        (opkg_cmd_fun_t) opkg_print_architecture_cmd},
    {"depends", 1, (opkg_cmd_fun_t) opkg_depends_cmd},
    {"whatdepends", 1, (opkg_cmd_fun_t) opkg_whatdepends_cmd},
    {"whatdependsrec", 1, (opkg_cmd_fun_t) opkg_whatdepends_recursively_cmd},
    {"whatrecommends", 1, (opkg_cmd_fun_t) opkg_whatrecommends_cmd},
    {"whatsuggests", 1, (opkg_cmd_fun_t) opkg_whatsuggests_cmd},
    {"whatprovides", 1, (opkg_cmd_fun_t) opkg_whatprovides_cmd},
    {"whatreplaces", 1, (opkg_cmd_fun_t) opkg_whatreplaces_cmd},
    {"whatconflicts", 1, (opkg_cmd_fun_t) opkg_whatconflicts_cmd},
};

opkg_cmd_t *opkg_cmd_find(const char *name)
{
    int i;
    opkg_cmd_t *cmd;
    int num_cmds = sizeof(cmds) / sizeof(opkg_cmd_t);

    for (i = 0; i < num_cmds; i++) {
        cmd = &cmds[i];
        if (strcmp(name, cmd->name) == 0)
            return cmd;
    }

    return NULL;
}

int opkg_cmd_exec(opkg_cmd_t * cmd, int argc, const char **argv)
{
    return (cmd->fun) (argc, argv);
}
