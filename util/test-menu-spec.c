/*
 * Copyright (C) 2004 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <config.h>
#include <glib/gi18n.h>
#include <locale.h>

#include "cafemenu-tree.h"

#include <string.h>

static char     *menu_file = NULL;
static gboolean  monitor = FALSE;
static gboolean  include_excluded = FALSE;
static gboolean  include_nodisplay = FALSE;
static gboolean  include_unallocated = FALSE;

static GOptionEntry options[] = {
	{ "file",                'f', 0, G_OPTION_ARG_STRING, &menu_file,           N_("Menu file"),                      N_("MENU_FILE") },
	{ "monitor",             'm', 0, G_OPTION_ARG_NONE,   &monitor,             N_("Monitor for menu changes"),       NULL },
	{ "include-excluded",    'i', 0, G_OPTION_ARG_NONE,   &include_excluded,    N_("Include <Exclude>d entries"),     NULL },
	{ "include-nodisplay",   'n', 0, G_OPTION_ARG_NONE,   &include_nodisplay,   N_("Include NoDisplay=true entries"), NULL },
	{ "include-unallocated", 'u', 0, G_OPTION_ARG_NONE,   &include_unallocated, N_("Include unallocated entries"), NULL },
	{ NULL }
};

static void
append_directory_path (CafeMenuTreeDirectory *directory,
		GString            *path)
{
	CafeMenuTreeDirectory *parent;

	parent = cafemenu_tree_directory_get_parent(directory);

	if (!parent)
	{
		g_string_append_c(path, '/');
		return;
	}

	append_directory_path(parent, path);

	g_string_append(path, cafemenu_tree_directory_get_name (directory));
	g_string_append_c(path, '/');

	cafemenu_tree_item_unref(parent);
}

static char *
make_path (CafeMenuTreeDirectory *directory)
{
	GString *path;

	g_return_val_if_fail(directory != NULL, NULL);

	path = g_string_new(NULL);

	append_directory_path(directory, path);

	return g_string_free(path, FALSE);
}

static void
print_entry (CafeMenuTreeEntry *entry,
		const char     *path)
{
	char *utf8_path;
	char *utf8_file_id;

	utf8_path = g_filename_to_utf8(cafemenu_tree_entry_get_desktop_file_path (entry),
			-1, NULL, NULL, NULL);

	utf8_file_id = g_filename_to_utf8(cafemenu_tree_entry_get_desktop_file_id (entry),
			-1, NULL, NULL, NULL);

	g_print("%s\t%s\t%s%s\n",
			path,
			utf8_file_id ? utf8_file_id : _("Invalid desktop file ID"),
			utf8_path ? utf8_path : _("[Invalid Filename]"),
			cafemenu_tree_entry_get_is_excluded(entry) ? _(" <excluded>") : "");

	g_free(utf8_file_id);
	g_free(utf8_path);
}

static void
print_directory(CafeMenuTreeDirectory *directory)
{
	CafeMenuTreeIter *iter;
	const char *path;
	char       *freeme;

	freeme = make_path(directory);
	if (!strcmp (freeme, "/"))
		path = freeme;
	else
		path = freeme + 1;

	iter = cafemenu_tree_directory_iter(directory);

	while(TRUE)
	{
		gpointer item;

		switch (cafemenu_tree_iter_next (iter))
		{
			case CAFEMENU_TREE_ITEM_INVALID:
				goto done;

			case CAFEMENU_TREE_ITEM_ENTRY:
				item = cafemenu_tree_iter_get_entry(iter);
				print_entry((CafeMenuTreeEntry*)item, path);
				break;

			case CAFEMENU_TREE_ITEM_DIRECTORY:
				item = cafemenu_tree_iter_get_directory(iter);
				print_directory((CafeMenuTreeDirectory*)item);
				break;

			case CAFEMENU_TREE_ITEM_HEADER:
			case CAFEMENU_TREE_ITEM_SEPARATOR:
				item = NULL;
				break;

			case CAFEMENU_TREE_ITEM_ALIAS:
				{
					item = cafemenu_tree_iter_get_alias(iter);

					if (cafemenu_tree_alias_get_aliased_item_type (item) == CAFEMENU_TREE_ITEM_ENTRY)
					{
						CafeMenuTreeEntry *entry = cafemenu_tree_alias_get_aliased_entry(item);
						print_entry(entry, path);
						cafemenu_tree_item_unref(entry);
					}
				}
				break;

			default:
				g_assert_not_reached();
				break;
		}

		cafemenu_tree_item_unref(item);
		continue;
done:
		break;
	}

	cafemenu_tree_iter_unref(iter);

	g_free(freeme);
}

static void
handle_tree_changed (CafeMenuTree *tree)
{
	CafeMenuTreeDirectory *root;
	GError *error = NULL;

	g_print(_("\n\n\n==== Menu changed, reloading ====\n\n\n"));

	if(!cafemenu_tree_load_sync (tree, &error))
	{
		g_printerr("Failed to load tree: %s\n", error->message);
		g_clear_error(&error);
		return;
	}

	root = cafemenu_tree_get_root_directory(tree);
	if (root == NULL)
	{
		g_warning(_("Menu tree is empty"));
		return;
	}

	print_directory(root);
	cafemenu_tree_item_unref(root);
}

int
main (int argc, char **argv)
{
	GOptionContext    *options_context;
	CafeMenuTree          *tree;
	CafeMenuTreeDirectory *root;
	CafeMenuTreeFlags      flags;
	GError             *error = NULL;

	setlocale(LC_ALL, "");
	options_context = g_option_context_new(_("- test CAFE's implementation of the Desktop Menu Specification"));
	g_option_context_add_main_entries(options_context, options, GETTEXT_PACKAGE);
	g_option_context_parse(options_context, &argc, &argv, NULL);
	g_option_context_free(options_context);

	flags = CAFEMENU_TREE_FLAGS_NONE;
	if (include_excluded)
		flags |= CAFEMENU_TREE_FLAGS_INCLUDE_EXCLUDED;
	if (include_nodisplay)
		flags |= CAFEMENU_TREE_FLAGS_INCLUDE_NODISPLAY;
	if (include_unallocated)
		flags |= CAFEMENU_TREE_FLAGS_INCLUDE_UNALLOCATED;

	tree = cafemenu_tree_new(menu_file ? menu_file : "cafe-applications.menu", flags);
	g_assert(tree != NULL);

	if (!cafemenu_tree_load_sync (tree, &error))
	{
		g_printerr("Failed to load tree: %s\n", error->message);
		return 1;
	}

	g_print("Loaded menu from %s\n", cafemenu_tree_get_canonical_menu_path(tree));

	root = cafemenu_tree_get_root_directory(tree);
	if (root != NULL)
	{
		print_directory(root);
		cafemenu_tree_item_unref(root);
	}
	else
	{
		g_warning(_("Menu tree is empty"));
	}

	if (monitor)
	{
		GMainLoop *main_loop;

		g_signal_connect(tree, "changed", G_CALLBACK(handle_tree_changed), NULL);

		main_loop = g_main_loop_new(NULL, FALSE);
		g_main_loop_run(main_loop);
		g_main_loop_unref(main_loop);
	}

	g_object_unref(tree);

	return 0;
}
