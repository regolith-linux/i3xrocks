/*
 * ini.c - generic INI parser
 * Copyright (C) 2017-2019  Vivien Didelot
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

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "ini.h"
#include "line.h"
#include "log.h"

struct ini {
	ini_sec_cb_t *sec_cb;
	ini_prop_cb_t *prop_cb;
	void *data;
	unsigned int skipped;
	bool orphan;
};

static int ini_section(struct ini *ini, char *section)
{
	if (!ini->sec_cb)
		return 0;

	return ini->sec_cb(section, ini->data);
}

static int ini_property(struct ini *ini, char *key, char *value)
{
	if (!ini->prop_cb)
		return 0;

	return ini->prop_cb(key, value, ini->data);
}

/* Count an invalid line; after an invalid section, ignore its properties */
static int ini_skip(struct ini *ini, bool section)
{
	ini->skipped++;

	if (section)
		ini->orphan = true;

	return 0;
}

static int ini_parse_line(char *line, size_t num, void *data)
{
	struct ini *ini = data;

	/* comment or empty line? */
	if (*line == '\0' || *line == '#')
		return 0;

	/* section? */
	if (*line == '[') {
		char *closing, *section;

		closing = strchr(line, ']');
		if (!closing) {
			error("line %zu: malformed section \"%s\", ignoring it",
			      num + 1, line);
			return ini_skip(ini, true);
		}

		if (*(closing + 1) != '\0') {
			error("line %zu: trailing characters \"%s\", ignoring the section",
			      num + 1, closing);
			return ini_skip(ini, true);
		}

		section = line + 1;
		*closing = '\0';
		ini->orphan = false;

		return ini_section(ini, section);
	}

	/* property? */
	if (isalpha(*line) || *line == '_') {
		char *equals, *key, *value;

		equals = strchr(line, '=');
		if (!equals) {
			error("line %zu: malformed property \"%s\", should be a key=value pair",
			      num + 1, line);
			return ini_skip(ini, false);
		}

		/* Neither the previous section nor the globals get these */
		if (ini->orphan) {
			debug("line %zu: ignoring property \"%s\" of an invalid section",
			      num + 1, line);
			return 0;
		}

		*equals = '\0';
		key = line;
		value = equals + 1;

		return ini_property(ini, key, value);
	}

	error("line %zu: invalid INI syntax \"%s\", ignoring it", num + 1, line);
	return ini_skip(ini, false);
}

int ini_read(int fd, size_t count, ini_sec_cb_t *sec_cb, ini_prop_cb_t *prop_cb,
		 void *data)
{
	struct ini ini = {
		.sec_cb = sec_cb,
		.prop_cb = prop_cb,
		.data = data,
	};
	int err;

	err = line_read(fd, count, ini_parse_line, &ini);
	if (err && err != -EAGAIN)
		return err;

	return ini.skipped;
}
