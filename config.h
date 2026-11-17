/*
 * config.h - configuration file header
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

#ifndef CONFIG_H
#define CONFIG_H

#include "map.h"

typedef int config_cb_t(struct map *map, void *data);
/* Called with the path of each file that was skipped or partially loaded */
typedef int config_skip_cb_t(const char *path, void *data);
int config_load(const char *path, config_cb_t *cb, config_skip_cb_t *skip_cb,
		void *data);
int config_dir_load(const char *path, config_cb_t *cb, config_skip_cb_t *skip_cb,
		    void *data, const bool quiet);

#endif /* CONFIG_H */
