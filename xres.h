/*
 * xres.h - header for the X resources lookup
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

#ifndef XRES_H
#define XRES_H

#include <stdbool.h>

/* Start the delay given to the X server to provide its resources */
int xres_init(void);
/* Return the non-empty value of an X resource, to be freed, or NULL */
char *xres_get(const char *name);
/* Return a descriptor to watch while the helper looks for a display, or -1 */
int xres_watch(void);
/* Read from the helper; true if the resources of a display were adopted */
bool xres_update(void);
void xres_free(void);

#endif /* XRES_H */
