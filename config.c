/*
 * config.c - parsing of the configuration file
 * Copyright (C) 2014-2019  Vivien Didelot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "ini.h"
#include "log.h"
#include "map.h"
#include "sys.h"

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

struct pending {
	struct map *map;	/* section with inherited globals */
	struct map *own;	/* keys written in the section itself */
	char *cmd_dir;		/* directory of the file that wrote "command" */
	bool user;
	bool dropped;
	struct pending *next;
};

struct config {
	struct map *section;
	struct map *own;
	struct map *global;
	config_cb_t *cb;
	void *data;

	char dir[PATH_MAX];		/* directory of the file being read */
	char global_cmd_dir[PATH_MAX];
	char section_cmd_dir[PATH_MAX];

	/* layered mode: collect sections, resolve, then emit */
	bool collect;
	bool user;
	struct pending *head, *tail;
};

static int config_finalize(struct config *conf)
{
	int err;

	if (!conf->section)
		return 0;

	if (conf->collect) {
		struct pending *p = calloc(1, sizeof(*p));
		if (!p)
			return -ENOMEM;
		p->cmd_dir = strdup(conf->section_cmd_dir);
		if (!p->cmd_dir) {
			free(p);
			return -ENOMEM;
		}
		p->map = conf->section;
		p->own = conf->own;
		p->user = conf->user;
		if (conf->tail)
			conf->tail->next = p;
		else
			conf->head = p;
		conf->tail = p;
	} else {
		map_destroy(conf->own);
		if (conf->cb) {
			err = conf->cb(conf->section, conf->section_cmd_dir, conf->data);
			if (err) {
				conf->section = NULL;
				conf->own = NULL;
				return err;
			}
		} else {
			map_destroy(conf->section);
		}
	}

	conf->section = NULL;
	conf->own = NULL;

	return 0;
}

static int config_reset(struct config *conf)
{
	conf->section = map_create();
	if (!conf->section)
		return -ENOMEM;

	conf->own = map_create();
	if (!conf->own)
		return -ENOMEM;

	strcpy(conf->section_cmd_dir, conf->global_cmd_dir);

	if (conf->global)
		return map_copy(conf->section, conf->global);

	return 0;
}

static int config_set(struct config *conf, const char *key, const char *value)
{
	struct map *map = conf->section;
	int err;

	if (!map) {
		if (!conf->global) {
			conf->global = map_create();
			if (!conf->global)
				return -ENOMEM;
		}

		if (strcmp(key, "command") == 0)
			strcpy(conf->global_cmd_dir, conf->dir);

		return map_set(conf->global, key, value);
	}

	if (strcmp(key, "command") == 0)
		strcpy(conf->section_cmd_dir, conf->dir);

	err = map_set(conf->own, key, value);
	if (err)
		return err;

	return map_set(map, key, value);
}

static int config_ini_section_cb(char *section, void *data)
{
	int err;

	err = config_finalize(data);
	if (err)
		return err;

	err = config_reset(data);
	if (err)
		return err;

	return config_set(data, "name", section);
}

static int config_ini_property_cb(char *key, char *value, void *data)
{
	return config_set(data, key, value);
}

static int config_read(struct config *conf, int fd)
{
	int err;

	err = ini_read(fd, -1, config_ini_section_cb, config_ini_property_cb,
		       conf);
	if (err && err != -EAGAIN)
		return err;

	return config_finalize(conf);
}

static int config_open(struct config *conf, const char *path, const bool single_mode)
{
	size_t plen = strlen(path);
	char pname[plen + 1];
	char *dname;
	int err;
	int fd;

	debug("try file %s", path);

	err = sys_open(path, &fd);
	if (err)
		return err;

	strcpy(pname, path);
	dname = dirname(pname);
	err = sys_chdir(dname);
	if (err) {
		error("failed to change directory to %s", dname);
		return err;
	}

	if (!getcwd(conf->dir, sizeof(conf->dir)))
		snprintf(conf->dir, sizeof(conf->dir), "%s", dname);

	debug("changed directory to %s", dname);

	err = config_read(conf, fd);
	sys_close(fd);

	if (single_mode && conf->global) {
		map_destroy(conf->global);
		conf->global = NULL;
	}

	return err;
}

int config_load(const char *path, config_cb_t *cb, void *data)
{
	const char * const home = sys_getenv("HOME");
	const char * const xdg_home = sys_getenv("XDG_CONFIG_HOME");
	const char * const xdg_dirs = sys_getenv("XDG_CONFIG_DIRS");
	struct config conf = {
		.data = data,
		.cb = cb,
	};
	char buf[PATH_MAX];
	int err;


	/* command line config file? */
	if (path)
		return config_open(&conf, path, true);

	/* user config file? */
	if (home) {
		if (xdg_home)
			snprintf(buf, sizeof(buf), "%s/i3xrocks/config", xdg_home);
		else
			snprintf(buf, sizeof(buf), "%s/.config/i3xrocks/config", home);
		err = config_open(&conf, buf, true);
		if (err != -ENOENT)
			return err;

		snprintf(buf, sizeof(buf), "%s/.i3xrocks.conf", home);
		err = config_open(&conf, buf, true);
		if (err != -ENOENT)
			return err;
	}

	/* system config file? */
	if (xdg_dirs)
		snprintf(buf, sizeof(buf), "%s/i3xrocks/config", xdg_dirs);
	else
		snprintf(buf, sizeof(buf), "%s/xdg/i3xrocks/config", SYSCONFDIR);
	err = config_open(&conf, buf, true);
	if (err != -ENOENT)
		return err;

	snprintf(buf, sizeof(buf), "%s/i3xrocks.conf", SYSCONFDIR);
	return config_open(&conf, buf, true);
}

struct entry {
	char name[NAME_MAX + 1];
	char dir[PATH_MAX];
	bool user;
};

static int entry_cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	int c = strcmp(x->name, y->name);

	if (c)
		return c;
	/* user entry first, so the package one can be skipped */
	return (int)y->user - (int)x->user;
}

static int config_scan(const char *path, bool user, bool quiet,
		       struct entry **list, size_t *len, size_t *cap)
{
	struct dirent **namelist;
	char real[PATH_MAX];
	int n;

	n = scandir(path, &namelist, NULL, alphasort);
	if (n < 0) {
		if (!quiet)
			perror(path);
		return -1;
	}

	if (!realpath(path, real))
		snprintf(real, sizeof(real), "%s", path);

	for (int i = 0; i < n; i++) {
		const char *name = namelist[i]->d_name;

		if (strcmp(name, ".") && strcmp(name, "..")) {
			if (*len == *cap) {
				size_t ncap = *cap ? *cap * 2 : 32;
				struct entry *nl = realloc(*list, ncap * sizeof(**list));
				if (!nl) {
					while (i < n)
						free(namelist[i++]);
					free(namelist);
					return -ENOMEM;
				}
				*list = nl;
				*cap = ncap;
			}
			snprintf((*list)[*len].name, sizeof((*list)[*len].name), "%s", name);
			snprintf((*list)[*len].dir, sizeof((*list)[*len].dir), "%s", real);
			(*list)[*len].user = user;
			(*len)++;
		}
		free(namelist[i]);
	}
	free(namelist);

	return 0;
}

static bool same_block(const struct map *a, const struct map *b)
{
	const char *an = map_get(a, "name"), *bn = map_get(b, "name");
	const char *ai = map_get(a, "instance"), *bi = map_get(b, "instance");

	if (!an || !bn || strcmp(an, bn))
		return false;

	return strcmp(ai ? ai : "", bi ? bi : "") == 0;
}

static int overlay_cb(const char *key, const char *value, void *data)
{
	return map_set(data, key, value);
}

static int config_resolve(struct config *conf)
{
	for (struct pending *u = conf->head; u; u = u->next) {
		struct pending *base = NULL;

		if (!u->user)
			continue;

		for (struct pending *p = conf->head; p; p = p->next) {
			if (p->user || p->dropped || !same_block(p->map, u->map))
				continue;
			p->dropped = true;
			base = p;
		}

		if (base) {
			struct map *merged = map_create();
			int err;

			if (!merged)
				return -ENOMEM;
			err = map_copy(merged, base->map);
			if (!err)
				err = map_for_each(u->own, overlay_cb, merged);
			if (err) {
				map_destroy(merged);
				return err;
			}
			if (!map_get(u->own, "command")) {
				char *dir = strdup(base->cmd_dir);

				if (!dir) {
					map_destroy(merged);
					return -ENOMEM;
				}
				free(u->cmd_dir);
				u->cmd_dir = dir;
			}
			map_destroy(u->map);
			u->map = merged;
		}
	}

	return 0;
}

static void config_free_pending(struct config *conf)
{
	struct pending *p = conf->head, *next;

	while (p) {
		next = p->next;
		if (p->map)
			map_destroy(p->map);
		map_destroy(p->own);
		free(p->cmd_dir);
		free(p);
		p = next;
	}
	conf->head = conf->tail = NULL;
}

int config_layers_load(const char *user_dir, const char *pkg_dir,
		       config_cb_t *cb, void *data)
{
	struct config conf = {
		.data = data,
		.cb = cb,
		.collect = true,
	};
	struct entry *list = NULL;
	size_t len = 0, cap = 0;
	int found = 0, err = 0;
	char buf[PATH_MAX];

	if (user_dir && config_scan(user_dir, true, true, &list, &len, &cap) == 0)
		found++;
	if (pkg_dir && config_scan(pkg_dir, false, false, &list, &len, &cap) == 0)
		found++;

	if (!found) {
		free(list);
		return -1;
	}

	qsort(list, len, sizeof(*list), entry_cmp);

	for (size_t i = 0; i < len; i++) {
		if (i > 0 && strcmp(list[i].name, list[i - 1].name) == 0)
			continue; /* masked by the user file of the same name */

		snprintf(buf, sizeof(buf), "%s/%s", list[i].dir, list[i].name);
		debug("Reading config file %s", buf);
		conf.user = list[i].user;
		err = config_open(&conf, buf, false);
		if (err) {
			error("failed to load config file %s", buf);
			break;
		}
	}

	free(list);
	if (conf.global)
		map_destroy(conf.global);
	if (conf.section) {
		map_destroy(conf.section);
		map_destroy(conf.own);
	}

	if (!err)
		err = config_resolve(&conf);

	for (struct pending *p = conf.head; p && !err; p = p->next) {
		if (p->dropped)
			continue;
		err = cb(p->map, p->cmd_dir, data);
		p->map = NULL; /* owned by the callback now */
	}

	config_free_pending(&conf);

	return err;
}
