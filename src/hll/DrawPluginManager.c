/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <string.h>

#include "hll.h"
#include "system4/string.h"

static char **loaded_plugins;
static int nr_loaded_plugins;

static int draw_plugin_index(const char *plugin_name)
{
	for (int i = 0; i < nr_loaded_plugins; i++) {
		if (!strcmp(loaded_plugins[i], plugin_name))
			return i;
	}
	return -1;
}

static int DrawPluginManager_Load(struct string *plugin_name)
{
	if (!plugin_name || !plugin_name->text || !*plugin_name->text)
		return 0;
	if (draw_plugin_index(plugin_name->text) >= 0)
		return 1;
	loaded_plugins = xrealloc(loaded_plugins, sizeof(char *) * (nr_loaded_plugins + 1));
	loaded_plugins[nr_loaded_plugins++] = strdup(plugin_name->text);
	NOTICE("DrawPluginManager loaded plugin: %s", plugin_name->text);
	return 1;
}

static int DrawPluginManager_IsLoad(struct string *plugin_name)
{
	if (!plugin_name || !plugin_name->text || !*plugin_name->text)
		return 0;
	return draw_plugin_index(plugin_name->text) >= 0;
}

HLL_LIBRARY(DrawPluginManager,
	    HLL_EXPORT(Load, DrawPluginManager_Load),
	    HLL_EXPORT(IsLoad, DrawPluginManager_IsLoad));
