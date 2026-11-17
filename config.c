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
#include <string.h>
#include <dirent.h>

#include "config.h"
#include "ini.h"
#include "log.h"
#include "map.h"
#include "sys.h"

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

struct config {
	struct map *section;
	struct map *global;
	config_cb_t *cb;
	config_skip_cb_t *skip_cb;
	void *data;
};

static int config_finalize(struct config *conf)
{
	int err;

	if (conf->section) {
		if (conf->cb) {
			err = conf->cb(conf->section, conf->data);
			if (err)
				return err;
		}

		conf->section = NULL;
	}

	return 0;
}

static int config_reset(struct config *conf)
{
	conf->section = map_create();
	if (!conf->section)
		return -ENOMEM;

	if (conf->global)
		return map_copy(conf->section, conf->global);

	return 0;
}

static int config_set(struct config *conf, const char *key, const char *value)
{
	struct map *map = conf->section;

	if (!map) {
		if (!conf->global) {
			conf->global = map_create();
			if (!conf->global)
				return -ENOMEM;
		}

		map = conf->global;
	}

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

static int config_skip(struct config *conf, const char *path)
{
	if (!conf->skip_cb)
		return 0;

	return conf->skip_cb(path, conf->data);
}

/* Return the number of invalid lines ignored, or a negative error code */
static int config_read(struct config *conf, int fd)
{
	int skipped;
	int err;

	skipped = ini_read(fd, -1, config_ini_section_cb, config_ini_property_cb,
			   conf);
	if (skipped < 0) {
		/* Drop an incomplete section, so that the next file does not extend it */
		if (conf->section) {
			map_destroy(conf->section);
			conf->section = NULL;
		}

		return skipped;
	}

	err = config_finalize(conf);
	if (err)
		return err;

	return skipped;
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

	debug("changed directory to %s", dname);

	err = config_read(conf, fd);
	sys_close(fd);

	if (single_mode && conf->global) {
		map_destroy(conf->global);
		conf->global = NULL;
	}

	if (err > 0) {
		error("ignored %d invalid line(s) in %s", err, path);
		err = config_skip(conf, path);
	}

	return err;
}

int config_load(const char *path, config_cb_t *cb, config_skip_cb_t *skip_cb,
		void *data)
{
	const char * const home = sys_getenv("HOME");
	const char * const xdg_home = sys_getenv("XDG_CONFIG_HOME");
	const char * const xdg_dirs = sys_getenv("XDG_CONFIG_DIRS");
	struct config conf = {
		.data = data,
		.cb = cb,
		.skip_cb = skip_cb,
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

int config_dir_load(const char *path, config_cb_t *cb, config_skip_cb_t *skip_cb,
		    void *data, const bool quiet)
{
	struct config conf = {
		.data = data,
		.cb = cb,
		.skip_cb = skip_cb,
	};

	char buf[PATH_MAX];
	struct dirent **namelist;
	int n, err, retval = 0;

	n = scandir(path, &namelist, NULL, alphasort);
	if (n < 0) {
		if (!quiet) perror(path);
		retval = -1;
	} else {
		for (int i=0; i < n; i++) {			
			char *conf_file = namelist[i]->d_name;
			if (!retval && strcmp(conf_file, ".") != 0 && strcmp(conf_file, "..") != 0) {
				snprintf(buf, sizeof(buf), "%s/%s", path, conf_file);
				debug("Reading config file %s\n", buf);
				err = config_open(&conf, buf, false);

				/* An unreadable file must not discard the others */
				if (err) {
					error("failed to load config file %s, skipping it", conf_file);
					retval = config_skip(&conf, buf);
				}
			}
			free(namelist[i]);
		}
		free(namelist);
	}

	if (conf.global)
		map_destroy(conf.global);

    return retval;
}
