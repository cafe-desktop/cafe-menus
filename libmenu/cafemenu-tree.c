/*
 * Copyright (C) 2003, 2004 Red Hat, Inc.
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

#include "cafemenu-tree.h"

#include <gio/gio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "menu-layout.h"
#include "menu-monitor.h"
#include "menu-util.h"

/* private */
typedef struct CafeMenuTreeItem CafeMenuTreeItem;
#define CAFEMENU_TREE_ITEM(i)      ((CafeMenuTreeItem *)(i))
#define CAFEMENU_TREE_DIRECTORY(i) ((CafeMenuTreeDirectory *)(i))
#define CAFEMENU_TREE_ENTRY(i)     ((CafeMenuTreeEntry *)(i))
#define CAFEMENU_TREE_SEPARATOR(i) ((CafeMenuTreeSeparator *)(i))
#define CAFEMENU_TREE_HEADER(i)    ((CafeMenuTreeHeader *)(i))
#define CAFEMENU_TREE_ALIAS(i)     ((CafeMenuTreeAlias *)(i))

enum {
  PROP_0,

  PROP_MENU_BASENAME,
  PROP_MENU_PATH,
  PROP_FLAGS
};

/* Signals */
enum
{
  CHANGED,
  LAST_SIGNAL
};

static guint cafemenu_tree_signals [LAST_SIGNAL] = { 0 };

struct _CafeMenuTree
{
  GObject       parent_instance;

  char *basename;
  char *non_prefixed_basename;
  char *path;
  char *canonical_path;

  CafeMenuTreeFlags flags;

  GSList *menu_file_monitors;

  MenuLayoutNode *layout;
  CafeMenuTreeDirectory *root;
  GHashTable *entries_by_id;

  guint canonical : 1;
  guint loaded    : 1;
};

G_DEFINE_TYPE (CafeMenuTree, cafemenu_tree, G_TYPE_OBJECT)

struct CafeMenuTreeItem
{
  volatile gint refcount;

  CafeMenuTreeItemType type;

  CafeMenuTreeDirectory *parent;
  CafeMenuTree *tree;
};

struct CafeMenuTreeIter
{
  volatile gint refcount;

  CafeMenuTreeItem *item;
  GSList        *contents;
  GSList        *contents_iter;
};

struct CafeMenuTreeDirectory
{
	CafeMenuTreeItem item;

	DesktopEntry *directory_entry;
	char         *name;

	GSList *entries;
	GSList *subdirs;

	MenuLayoutValues  default_layout_values;
	GSList           *default_layout_info;
	GSList           *layout_info;
	GSList           *contents;

  guint only_unallocated : 1;
  guint is_nodisplay : 1;
  guint layout_pending_separator : 1;
  guint preprocessed : 1;

	/* 16 bits should be more than enough; G_MAXUINT16 means no inline header */
	guint will_inline_header : 16;
};

struct CafeMenuTreeEntry
{
  CafeMenuTreeItem item;

  DesktopEntry *desktop_entry;
  char         *desktop_file_id;

  guint is_excluded : 1;
  guint is_unallocated : 1;
};

struct CafeMenuTreeSeparator
{
  CafeMenuTreeItem item;
};

struct CafeMenuTreeHeader
{
  CafeMenuTreeItem item;

  CafeMenuTreeDirectory *directory;
};

struct CafeMenuTreeAlias
{
  CafeMenuTreeItem item;

  CafeMenuTreeDirectory *directory;
  CafeMenuTreeItem      *aliased_item;
};

static gboolean  cafemenu_tree_load_layout          (CafeMenuTree       *tree,
                                                  GError         **error);
static void      cafemenu_tree_force_reload         (CafeMenuTree       *tree);
static gboolean  cafemenu_tree_build_from_layout    (CafeMenuTree       *tree,
                                                  GError         **error);
static void      cafemenu_tree_force_rebuild        (CafeMenuTree       *tree);
static void      cafemenu_tree_resolve_files        (CafeMenuTree       *tree,
						  GHashTable      *loaded_menu_files,
						  MenuLayoutNode  *layout);
static void      cafemenu_tree_force_recanonicalize (CafeMenuTree       *tree);
static void      cafemenu_tree_invoke_monitors      (CafeMenuTree       *tree);

static void cafemenu_tree_item_unref_and_unset_parent (gpointer itemp);

typedef enum
{
  MENU_FILE_MONITOR_INVALID = 0,
  MENU_FILE_MONITOR_FILE,
  MENU_FILE_MONITOR_NONEXISTENT_FILE,
  MENU_FILE_MONITOR_DIRECTORY
} MenuFileMonitorType;

typedef struct
{
  MenuFileMonitorType  type;
  MenuMonitor         *monitor;
} MenuFileMonitor;

static void
handle_nonexistent_menu_file_changed (MenuMonitor      *monitor G_GNUC_UNUSED,
				      MenuMonitorEvent  event,
				      const char       *path G_GNUC_UNUSED,
				      CafeMenuTree     *tree)
{
  if (event == MENU_MONITOR_EVENT_CHANGED ||
      event == MENU_MONITOR_EVENT_CREATED)
    {
      menu_verbose ("\"%s\" %s, marking tree for recanonicalization\n",
                    path,
                    event == MENU_MONITOR_EVENT_CREATED ? "created" : "changed");

      cafemenu_tree_force_recanonicalize (tree);
      cafemenu_tree_invoke_monitors (tree);
    }
}

static void
handle_menu_file_changed (MenuMonitor      *monitor G_GNUC_UNUSED,
			  MenuMonitorEvent  event G_GNUC_UNUSED,
			  const char       *path G_GNUC_UNUSED,
			  CafeMenuTree     *tree)
{
  menu_verbose ("\"%s\" %s, marking tree for recanicalization\n",
		path,
		event == MENU_MONITOR_EVENT_CREATED ? "created" :
		event == MENU_MONITOR_EVENT_CHANGED ? "changed" : "deleted");

  cafemenu_tree_force_recanonicalize (tree);
  cafemenu_tree_invoke_monitors (tree);
}

static void
handle_menu_file_directory_changed (MenuMonitor      *monitor G_GNUC_UNUSED,
				    MenuMonitorEvent  event G_GNUC_UNUSED,
				    const char       *path,
				    CafeMenuTree     *tree)
{
  if (!g_str_has_suffix (path, ".menu"))
    return;

  menu_verbose ("\"%s\" %s, marking tree for recanicalization\n",
		path,
		event == MENU_MONITOR_EVENT_CREATED ? "created" :
		event == MENU_MONITOR_EVENT_CHANGED ? "changed" : "deleted");

  cafemenu_tree_force_recanonicalize (tree);
  cafemenu_tree_invoke_monitors (tree);
}

static void
cafemenu_tree_add_menu_file_monitor (CafeMenuTree           *tree,
				  const char          *path,
				  MenuFileMonitorType  type)
{
  MenuFileMonitor *monitor;

  monitor = g_slice_new0 (MenuFileMonitor);

  monitor->type = type;

  switch (type)
    {
    case MENU_FILE_MONITOR_FILE:
      menu_verbose ("Adding a menu file monitor for \"%s\"\n", path);

      monitor->monitor = menu_get_file_monitor (path);
      menu_monitor_add_notify (monitor->monitor,
			       (MenuMonitorNotifyFunc) handle_menu_file_changed,
			       tree);
      break;

    case MENU_FILE_MONITOR_NONEXISTENT_FILE:
      menu_verbose ("Adding a menu file monitor for non-existent \"%s\"\n", path);

      monitor->monitor = menu_get_file_monitor (path);
      menu_monitor_add_notify (monitor->monitor,
			       (MenuMonitorNotifyFunc) handle_nonexistent_menu_file_changed,
			       tree);
      break;

    case MENU_FILE_MONITOR_DIRECTORY:
      menu_verbose ("Adding a menu directory monitor for \"%s\"\n", path);

      monitor->monitor = menu_get_directory_monitor (path);
      menu_monitor_add_notify (monitor->monitor,
			       (MenuMonitorNotifyFunc) handle_menu_file_directory_changed,
			       tree);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  tree->menu_file_monitors = g_slist_prepend (tree->menu_file_monitors, monitor);
}

static void
remove_menu_file_monitor (MenuFileMonitor *monitor,
			  CafeMenuTree       *tree)
{
  switch (monitor->type)
    {
    case MENU_FILE_MONITOR_FILE:
      menu_monitor_remove_notify (monitor->monitor,
				  (MenuMonitorNotifyFunc) handle_menu_file_changed,
				  tree);
      break;

    case MENU_FILE_MONITOR_NONEXISTENT_FILE:
      menu_monitor_remove_notify (monitor->monitor,
				  (MenuMonitorNotifyFunc) handle_nonexistent_menu_file_changed,
				  tree);
      break;

    case MENU_FILE_MONITOR_DIRECTORY:
      menu_monitor_remove_notify (monitor->monitor,
				  (MenuMonitorNotifyFunc) handle_menu_file_directory_changed,
				  tree);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  menu_monitor_unref (monitor->monitor);
  monitor->monitor = NULL;

  monitor->type = MENU_FILE_MONITOR_INVALID;

  g_slice_free (MenuFileMonitor, monitor);
}

static void
cafemenu_tree_remove_menu_file_monitors (CafeMenuTree *tree)
{
  menu_verbose ("Removing all menu file monitors\n");

  g_slist_foreach (tree->menu_file_monitors,
                   (GFunc) remove_menu_file_monitor,
                   tree);
  g_slist_free (tree->menu_file_monitors);
  tree->menu_file_monitors = NULL;
}

static gboolean
canonicalize_path (CafeMenuTree  *tree,
                   const char *path)
{
  tree->canonical_path = realpath (path, NULL);
  if (tree->canonical_path)
    {
      tree->canonical = TRUE;
      cafemenu_tree_add_menu_file_monitor (tree,
					tree->canonical_path,
					MENU_FILE_MONITOR_FILE);
    }
  else
    {
      cafemenu_tree_add_menu_file_monitor (tree,
					path,
					MENU_FILE_MONITOR_NONEXISTENT_FILE);
    }

  return tree->canonical;
}

static gboolean
canonicalize_basename_with_config_dir (CafeMenuTree   *tree,
                                       const char *basename,
                                       const char *config_dir)
{
  gboolean  ret;
  char     *path;

  path = g_build_filename (config_dir, "menus",  basename,  NULL);
  ret = canonicalize_path (tree, path);
  g_free (path);

  return ret;
}

static void
canonicalize_basename (CafeMenuTree  *tree,
                       const char *basename)
{
  if (!canonicalize_basename_with_config_dir (tree,
                                              basename,
                                              g_get_user_config_dir ()))
    {
      const char * const *system_config_dirs;
      int                 i;

      system_config_dirs = g_get_system_config_dirs ();

      i = 0;
      while (system_config_dirs[i] != NULL)
        {
          if (canonicalize_basename_with_config_dir (tree,
                                                     basename,
                                                     system_config_dirs[i]))
            break;

          ++i;
        }
    }
}

static gboolean cafemenu_tree_canonicalize_path(CafeMenuTree* tree,
                              GError   **error)
{
  const char *menu_file = NULL;

  if (tree->canonical)
    return TRUE;

	g_assert(tree->canonical_path == NULL);

  cafemenu_tree_remove_menu_file_monitors (tree);

  if (tree->path)
    {
      menu_file = tree->path;
      canonicalize_path (tree, tree->path);
    }
  else
    {
      const gchar *xdg_menu_prefix;

      menu_file = tree->basename;
      xdg_menu_prefix = g_getenv ("XDG_MENU_PREFIX");

      if (xdg_menu_prefix != NULL)
        {
          gchar *prefixed_basename;

          prefixed_basename = g_strdup_printf ("%sapplications.menu",
                                               xdg_menu_prefix);

          /* Some gnome-menus using applications just use "applications.menu"
           * as the basename and expect gnome-menus to prefix it. Others (e.g.
           * Alacarte) explicitly use "${XDG_MENU_PREFIX}applications.menu" as
           * the basename, because they want to save changes to the right files
           * in ~. In both cases, we want to use "applications-merged" as the
           * merge directory (as required by the fd.o menu spec), so we save
           * the non-prefixed basename and use it later when calling
           * menu_layout_load().
           */
          if (!g_strcmp0 (tree->basename, "cafe-applications.menu") ||
              !g_strcmp0 (tree->basename, prefixed_basename))
            {
              canonicalize_basename (tree, prefixed_basename);
              g_free (tree->non_prefixed_basename);
              tree->non_prefixed_basename = g_strdup ("cafe-applications.menu");
            }
          g_free (prefixed_basename);
        }

      if (!tree->canonical)
        canonicalize_basename (tree, tree->basename);
    }

  if (tree->canonical)
    {
      menu_verbose ("Successfully looked up menu_file for \"%s\": %s\n",
                    menu_file, tree->canonical_path);
      return TRUE;
    }
  else
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to look up menu_file for \"%s\"\n",
                   menu_file);
      return FALSE;
    }
}

static void
cafemenu_tree_force_recanonicalize (CafeMenuTree *tree)
{
  cafemenu_tree_remove_menu_file_monitors (tree);

  if (tree->canonical)
    {
      cafemenu_tree_force_reload (tree);

      g_free (tree->canonical_path);
      tree->canonical_path = NULL;

      tree->canonical = FALSE;
    }
}

/**
 * cafemenu_tree_new:
 * @menu_basename: Basename of menu file
 * @flags: Flags controlling menu content
 *
 * Returns: (transfer full): A new #CafeMenuTree instance
 */
CafeMenuTree *
cafemenu_tree_new (const char     *menu_basename,
                CafeMenuTreeFlags  flags)
{
  g_return_val_if_fail (menu_basename != NULL, NULL);

  return g_object_new (CAFEMENU_TYPE_TREE,
                       "menu-basename", menu_basename,
                       "flags", flags,
                       NULL);
}

/**
 * cafemenu_tree_new_fo_path:
 * @menu_path: Path of menu file
 * @flags: Flags controlling menu content
 *
 * Returns: (transfer full): A new #CafeMenuTree instance
 */
CafeMenuTree *
cafemenu_tree_new_for_path (const char     *menu_path,
                         CafeMenuTreeFlags  flags)
{
  g_return_val_if_fail (menu_path != NULL, NULL);

  return g_object_new (CAFEMENU_TYPE_TREE,
                       "menu-path", menu_path,
                       "flags", flags,
                       NULL);
}

static GObject *
cafemenu_tree_constructor (GType                  type,
                        guint                  n_construct_properties,
                        GObjectConstructParam *construct_properties)
{
	GObject   *obj;
	CafeMenuTree *self;

	obj = G_OBJECT_CLASS (cafemenu_tree_parent_class)->constructor (type,
                                                                     n_construct_properties,
                                                                     construct_properties);

        /* If CafeMenuTree:menu-path is set, then we should make sure that
         * CafeMenuTree:menu-basename is unset (especially as it has a default
         * value). This has to be done here, in the constructor, since the
         * properties are construct-only. */

	self = CAFEMENU_TREE (obj);

        if (self->path != NULL)
          g_object_set (self, "menu-basename", NULL, NULL);

	return obj;
}

static void
cafemenu_tree_set_property (GObject         *object,
                         guint            prop_id,
                         const GValue    *value,
                         GParamSpec      *pspec)
{
  CafeMenuTree *self = CAFEMENU_TREE (object);

  switch (prop_id)
    {
    case PROP_MENU_BASENAME:
      self->basename = g_value_dup_string (value);
      break;

    case PROP_MENU_PATH:
      self->path = g_value_dup_string (value);
      break;

    case PROP_FLAGS:
      self->flags = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cafemenu_tree_get_property (GObject         *object,
                         guint            prop_id,
                         GValue          *value,
                         GParamSpec      *pspec)
{
  CafeMenuTree *self = CAFEMENU_TREE (object);

  switch (prop_id)
    {
    case PROP_MENU_BASENAME:
      g_value_set_string (value, self->basename);
      break;
    case PROP_MENU_PATH:
      g_value_set_string (value, self->path);
      break;
    case PROP_FLAGS:
      g_value_set_flags (value, self->flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cafemenu_tree_finalize (GObject *object)
{
  CafeMenuTree *tree = CAFEMENU_TREE (object);

  cafemenu_tree_force_recanonicalize (tree);

  if (tree->basename != NULL)
    g_free (tree->basename);
  tree->basename = NULL;

  g_free (tree->non_prefixed_basename);
  tree->non_prefixed_basename = NULL;

  if (tree->path != NULL)
    g_free (tree->path);
  tree->path = NULL;

  if (tree->canonical_path != NULL)
    g_free (tree->canonical_path);
  tree->canonical_path = NULL;

  g_hash_table_destroy (tree->entries_by_id);
  tree->entries_by_id = NULL;

  G_OBJECT_CLASS (cafemenu_tree_parent_class)->finalize (object);
}

static void
cafemenu_tree_init (CafeMenuTree *self)
{
  self->entries_by_id = g_hash_table_new (g_str_hash, g_str_equal);
}

static void
cafemenu_tree_class_init (CafeMenuTreeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructor = cafemenu_tree_constructor;
  gobject_class->get_property = cafemenu_tree_get_property;
  gobject_class->set_property = cafemenu_tree_set_property;
  gobject_class->finalize = cafemenu_tree_finalize;

  /**
   * CafeMenuTree:menu-basename:
   *
   * The name of the menu file; must be a basename or a relative path. The file
   * will be looked up in $XDG_CONFIG_DIRS/menus/. See the Desktop Menu
   * specification.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MENU_BASENAME,
                                   g_param_spec_string ("menu-basename", "", "",
                                                        "applications.menu",
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  /**
   * CafeMenuTree:menu-path:
   *
   * The full path of the menu file. If set, CafeMenuTree:menu-basename will get
   * ignored.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MENU_PATH,
                                   g_param_spec_string ("menu-path", "", "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  /**
   * CafeMenuTree:flags:
   *
   * Flags controlling the content of the menu.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_FLAGS,
                                   g_param_spec_flags ("flags", "", "",
                                                       CAFEMENU_TYPE_TREE_FLAGS,
                                                       CAFEMENU_TREE_FLAGS_NONE,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * CafeMenuTree:changed:
   *
   * This signal is emitted when applications are added, removed, or
   * upgraded.  But note the new data will only be visible after
   * cafemenu_tree_load_sync() or a variant thereof is invoked.
   */
  cafemenu_tree_signals[CHANGED] =
      g_signal_new ("changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0,
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);
}

/**
 * cafemenu_tree_get_canonical_menu_path:
 * @tree: a #CafeMenuTree
 *
 * This function is only available if the tree has been loaded via
 * cafemenu_tree_load_sync() or a variant thereof.
 *
 * Returns: The absolute and canonicalized path to the loaded menu file
 */
const char *
cafemenu_tree_get_canonical_menu_path (CafeMenuTree *tree)
{
  g_return_val_if_fail (CAFEMENU_IS_TREE (tree), NULL);
  g_return_val_if_fail (tree->loaded, NULL);

  return tree->canonical_path;
}

/**
 * cafemenu_tree_load_sync:
 * @tree: a #CafeMenuTree
 * @error: a #GError
 *
 * Synchronously load the menu contents.  This function
 * performs a significant amount of blocking I/O if the
 * tree has not been loaded yet.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
cafemenu_tree_load_sync (CafeMenuTree  *tree,
                      GError    **error)
{
  GError *local_error = NULL;

  if (tree->loaded)
    return TRUE;

  if (!cafemenu_tree_build_from_layout (tree, &local_error))
    {
      if (local_error)
        g_propagate_error (error, local_error);
      return FALSE;
    }

  tree->loaded = TRUE;

  return TRUE;
}

/**
 * cafemenu_tree_get_root_directory:
 * @tree: a #CafeMenuTree
 *
 * Get the root directory; you must have loaded the tree first (at
 * least once) via cafemenu_tree_load_sync() or a variant thereof.
 *
 * Returns: (transfer full): Root of the tree
 */
CafeMenuTreeDirectory *
cafemenu_tree_get_root_directory (CafeMenuTree *tree)
{
  g_return_val_if_fail (tree != NULL, NULL);
  g_return_val_if_fail (tree->loaded, NULL);

  return cafemenu_tree_item_ref (tree->root);
}

static CafeMenuTreeDirectory *
find_path (CafeMenuTreeDirectory *directory,
	   const char         *path)
{
  const char *name;
  char       *slash;
  char       *freeme;
  GSList     *tmp;

  while (path[0] == G_DIR_SEPARATOR) path++;

  if (path[0] == '\0')
    return directory;

  freeme = NULL;
  slash = strchr (path, G_DIR_SEPARATOR);
  if (slash)
    {
      name = freeme = g_strndup (path, slash - path);
      path = slash + 1;
    }
  else
    {
      name = path;
      path = NULL;
    }

  tmp = directory->contents;
  while (tmp != NULL)
    {
      CafeMenuTreeItem *item = tmp->data;

      if (item->type != CAFEMENU_TREE_ITEM_DIRECTORY)
        {
          tmp = tmp->next;
          continue;
        }

      if (!strcmp (name, CAFEMENU_TREE_DIRECTORY (item)->name))
	{
	  g_free (freeme);

	  if (path)
	    return find_path (CAFEMENU_TREE_DIRECTORY (item), path);
	  else
	    return CAFEMENU_TREE_DIRECTORY (item);
	}

      tmp = tmp->next;
    }

  g_free (freeme);

  return NULL;
}

CafeMenuTreeDirectory *
cafemenu_tree_get_directory_from_path (CafeMenuTree  *tree,
				    const char *path)
{
  CafeMenuTreeDirectory *root;
  CafeMenuTreeDirectory *directory;

  g_return_val_if_fail (tree != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  if (path[0] != G_DIR_SEPARATOR)
    return NULL;

  if (!(root = cafemenu_tree_get_root_directory (tree)))
    return NULL;

  directory = find_path (root, path);

  cafemenu_tree_item_unref (root);

  return directory ? cafemenu_tree_item_ref (directory) : NULL;
}

/**
 * cafemenu_tree_get_entry_by_id:
 * @tree: a #CafeMenuTree
 * @id: a desktop file ID
 *
 * Look up the entry corresponding to the given "desktop file id".
 *
 * Returns: (transfer full): A newly referenced #CafeMenuTreeEntry, or %NULL if none
 */
CafeMenuTreeEntry     *
cafemenu_tree_get_entry_by_id (CafeMenuTree  *tree,
			    const char *id)
{
  CafeMenuTreeEntry *entry;

  g_return_val_if_fail (tree->loaded, NULL);

  entry = g_hash_table_lookup (tree->entries_by_id, id);
  if (entry != NULL)
    cafemenu_tree_item_ref (entry);

  return entry;
}

static void
cafemenu_tree_invoke_monitors (CafeMenuTree *tree)
{
  g_signal_emit (tree, cafemenu_tree_signals[CHANGED], 0);
}

static CafeMenuTreeDirectory *
get_parent (CafeMenuTreeItem *item)
{
  g_return_val_if_fail (item != NULL, NULL);
  return item->parent ? cafemenu_tree_item_ref (item->parent) : NULL;
}

/**
 * cafemenu_tree_directory_get_parent:
 * @directory: a #CafeMenuTreeDirectory
 *
 * Returns: (transfer full): The parent directory, or %NULL if none
 */
CafeMenuTreeDirectory *
cafemenu_tree_directory_get_parent (CafeMenuTreeDirectory *directory)
{
  return get_parent ((CafeMenuTreeItem *)directory);
}

/**
 * cafemenu_tree_entry_get_parent:
 * @entry: a #CafeMenuTreeEntry
 *
 * Returns: (transfer full): The parent directory, or %NULL if none
 */
CafeMenuTreeDirectory *
cafemenu_tree_entry_get_parent (CafeMenuTreeEntry *entry)
{
  return get_parent ((CafeMenuTreeItem *)entry);
}

/**
 * cafemenu_tree_alias_get_parent:
 * @alias: a #CafeMenuTreeAlias
 *
 * Returns: (transfer full): The parent directory, or %NULL if none
 */
CafeMenuTreeDirectory *
cafemenu_tree_alias_get_parent (CafeMenuTreeAlias *alias)
{
  return get_parent ((CafeMenuTreeItem *)alias);
}

/**
 * cafemenu_tree_header_get_parent:
 * @header: a #CafeMenuTreeHeader
 *
 * Returns: (transfer full): The parent directory, or %NULL if none
 */
CafeMenuTreeDirectory *
cafemenu_tree_header_get_parent (CafeMenuTreeHeader *header)
{
  return get_parent ((CafeMenuTreeItem *)header);
}

/**
 * cafemenu_tree_separator_get_parent:
 * @separator: a #CafeMenuTreeSeparator
 *
 * Returns: (transfer full): The parent directory, or %NULL if none
 */
CafeMenuTreeDirectory *
cafemenu_tree_separator_get_parent (CafeMenuTreeSeparator *separator)
{
  return get_parent ((CafeMenuTreeItem *)separator);
}

static void
cafemenu_tree_item_set_parent (CafeMenuTreeItem      *item,
			    CafeMenuTreeDirectory *parent)
{
  g_return_if_fail (item != NULL);

  item->parent = parent;
}

/**
 * cafemenu_tree_iter_ref: (skip)
 * @iter: iter
 *
 * Increment the reference count of @iter
 */
CafeMenuTreeIter *
cafemenu_tree_iter_ref (CafeMenuTreeIter *iter)
{
  g_atomic_int_inc (&iter->refcount);
  return iter;
}

/**
 * cafemenu_tree_iter_unref: (skip)
 * @iter: iter
 *
 * Decrement the reference count of @iter
 */
void
cafemenu_tree_iter_unref (CafeMenuTreeIter *iter)
{
  if (!g_atomic_int_dec_and_test (&iter->refcount))
    return;

  g_slist_foreach (iter->contents, (GFunc)cafemenu_tree_item_unref, NULL);
  g_slist_free (iter->contents);

  g_slice_free (CafeMenuTreeIter, iter);
}

/**
 * cafemenu_tree_directory_iter:
 * @directory: directory
 *
 * Returns: (transfer full): A new iterator over the directory contents
 */
CafeMenuTreeIter *
cafemenu_tree_directory_iter (CafeMenuTreeDirectory *directory)
{
  CafeMenuTreeIter *iter;

  g_return_val_if_fail (directory != NULL, NULL);

  iter = g_slice_new0 (CafeMenuTreeIter);
  iter->refcount = 1;

  iter->contents = g_slist_copy (directory->contents);
  iter->contents_iter = iter->contents;
  g_slist_foreach (iter->contents, (GFunc) cafemenu_tree_item_ref, NULL);

  return iter;
}

/**
 * cafemenu_tree_iter_next:
 * @iter: iter
 *
 * Change the iterator to the next item, and return its type.  If
 * there are no more items, %CAFEMENU_TREE_ITEM_INVALID is returned.
 *
 * Returns: The type of the next item that can be retrived from the iterator
 */
CafeMenuTreeItemType
cafemenu_tree_iter_next (CafeMenuTreeIter *iter)
{
  g_return_val_if_fail (iter != NULL, CAFEMENU_TREE_ITEM_INVALID);

  if (iter->contents_iter)
    {
      iter->item = iter->contents_iter->data;
      iter->contents_iter = iter->contents_iter->next;
      return iter->item->type;
    }
  else
    return CAFEMENU_TREE_ITEM_INVALID;
}

/**
 * cafemenu_tree_iter_get_directory:
 * @iter: iter
 *
 * This method may only be called if cafemenu_tree_iter_next()
 * returned CAFEMENU_TREE_ITEM_DIRECTORY.
 *
 * Returns: (transfer full): A directory
 */
CafeMenuTreeDirectory *
cafemenu_tree_iter_get_directory (CafeMenuTreeIter *iter)
{
  g_return_val_if_fail (iter != NULL, NULL);
  g_return_val_if_fail (iter->item != NULL, NULL);
  g_return_val_if_fail (iter->item->type == CAFEMENU_TREE_ITEM_DIRECTORY, NULL);

  return (CafeMenuTreeDirectory*)cafemenu_tree_item_ref (iter->item);
}

/**
 * cafemenu_tree_iter_get_entry:
 * @iter: iter
 *
 * This method may only be called if cafemenu_tree_iter_next()
 * returned CAFEMENU_TREE_ITEM_ENTRY.
 *
 * Returns: (transfer full): An entry
 */
CafeMenuTreeEntry *
cafemenu_tree_iter_get_entry (CafeMenuTreeIter *iter)
{
  g_return_val_if_fail (iter != NULL, NULL);
  g_return_val_if_fail (iter->item != NULL, NULL);
  g_return_val_if_fail (iter->item->type == CAFEMENU_TREE_ITEM_ENTRY, NULL);

  return (CafeMenuTreeEntry*)cafemenu_tree_item_ref (iter->item);
}

/**
 * cafemenu_tree_iter_get_header:
 * @iter: iter
 *
 * This method may only be called if cafemenu_tree_iter_next()
 * returned CAFEMENU_TREE_ITEM_HEADER.
 *
 * Returns: (transfer full): A header
 */
CafeMenuTreeHeader *
cafemenu_tree_iter_get_header (CafeMenuTreeIter *iter)
{
  g_return_val_if_fail (iter != NULL, NULL);
  g_return_val_if_fail (iter->item != NULL, NULL);
  g_return_val_if_fail (iter->item->type == CAFEMENU_TREE_ITEM_HEADER, NULL);

  return (CafeMenuTreeHeader*)cafemenu_tree_item_ref (iter->item);
}

/**
 * cafemenu_tree_iter_get_alias:
 * @iter: iter
 *
 * This method may only be called if cafemenu_tree_iter_next()
 * returned CAFEMENU_TREE_ITEM_ALIAS.
 *
 * Returns: (transfer full): An alias
 */
CafeMenuTreeAlias *
cafemenu_tree_iter_get_alias (CafeMenuTreeIter *iter)
{
  g_return_val_if_fail (iter != NULL, NULL);
  g_return_val_if_fail (iter->item != NULL, NULL);
  g_return_val_if_fail (iter->item->type == CAFEMENU_TREE_ITEM_ALIAS, NULL);

  return (CafeMenuTreeAlias*)cafemenu_tree_item_ref (iter->item);
}

/**
 * cafemenu_tree_iter_get_separator:
 * @iter: iter
 *
 * This method may only be called if cafemenu_tree_iter_next()
 * returned #CAFEMENU_TREE_ITEM_SEPARATOR.
 *
 * Returns: (transfer full): A separator
 */
CafeMenuTreeSeparator *
cafemenu_tree_iter_get_separator (CafeMenuTreeIter *iter)
{
  g_return_val_if_fail (iter != NULL, NULL);
  g_return_val_if_fail (iter->item != NULL, NULL);
  g_return_val_if_fail (iter->item->type == CAFEMENU_TREE_ITEM_SEPARATOR, NULL);

  return (CafeMenuTreeSeparator*)cafemenu_tree_item_ref (iter->item);
}

const char *
cafemenu_tree_directory_get_name (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return directory->name;

  return desktop_entry_get_name (directory->directory_entry);
}

const char *
cafemenu_tree_directory_get_generic_name (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return NULL;

  return desktop_entry_get_generic_name (directory->directory_entry);
}

const char *
cafemenu_tree_directory_get_comment (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return NULL;

  return desktop_entry_get_comment (directory->directory_entry);
}

/**
 * cafemenu_tree_directory_get_icon:
 * @directory: a #CafeMenuTreeDirectory
 *
 * Gets the icon for the directory.
 *
 * Returns: (transfer none): The #GIcon for this directory
 */
GIcon *
cafemenu_tree_directory_get_icon (CafeMenuTreeDirectory *directory)
{
	g_return_val_if_fail(directory != NULL, NULL);

	if (!directory->directory_entry)
		return NULL;

	return desktop_entry_get_icon(directory->directory_entry);
}

const char *
cafemenu_tree_directory_get_desktop_file_path (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return NULL;

  return desktop_entry_get_path (directory->directory_entry);
}

const char *
cafemenu_tree_directory_get_menu_id (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  return directory->name;
}

gboolean
cafemenu_tree_directory_get_is_nodisplay (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, FALSE);

  return directory->is_nodisplay;
}

/**
 * cafemenu_tree_directory_get_tree:
 * @directory: A #CafeMenuTreeDirectory
 *
 * Grab the tree associated with a #CafeMenuTreeItem.
 *
 * Returns: (transfer full): The #CafeMenuTree
 */
CafeMenuTree *
cafemenu_tree_directory_get_tree (CafeMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  return g_object_ref (directory->item.tree);
}

static void
append_directory_path (CafeMenuTreeDirectory *directory,
		       GString            *path)
{

  if (!directory->item.parent)
    {
      g_string_append_c (path, G_DIR_SEPARATOR);
      return;
    }

  append_directory_path (directory->item.parent, path);

  g_string_append (path, directory->name);
  g_string_append_c (path, G_DIR_SEPARATOR);
}

char *
cafemenu_tree_directory_make_path (CafeMenuTreeDirectory *directory,
				CafeMenuTreeEntry     *entry)
{
  GString *path;

  g_return_val_if_fail (directory != NULL, NULL);

  path = g_string_new (NULL);

  append_directory_path (directory, path);

  if (entry != NULL)
    g_string_append (path,
		     desktop_entry_get_basename (entry->desktop_entry));

  return g_string_free (path, FALSE);
}

/**
 * cafemenu_tree_entry_get_app_info:
 * @entry: a #CafeMenuTreeEntry
 *
 * Returns: (transfer none): The #GDesktopAppInfo for this entry
 */
GDesktopAppInfo *
cafemenu_tree_entry_get_app_info (CafeMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return desktop_entry_get_app_info (entry->desktop_entry);
}

const char *
cafemenu_tree_entry_get_desktop_file_path (CafeMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return desktop_entry_get_path (entry->desktop_entry);
}

const char *
cafemenu_tree_entry_get_desktop_file_id (CafeMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return entry->desktop_file_id;
}

gboolean
cafemenu_tree_entry_get_is_nodisplay_recurse (CafeMenuTreeEntry *entry)
{
  CafeMenuTreeDirectory *directory;
  GDesktopAppInfo *app_info;

  g_return_val_if_fail (entry != NULL, FALSE);

  app_info = cafemenu_tree_entry_get_app_info (entry);

  if (g_desktop_app_info_get_nodisplay (app_info))
    return TRUE;

  directory = entry->item.parent;
  while (directory != NULL)
    {
      if (directory->is_nodisplay)
        return TRUE;

      directory = directory->item.parent;
    }

  return FALSE;
}

gboolean
cafemenu_tree_entry_get_is_excluded (CafeMenuTreeEntry *entry)
{
  g_return_val_if_fail(entry != NULL, FALSE);

  return entry->is_excluded;
}

gboolean
cafemenu_tree_entry_get_is_unallocated (CafeMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, FALSE);

  return entry->is_unallocated;
}

/**
 * cafemenu_tree_entry_get_tree:
 * @entry: A #CafeMenuTreeEntry
 *
 * Grab the tree associated with a #CafeMenuTreeEntry.
 *
 * Returns: (transfer full): The #CafeMenuTree
 */
CafeMenuTree *
cafemenu_tree_entry_get_tree (CafeMenuTreeEntry *entry)
{
	g_return_val_if_fail(entry != NULL, NULL);

  return g_object_ref (entry->item.tree);
}

CafeMenuTreeDirectory *
cafemenu_tree_header_get_directory (CafeMenuTreeHeader *header)
{
  g_return_val_if_fail (header != NULL, NULL);

  return cafemenu_tree_item_ref (header->directory);
}

/**
 * cafemenu_tree_header_get_tree:
 * @header: A #CafeMenuTreeHeader
 *
 * Grab the tree associated with a #CafeMenuTreeHeader.
 *
 * Returns: (transfer full): The #CafeMenuTree
 */
CafeMenuTree *
cafemenu_tree_header_get_tree (CafeMenuTreeHeader *header)
{
  g_return_val_if_fail (header != NULL, NULL);

  return g_object_ref (header->item.tree);
}

CafeMenuTreeItemType
cafemenu_tree_alias_get_aliased_item_type (CafeMenuTreeAlias *alias)
{
  g_return_val_if_fail (alias != NULL, CAFEMENU_TREE_ITEM_INVALID);

  g_assert (alias->aliased_item != NULL);
  return alias->aliased_item->type;
}

CafeMenuTreeDirectory* cafemenu_tree_alias_get_directory(CafeMenuTreeAlias* alias)
{
	g_return_val_if_fail (alias != NULL, NULL);

	return cafemenu_tree_item_ref(alias->directory);
}

/**
 * cafemenu_tree_alias_get_tree:
 * @alias: A #CafeMenuTreeAlias
 *
 * Grab the tree associated with a #CafeMenuTreeAlias.
 *
 * Returns: (transfer full): The #CafeMenuTree
 */
CafeMenuTree *
cafemenu_tree_alias_get_tree (CafeMenuTreeAlias *alias)
{
  g_return_val_if_fail (alias != NULL, NULL);

  return g_object_ref (alias->item.tree);
}

/**
 * cafemenu_tree_separator_get_tree:
 * @separator: A #CafeMenuTreeSeparator
 *
 * Grab the tree associated with a #CafeMenuTreeSeparator.
 *
 * Returns: (transfer full): The #CafeMenuTree
 */
CafeMenuTree *
cafemenu_tree_separator_get_tree (CafeMenuTreeSeparator *separator)
{
  g_return_val_if_fail (separator != NULL, NULL);

  return g_object_ref (separator->item.tree);
}

/**
 * cafemenu_tree_alias_get_aliased_directory:
 * @alias: alias
 *
 * Returns: (transfer full): The aliased directory entry
 */
CafeMenuTreeDirectory *
cafemenu_tree_alias_get_aliased_directory (CafeMenuTreeAlias *alias)
{
  g_return_val_if_fail (alias != NULL, NULL);
  g_return_val_if_fail (alias->aliased_item->type == CAFEMENU_TREE_ITEM_DIRECTORY, NULL);

  return (CafeMenuTreeDirectory *) cafemenu_tree_item_ref (alias->aliased_item);
}

/**
 * cafemenu_tree_alias_get_aliased_entry:
 * @alias: alias
 *
 * Returns: (transfer full): The aliased entry
 */
CafeMenuTreeEntry *
cafemenu_tree_alias_get_aliased_entry (CafeMenuTreeAlias *alias)
{
  g_return_val_if_fail (alias != NULL, NULL);
  g_return_val_if_fail (alias->aliased_item->type == CAFEMENU_TREE_ITEM_ENTRY, NULL);

  return (CafeMenuTreeEntry *) cafemenu_tree_item_ref (alias->aliased_item);
}

static CafeMenuTreeDirectory *
cafemenu_tree_directory_new (CafeMenuTree          *tree,
                          CafeMenuTreeDirectory *parent,
			  const char         *name)
{
  CafeMenuTreeDirectory *retval;

  retval = g_slice_new0 (CafeMenuTreeDirectory);

  retval->item.type     = CAFEMENU_TREE_ITEM_DIRECTORY;
  retval->item.parent   = parent;
  retval->item.refcount = 1;
  retval->item.tree     = tree;

  retval->name                = g_strdup (name);
  retval->directory_entry     = NULL;
  retval->entries             = NULL;
  retval->subdirs             = NULL;
  retval->default_layout_info = NULL;
  retval->layout_info         = NULL;
  retval->contents            = NULL;
  retval->only_unallocated    = FALSE;
  retval->is_nodisplay        = FALSE;
  retval->layout_pending_separator = FALSE;
  retval->preprocessed        = FALSE;
  retval->will_inline_header  = G_MAXUINT16;

  retval->default_layout_values.mask          = MENU_LAYOUT_VALUES_NONE;
  retval->default_layout_values.show_empty    = FALSE;
  retval->default_layout_values.inline_menus  = FALSE;
  retval->default_layout_values.inline_limit  = 4;
  retval->default_layout_values.inline_header = FALSE;
  retval->default_layout_values.inline_alias  = FALSE;

  return retval;
}

static void
cafemenu_tree_directory_finalize (CafeMenuTreeDirectory *directory)
{
  g_assert (directory->item.refcount == 0);

  g_slist_foreach (directory->contents,
		   (GFunc) cafemenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->contents);
  directory->contents = NULL;

  g_slist_foreach (directory->default_layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->default_layout_info);
  directory->default_layout_info = NULL;

  g_slist_foreach (directory->layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->layout_info);
  directory->layout_info = NULL;

  g_slist_foreach (directory->subdirs,
		   (GFunc) cafemenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->subdirs);
  directory->subdirs = NULL;

  g_slist_foreach (directory->entries,
		   (GFunc) cafemenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->entries);
  directory->entries = NULL;

  if (directory->directory_entry)
    desktop_entry_unref (directory->directory_entry);
  directory->directory_entry = NULL;

  g_free (directory->name);
  directory->name = NULL;

  g_slice_free (CafeMenuTreeDirectory, directory);
}

static CafeMenuTreeSeparator *
cafemenu_tree_separator_new (CafeMenuTreeDirectory *parent)
{
  CafeMenuTreeSeparator *retval;

  retval = g_slice_new0 (CafeMenuTreeSeparator);

  retval->item.type     = CAFEMENU_TREE_ITEM_SEPARATOR;
  retval->item.parent   = parent;
  retval->item.refcount = 1;
  retval->item.tree     = parent->item.tree;

  return retval;
}

static void
cafemenu_tree_separator_finalize (CafeMenuTreeSeparator *separator)
{
  g_assert (separator->item.refcount == 0);

  g_slice_free (CafeMenuTreeSeparator, separator);
}

static CafeMenuTreeHeader *
cafemenu_tree_header_new (CafeMenuTreeDirectory *parent,
		       CafeMenuTreeDirectory *directory)
{
  CafeMenuTreeHeader *retval;

  retval = g_slice_new0 (CafeMenuTreeHeader);

  retval->item.type     = CAFEMENU_TREE_ITEM_HEADER;
  retval->item.parent   = parent;
  retval->item.refcount = 1;
  retval->item.tree     = parent->item.tree;

  retval->directory = cafemenu_tree_item_ref (directory);

  cafemenu_tree_item_set_parent (CAFEMENU_TREE_ITEM (retval->directory), NULL);

  return retval;
}

static void
cafemenu_tree_header_finalize (CafeMenuTreeHeader *header)
{
  g_assert (header->item.refcount == 0);

  if (header->directory != NULL)
    cafemenu_tree_item_unref (header->directory);
  header->directory = NULL;

  g_slice_free (CafeMenuTreeHeader, header);
}

static CafeMenuTreeAlias *
cafemenu_tree_alias_new (CafeMenuTreeDirectory *parent,
		      CafeMenuTreeDirectory *directory,
		      CafeMenuTreeItem      *item)
{
  CafeMenuTreeAlias *retval;

  retval = g_slice_new0 (CafeMenuTreeAlias);

  retval->item.type     = CAFEMENU_TREE_ITEM_ALIAS;
  retval->item.parent   = parent;
  retval->item.refcount = 1;
  retval->item.tree     = parent->item.tree;

  retval->directory    = cafemenu_tree_item_ref (directory);
  if (item->type != CAFEMENU_TREE_ITEM_ALIAS)
    retval->aliased_item = cafemenu_tree_item_ref (item);
  else
    {
      CafeMenuTreeAlias *alias = CAFEMENU_TREE_ALIAS (item);
      retval->aliased_item = cafemenu_tree_item_ref (alias->aliased_item);
    }

  cafemenu_tree_item_set_parent (CAFEMENU_TREE_ITEM (retval->directory), NULL);
  cafemenu_tree_item_set_parent (retval->aliased_item, NULL);

  return retval;
}

static void
cafemenu_tree_alias_finalize (CafeMenuTreeAlias *alias)
{
  g_assert (alias->item.refcount == 0);

  if (alias->directory != NULL)
    cafemenu_tree_item_unref (alias->directory);
  alias->directory = NULL;

  if (alias->aliased_item != NULL)
    cafemenu_tree_item_unref (alias->aliased_item);
  alias->aliased_item = NULL;

  g_slice_free (CafeMenuTreeAlias, alias);
}

static CafeMenuTreeEntry *
cafemenu_tree_entry_new (CafeMenuTreeDirectory *parent,
		      DesktopEntry       *desktop_entry,
		      const char         *desktop_file_id,
		      gboolean            is_excluded,
                      gboolean            is_unallocated)
{
  CafeMenuTreeEntry *retval;

  retval = g_slice_new0 (CafeMenuTreeEntry);

  retval->item.type     = CAFEMENU_TREE_ITEM_ENTRY;
  retval->item.parent   = parent;
  retval->item.refcount = 1;
  retval->item.tree     = parent->item.tree;

  retval->desktop_entry   = desktop_entry_ref (desktop_entry);
  retval->desktop_file_id = g_strdup (desktop_file_id);
  retval->is_excluded     = is_excluded != FALSE;
  retval->is_unallocated  = is_unallocated != FALSE;

  return retval;
}

static void
cafemenu_tree_entry_finalize (CafeMenuTreeEntry *entry)
{
  g_assert (entry->item.refcount == 0);

  g_free (entry->desktop_file_id);
  entry->desktop_file_id = NULL;

  if (entry->desktop_entry)
    desktop_entry_unref (entry->desktop_entry);
  entry->desktop_entry = NULL;

  g_slice_free (CafeMenuTreeEntry, entry);
}

static int
cafemenu_tree_entry_compare_by_id (CafeMenuTreeItem *a,
				CafeMenuTreeItem *b)
{
  if (a->type == CAFEMENU_TREE_ITEM_ALIAS)
    a = CAFEMENU_TREE_ALIAS (a)->aliased_item;

  if (b->type == CAFEMENU_TREE_ITEM_ALIAS)
    b = CAFEMENU_TREE_ALIAS (b)->aliased_item;

  return strcmp (CAFEMENU_TREE_ENTRY (a)->desktop_file_id,
                 CAFEMENU_TREE_ENTRY (b)->desktop_file_id);
}

/**
 * cafemenu_tree_item_ref:
 * @item: a #CafeMenuTreeItem
 *
 * Returns: (transfer full): The same @item, or %NULL if @item is not a valid #CafeMenuTreeItem
 */
gpointer
cafemenu_tree_item_ref (gpointer itemp)
{
	CafeMenuTreeItem* item = (CafeMenuTreeItem*) itemp;

	g_return_val_if_fail(item != NULL, NULL);
	g_return_val_if_fail(item->refcount > 0, NULL);

  g_atomic_int_inc (&item->refcount);

	return item;
}

void
cafemenu_tree_item_unref (gpointer itemp)
{
  CafeMenuTreeItem *item;

  item = (CafeMenuTreeItem *) itemp;

  g_return_if_fail (item != NULL);
  g_return_if_fail (item->refcount > 0);

  if (g_atomic_int_dec_and_test (&(item->refcount)))
    {
      switch (item->type)
	{
	case CAFEMENU_TREE_ITEM_DIRECTORY:
	  cafemenu_tree_directory_finalize (CAFEMENU_TREE_DIRECTORY (item));
	  break;

	case CAFEMENU_TREE_ITEM_ENTRY:
	  cafemenu_tree_entry_finalize (CAFEMENU_TREE_ENTRY (item));
	  break;

	case CAFEMENU_TREE_ITEM_SEPARATOR:
	  cafemenu_tree_separator_finalize (CAFEMENU_TREE_SEPARATOR (item));
	  break;

	case CAFEMENU_TREE_ITEM_HEADER:
	  cafemenu_tree_header_finalize (CAFEMENU_TREE_HEADER (item));
	  break;

	case CAFEMENU_TREE_ITEM_ALIAS:
	  cafemenu_tree_alias_finalize (CAFEMENU_TREE_ALIAS (item));
	  break;

	default:
	  g_assert_not_reached ();
	  break;
	}
    }
}

static void
cafemenu_tree_item_unref_and_unset_parent (gpointer itemp)
{
  CafeMenuTreeItem *item;

  item = (CafeMenuTreeItem *) itemp;

  g_return_if_fail (item != NULL);

  cafemenu_tree_item_set_parent (item, NULL);
  cafemenu_tree_item_unref (item);
}

static inline const char *
cafemenu_tree_item_compare_get_name_helper (CafeMenuTreeItem    *item,
					 CafeMenuTreeFlags    flags)
{
  const char *name;

  name = NULL;

  switch (item->type)
    {
    case CAFEMENU_TREE_ITEM_DIRECTORY:
      if (CAFEMENU_TREE_DIRECTORY (item)->directory_entry)
	name = desktop_entry_get_name (CAFEMENU_TREE_DIRECTORY (item)->directory_entry);
      else
	name = CAFEMENU_TREE_DIRECTORY (item)->name;
      break;

    case CAFEMENU_TREE_ITEM_ENTRY:
      if (flags & CAFEMENU_TREE_FLAGS_SORT_DISPLAY_NAME)
        name = g_app_info_get_display_name (G_APP_INFO (cafemenu_tree_entry_get_app_info (CAFEMENU_TREE_ENTRY (item))));
      else
        name = desktop_entry_get_name (CAFEMENU_TREE_ENTRY (item)->desktop_entry);
      break;

    case CAFEMENU_TREE_ITEM_ALIAS:
      {
        CafeMenuTreeItem *dir;
        dir = CAFEMENU_TREE_ITEM (CAFEMENU_TREE_ALIAS (item)->directory);
        name = cafemenu_tree_item_compare_get_name_helper (dir, flags);
      }
      break;

    case CAFEMENU_TREE_ITEM_SEPARATOR:
    case CAFEMENU_TREE_ITEM_HEADER:
    default:
      g_assert_not_reached ();
      break;
    }

  return name;
}

static int
cafemenu_tree_item_compare (CafeMenuTreeItem *a,
			 CafeMenuTreeItem *b,
			 gpointer       flags_p)
{
  const char       *name_a;
  const char       *name_b;
  CafeMenuTreeFlags    flags;

  flags = GPOINTER_TO_INT (flags_p);

  name_a = cafemenu_tree_item_compare_get_name_helper (a, flags);
  name_b = cafemenu_tree_item_compare_get_name_helper (b, flags);

  return g_utf8_collate (name_a, name_b);
}

static MenuLayoutNode *
find_menu_child (MenuLayoutNode *layout)
{
  MenuLayoutNode *child;

  child = menu_layout_node_get_children (layout);
  while (child && menu_layout_node_get_type (child) != MENU_LAYOUT_NODE_MENU)
    child = menu_layout_node_get_next (child);

  return child;
}

static void
merge_resolved_children (CafeMenuTree      *tree,
			 GHashTable     *loaded_menu_files,
                         MenuLayoutNode *where,
                         MenuLayoutNode *from)
{
  MenuLayoutNode *insert_after;
  MenuLayoutNode *menu_child;
  MenuLayoutNode *from_child;

  cafemenu_tree_resolve_files (tree, loaded_menu_files, from);

  insert_after = where;
  g_assert (menu_layout_node_get_type (insert_after) != MENU_LAYOUT_NODE_ROOT);
  g_assert (menu_layout_node_get_parent (insert_after) != NULL);

  /* skip root node */
  menu_child = find_menu_child (from);
  g_assert (menu_child != NULL);
  g_assert (menu_layout_node_get_type (menu_child) == MENU_LAYOUT_NODE_MENU);

  /* merge children of toplevel <Menu> */
  from_child = menu_layout_node_get_children (menu_child);
  while (from_child != NULL)
    {
      MenuLayoutNode *next;

      next = menu_layout_node_get_next (from_child);

      menu_verbose ("Merging ");
      menu_debug_print_layout (from_child, FALSE);
      menu_verbose (" after ");
      menu_debug_print_layout (insert_after, FALSE);

      switch (menu_layout_node_get_type (from_child))
        {
        case MENU_LAYOUT_NODE_NAME:
          menu_layout_node_unlink (from_child); /* delete this */
          break;

        default:
          menu_layout_node_steal (from_child);
          menu_layout_node_insert_after (insert_after, from_child);
          menu_layout_node_unref (from_child);

          insert_after = from_child;
          break;
        }

      from_child = next;
    }
}

static gboolean
load_merge_file (CafeMenuTree      *tree,
		 GHashTable     *loaded_menu_files,
                 const char     *filename,
                 gboolean        is_canonical,
		 gboolean        add_monitor,
                 MenuLayoutNode *where)
{
  MenuLayoutNode *to_merge;
  const char     *canonical;
  char           *freeme;
  gboolean        retval;

  freeme = NULL;
  retval = FALSE;

  if (!is_canonical)
    {
      canonical = freeme = realpath (filename, NULL);
      if (canonical == NULL)
        {
	  if (add_monitor)
	    cafemenu_tree_add_menu_file_monitor (tree,
					     filename,
					     MENU_FILE_MONITOR_NONEXISTENT_FILE);

          menu_verbose ("Failed to canonicalize merge file path \"%s\": %s\n",
                        filename, g_strerror (errno));
	  goto out;
        }
    }
  else
    {
      canonical = filename;
    }

  if (g_hash_table_lookup (loaded_menu_files, canonical) != NULL)
    {
      g_warning ("Not loading \"%s\": recursive loop detected in .menu files",
		 canonical);
      retval = TRUE;
      goto out;
    }

  menu_verbose ("Merging file \"%s\"\n", canonical);

  to_merge = menu_layout_load (canonical, tree->non_prefixed_basename, NULL);
  if (to_merge == NULL)
    {
      menu_verbose ("No menu for file \"%s\" found when merging\n",
                    canonical);
      goto out;
    }

  retval = TRUE;

  g_hash_table_insert (loaded_menu_files, (char *) canonical, GUINT_TO_POINTER (TRUE));

  if (add_monitor)
    cafemenu_tree_add_menu_file_monitor (tree,
				      canonical,
				      MENU_FILE_MONITOR_FILE);

  merge_resolved_children (tree, loaded_menu_files, where, to_merge);

  g_hash_table_remove (loaded_menu_files, canonical);

  menu_layout_node_unref (to_merge);

 out:
  if (freeme)
    g_free (freeme);

  return retval;
}

static gboolean
load_merge_file_with_config_dir (CafeMenuTree      *tree,
				 GHashTable     *loaded_menu_files,
				 const char     *menu_file,
				 const char     *config_dir,
				 MenuLayoutNode *where)
{
  char     *merge_file;
  gboolean  loaded;

  loaded = FALSE;

  merge_file = g_build_filename (config_dir, "menus", menu_file, NULL);

  if (load_merge_file (tree, loaded_menu_files, merge_file, FALSE, TRUE, where))
    loaded = TRUE;

  g_free (merge_file);

  return loaded;
}

static gboolean
compare_basedir_to_config_dir (const char *canonical_basedir,
			       const char *config_dir)
{
  char     *dirname;
  char     *canonical_menus_dir;
  gboolean  retval;

  menu_verbose ("Checking to see if basedir '%s' is in '%s'\n",
		canonical_basedir, config_dir);

  dirname = g_build_filename (config_dir, "menus", NULL);

  retval = FALSE;

  canonical_menus_dir = realpath (dirname, NULL);
  if (canonical_menus_dir != NULL &&
      strcmp (canonical_basedir, canonical_menus_dir) == 0)
    {
      retval = TRUE;
    }

  g_free (canonical_menus_dir);
  g_free (dirname);

  return retval;
}

static gboolean
load_parent_merge_file_from_basename (CafeMenuTree      *tree,
                                      GHashTable     *loaded_menu_files,
			              MenuLayoutNode *layout,
                                      const char     *menu_file,
                                      const char     *canonical_basedir)
{
  gboolean            found_basedir;
  const char * const *system_config_dirs;
  int                 i;

  /* We're not interested in menu files that are in directories which are not a
   * parent of the base directory of this menu file */
  found_basedir = compare_basedir_to_config_dir (canonical_basedir,
						 g_get_user_config_dir ());

  system_config_dirs = g_get_system_config_dirs ();

  i = 0;
  while (system_config_dirs[i] != NULL)
    {
      if (!found_basedir)
	{
	  found_basedir = compare_basedir_to_config_dir (canonical_basedir,
							 system_config_dirs[i]);
	}
      else
	{
	  menu_verbose ("Looking for parent menu file '%s' in '%s'\n",
			menu_file, system_config_dirs[i]);

	  if (load_merge_file_with_config_dir (tree,
					       loaded_menu_files,
					       menu_file,
					       system_config_dirs[i],
					       layout))
	    {
	      break;
	    }
	}

      ++i;
    }

  return system_config_dirs[i] != NULL;
}

static gboolean load_parent_merge_file(CafeMenuTree* tree, GHashTable* loaded_menu_files, MenuLayoutNode* layout)
{
	MenuLayoutNode* root;
	const char* basedir;
	const char* menu_name;
	char* canonical_basedir;
	char* menu_file;
	gboolean found;

	root = menu_layout_node_get_root(layout);

	basedir   = menu_layout_node_root_get_basedir(root);
	menu_name = menu_layout_node_root_get_name(root);

	canonical_basedir = realpath (basedir, NULL);

	if (canonical_basedir == NULL)
	{
		menu_verbose("Menu basedir '%s' no longer exists, not merging parent\n", basedir);
		return FALSE;
	}

	found = FALSE;
	menu_file = g_strconcat(menu_name, ".menu", NULL);

	if (strcmp(menu_file, "cafe-applications.menu") == 0 && g_getenv("XDG_MENU_PREFIX"))
	{
		char* prefixed_basename;
		prefixed_basename = g_strdup_printf("%s%s", g_getenv("XDG_MENU_PREFIX"), menu_file);
		found = load_parent_merge_file_from_basename(tree, loaded_menu_files, layout, prefixed_basename, canonical_basedir);
		g_free(prefixed_basename);
	}

	if (!found)
	{
		found = load_parent_merge_file_from_basename(tree, loaded_menu_files, layout, menu_file, canonical_basedir);
	}

	g_free(menu_file);
	g_free(canonical_basedir);

	return found;
}

static void
load_merge_dir (CafeMenuTree      *tree,
		GHashTable     *loaded_menu_files,
                const char     *dirname,
                MenuLayoutNode *where)
{
  GDir       *dir;
  const char *menu_file;

  menu_verbose ("Loading merge dir \"%s\"\n", dirname);

  cafemenu_tree_add_menu_file_monitor (tree,
				    dirname,
				    MENU_FILE_MONITOR_DIRECTORY);

  if ((dir = g_dir_open (dirname, 0, NULL)) == NULL)
    return;

  while ((menu_file = g_dir_read_name (dir)))
    {
      if (g_str_has_suffix (menu_file, ".menu"))
        {
          char *full_path;

          full_path = g_build_filename (dirname, menu_file, NULL);

          load_merge_file (tree, loaded_menu_files, full_path, TRUE, FALSE, where);

          g_free (full_path);
        }
    }

  g_dir_close (dir);
}

static void
load_merge_dir_with_config_dir (CafeMenuTree      *tree,
				GHashTable     *loaded_menu_files,
                                const char     *config_dir,
                                const char     *dirname,
                                MenuLayoutNode *where)
{
  char *path;

  path = g_build_filename (config_dir, "menus", dirname, NULL);

  load_merge_dir (tree, loaded_menu_files, path, where);

  g_free (path);
}

static void
resolve_merge_file (CafeMenuTree      *tree,
		    GHashTable     *loaded_menu_files,
                    MenuLayoutNode *layout)
{
  char *filename;

  if (menu_layout_node_merge_file_get_type (layout) == MENU_MERGE_FILE_TYPE_PARENT)
    {
      if (load_parent_merge_file (tree, loaded_menu_files, layout))
        return;
    }

  filename = menu_layout_node_get_content_as_path (layout);
  if (filename == NULL)
    {
      menu_verbose ("didn't get node content as a path, not merging file\n");
    }
  else
    {
      load_merge_file (tree, loaded_menu_files, filename, FALSE, TRUE, layout);

      g_free (filename);
    }

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
resolve_merge_dir (CafeMenuTree      *tree,
		   GHashTable     *loaded_menu_files,
                   MenuLayoutNode *layout)
{
  char *path;

  path = menu_layout_node_get_content_as_path (layout);
  if (path == NULL)
    {
      menu_verbose ("didn't get layout node content as a path, not merging dir\n");
    }
  else
    {
      load_merge_dir (tree, loaded_menu_files, path, layout);

      g_free (path);
    }

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static MenuLayoutNode *
add_app_dir (CafeMenuTree   *tree G_GNUC_UNUSED,
	     MenuLayoutNode *before,
	     const char     *data_dir)
{
  MenuLayoutNode *tmp;
  char           *dirname;

  tmp = menu_layout_node_new (MENU_LAYOUT_NODE_APP_DIR);
  dirname = g_build_filename (data_dir, "applications", NULL);
  menu_layout_node_set_content (tmp, dirname);
  menu_layout_node_insert_before (before, tmp);
  menu_layout_node_unref (before);

  menu_verbose ("Adding <AppDir>%s</AppDir> in <DefaultAppDirs/>\n",
                dirname);

  g_free (dirname);

  return tmp;
}

static void
resolve_default_app_dirs (CafeMenuTree      *tree,
                          MenuLayoutNode *layout)
{
  MenuLayoutNode     *before;
  const char * const *system_data_dirs;
  int                 i;

  system_data_dirs = g_get_system_data_dirs ();

  before = add_app_dir (tree,
			menu_layout_node_ref (layout),
			g_get_user_data_dir ());

  i = 0;
  while (system_data_dirs[i] != NULL)
    {
      before = add_app_dir (tree, before, system_data_dirs[i]);

      ++i;
    }

  menu_layout_node_unref (before);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static MenuLayoutNode* add_directory_dir (CafeMenuTree   *tree G_GNUC_UNUSED,
					  MenuLayoutNode *before,
					  const char     *data_dir)
{
	MenuLayoutNode* tmp;
	char* dirname;

	tmp = menu_layout_node_new(MENU_LAYOUT_NODE_DIRECTORY_DIR);
	dirname = g_build_filename(data_dir, "desktop-directories", NULL);
	menu_layout_node_set_content(tmp, dirname);
	menu_layout_node_insert_before(before, tmp);
	menu_layout_node_unref(before);

	menu_verbose("Adding <DirectoryDir>%s</DirectoryDir> in <DefaultDirectoryDirs/>\n", dirname);

	g_free(dirname);

	return tmp;
}

/* According to desktop spec, since our menu file is called 'cafe-applications', our
 * merged menu folders need to be called 'cafe-applications-merged'.  We'll setup the folder
 * 'applications-merged' if it doesn't exist yet, and a symlink pointing to it in the
 * ~/.config/menus directory
 */
static void
setup_merge_dir_symlink(void)
{
    gchar *user_config = (gchar *) g_get_user_config_dir();
    gchar *merge_path = g_build_filename (user_config, "menus", "applications-merged", NULL);
    GFile *merge_file = g_file_new_for_path (merge_path);
    gchar *sym_path;
    GFile *sym_file;

    g_file_make_directory_with_parents (merge_file, NULL, NULL);

    sym_path = g_build_filename (user_config, "menus", "cafe-applications-merged", NULL);
    sym_file = g_file_new_for_path (sym_path);
    if (!g_file_query_exists (sym_file, NULL)) {
        g_file_make_symbolic_link (sym_file, merge_path, NULL, NULL);
    }

    g_free (merge_path);
    g_free (sym_path);
    g_object_unref (merge_file);
    g_object_unref (sym_file);
}

static void
resolve_default_directory_dirs (CafeMenuTree      *tree,
                                MenuLayoutNode *layout)
{
  MenuLayoutNode     *before;
  const char * const *system_data_dirs;
  int                 i;

  system_data_dirs = g_get_system_data_dirs ();

  before = add_directory_dir (tree,
			      menu_layout_node_ref (layout),
			      g_get_user_data_dir ());

  i = 0;
  while (system_data_dirs[i] != NULL)
    {
		/* Parche para tomar las carpetas /cafe/ */
		char* path = g_build_filename(system_data_dirs[i], "cafe", NULL);
		before = add_directory_dir(tree, before, path);
		g_free(path);
		/* /fin parche */
      before = add_directory_dir (tree, before, system_data_dirs[i]);

      ++i;
    }

  menu_layout_node_unref (before);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
resolve_default_merge_dirs (CafeMenuTree      *tree,
			    GHashTable     *loaded_menu_files,
                            MenuLayoutNode *layout)
{
  MenuLayoutNode     *root;
  const char         *menu_name;
  char               *merge_name;
  const char * const *system_config_dirs;
  int                 i;

  setup_merge_dir_symlink();

  root = menu_layout_node_get_root (layout);
  menu_name = menu_layout_node_root_get_name (root);

  merge_name = g_strconcat (menu_name, "-merged", NULL);

  system_config_dirs = g_get_system_config_dirs ();

  /* Merge in reverse order */
  i = 0;
  while (system_config_dirs[i] != NULL) i++;
  while (i > 0)
    {
      i--;
      load_merge_dir_with_config_dir (tree,
				      loaded_menu_files,
                                      system_config_dirs[i],
                                      merge_name,
                                      layout);
    }

  load_merge_dir_with_config_dir (tree,
				  loaded_menu_files,
                                  g_get_user_config_dir (),
                                  merge_name,
                                  layout);

  g_free (merge_name);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
add_filename_include (const char     *desktop_file_id,
                      DesktopEntry   *entry,
                      MenuLayoutNode *include)
{
  if (!desktop_entry_has_categories (entry))
    {
      MenuLayoutNode *node;

      node = menu_layout_node_new (MENU_LAYOUT_NODE_FILENAME);
      menu_layout_node_set_content (node, desktop_file_id);

      menu_layout_node_append_child (include, node);
      menu_layout_node_unref (node);
    }
}

static void
is_dot_directory (const char   *basename,
		  DesktopEntry *entry G_GNUC_UNUSED,
		  gboolean     *has_dot_directory)
{
  if (!strcmp (basename, ".directory"))
    *has_dot_directory = TRUE;
}

static gboolean
add_menu_for_legacy_dir (MenuLayoutNode *parent,
                         const char     *legacy_dir,
                	 const char     *relative_path,
                         const char     *legacy_prefix,
                         const char     *menu_name)
{
  EntryDirectory  *ed;
  DesktopEntrySet *desktop_entries;
  DesktopEntrySet *directory_entries;
  GSList          *subdirs;
  gboolean         menu_added;
  gboolean         has_dot_directory;

  ed = entry_directory_new_legacy (DESKTOP_ENTRY_INVALID, legacy_dir, legacy_prefix);
  if (!ed)
    return FALSE;

  subdirs = NULL;
  desktop_entries   = desktop_entry_set_new ();
  directory_entries = desktop_entry_set_new ();

  entry_directory_get_flat_contents (ed,
                                     desktop_entries,
                                     directory_entries,
                                     &subdirs);
  entry_directory_unref (ed);

  has_dot_directory = FALSE;
  desktop_entry_set_foreach (directory_entries,
			     (DesktopEntrySetForeachFunc) is_dot_directory,
			     &has_dot_directory);
  desktop_entry_set_unref (directory_entries);

  menu_added = FALSE;
  if (desktop_entry_set_get_count (desktop_entries) > 0 || subdirs)
    {
      MenuLayoutNode *menu;
      MenuLayoutNode *node;
      GString        *subdir_path;
      GString        *subdir_relative;
      GSList         *tmp;
      int             legacy_dir_len;
      int             relative_path_len;

      menu = menu_layout_node_new (MENU_LAYOUT_NODE_MENU);
      menu_layout_node_append_child (parent, menu);

      menu_added = TRUE;

      g_assert (menu_name != NULL);

      node = menu_layout_node_new (MENU_LAYOUT_NODE_NAME);
      menu_layout_node_set_content (node, menu_name);
      menu_layout_node_append_child (menu, node);
      menu_layout_node_unref (node);

      if (has_dot_directory)
	{
	  node = menu_layout_node_new (MENU_LAYOUT_NODE_DIRECTORY);
	  if (relative_path != NULL)
	    {
	      char *directory_entry_path;

	      directory_entry_path = g_strdup_printf ("%s/.directory", relative_path);
	      menu_layout_node_set_content (node, directory_entry_path);
	      g_free (directory_entry_path);
	    }
	  else
	    {
	      menu_layout_node_set_content (node, ".directory");
	    }
	  menu_layout_node_append_child (menu, node);
	  menu_layout_node_unref (node);
	}

      if (desktop_entry_set_get_count (desktop_entries) > 0)
	{
	  MenuLayoutNode *include;

	  include = menu_layout_node_new (MENU_LAYOUT_NODE_INCLUDE);
	  menu_layout_node_append_child (menu, include);

	  desktop_entry_set_foreach (desktop_entries,
				     (DesktopEntrySetForeachFunc) add_filename_include,
				     include);

	  menu_layout_node_unref (include);
	}

      subdir_path = g_string_new (legacy_dir);
      legacy_dir_len = strlen (legacy_dir);

      subdir_relative = g_string_new (relative_path);
      relative_path_len = relative_path ? strlen (relative_path) : 0;

      tmp = subdirs;
      while (tmp != NULL)
        {
          const char *subdir = tmp->data;

          g_string_append_c (subdir_path, G_DIR_SEPARATOR);
          g_string_append (subdir_path, subdir);

	  if (relative_path_len)
	    {
	      g_string_append_c (subdir_relative, G_DIR_SEPARATOR);
	    }
          g_string_append (subdir_relative, subdir);

          add_menu_for_legacy_dir (menu,
                                   subdir_path->str,
				   subdir_relative->str,
                                   legacy_prefix,
                                   subdir);

          g_string_truncate (subdir_relative, relative_path_len);
          g_string_truncate (subdir_path, legacy_dir_len);

          tmp = tmp->next;
        }

      g_string_free (subdir_path, TRUE);
      g_string_free (subdir_relative, TRUE);

      menu_layout_node_unref (menu);
    }

  desktop_entry_set_unref (desktop_entries);

  g_slist_foreach (subdirs, (GFunc) g_free, NULL);
  g_slist_free (subdirs);

  return menu_added;
}

static void
resolve_legacy_dir (CafeMenuTree      *tree,
		    GHashTable     *loaded_menu_files,
                    MenuLayoutNode *legacy)
{
  MenuLayoutNode *to_merge;
  MenuLayoutNode *menu;

  to_merge = menu_layout_node_new (MENU_LAYOUT_NODE_ROOT);

  menu = menu_layout_node_get_parent (legacy);
  g_assert (menu_layout_node_get_type (menu) == MENU_LAYOUT_NODE_MENU);

  if (add_menu_for_legacy_dir (to_merge,
                               menu_layout_node_get_content (legacy),
			       NULL,
                               menu_layout_node_legacy_dir_get_prefix (legacy),
                               menu_layout_node_menu_get_name (menu)))
    {
      merge_resolved_children (tree, loaded_menu_files, legacy, to_merge);
    }

  menu_layout_node_unref (to_merge);
}

static MenuLayoutNode *
add_legacy_dir (CafeMenuTree      *tree,
		GHashTable     *loaded_menu_files,
                MenuLayoutNode *before,
                const char     *data_dir)
{
  MenuLayoutNode *legacy;
  char           *dirname;

  dirname = g_build_filename (data_dir, "applnk", NULL);

  legacy = menu_layout_node_new (MENU_LAYOUT_NODE_LEGACY_DIR);
  menu_layout_node_set_content (legacy, dirname);
  menu_layout_node_legacy_dir_set_prefix (legacy, "kde");
  menu_layout_node_insert_before (before, legacy);
  menu_layout_node_unref (before);

  menu_verbose ("Adding <LegacyDir>%s</LegacyDir> in <KDELegacyDirs/>\n",
                dirname);

  resolve_legacy_dir (tree, loaded_menu_files, legacy);

  g_free (dirname);

  return legacy;
}

static void
resolve_kde_legacy_dirs (CafeMenuTree      *tree,
			 GHashTable     *loaded_menu_files,
                         MenuLayoutNode *layout)
{
  MenuLayoutNode     *before;
  const char * const *system_data_dirs;
  int                 i;

  system_data_dirs = g_get_system_data_dirs ();

  before = add_legacy_dir (tree,
			   loaded_menu_files,
			   menu_layout_node_ref (layout),
			   g_get_user_data_dir ());

  i = 0;
  while (system_data_dirs[i] != NULL)
    {
      before = add_legacy_dir (tree, loaded_menu_files, before, system_data_dirs[i]);

      ++i;
    }

  menu_layout_node_unref (before);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
cafemenu_tree_resolve_files (CafeMenuTree      *tree,
			  GHashTable     *loaded_menu_files,
			  MenuLayoutNode *layout)
{
  MenuLayoutNode *child;

  menu_verbose ("Resolving files in: ");
  menu_debug_print_layout (layout, TRUE);

  switch (menu_layout_node_get_type (layout))
    {
    case MENU_LAYOUT_NODE_MERGE_FILE:
      resolve_merge_file (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_MERGE_DIR:
      resolve_merge_dir (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_DEFAULT_APP_DIRS:
      resolve_default_app_dirs (tree, layout);
      break;

    case MENU_LAYOUT_NODE_DEFAULT_DIRECTORY_DIRS:
      resolve_default_directory_dirs (tree, layout);
      break;

    case MENU_LAYOUT_NODE_DEFAULT_MERGE_DIRS:
      resolve_default_merge_dirs (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_LEGACY_DIR:
      resolve_legacy_dir (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_KDE_LEGACY_DIRS:
      resolve_kde_legacy_dirs (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_PASSTHROUGH:
      /* Just get rid of these, we don't need the memory usage */
      menu_layout_node_unlink (layout);
      break;

    default:
      /* Recurse */
      child = menu_layout_node_get_children (layout);
      while (child != NULL)
        {
          MenuLayoutNode *next = menu_layout_node_get_next (child);

          cafemenu_tree_resolve_files (tree, loaded_menu_files, child);

          child = next;
        }
      break;
    }
}

static void
move_children (MenuLayoutNode *from,
               MenuLayoutNode *to)
{
  MenuLayoutNode *from_child;
  MenuLayoutNode *insert_before;

  insert_before = menu_layout_node_get_children (to);
  from_child    = menu_layout_node_get_children (from);

  while (from_child != NULL)
    {
      MenuLayoutNode *next;

      next = menu_layout_node_get_next (from_child);

      menu_layout_node_steal (from_child);

      if (menu_layout_node_get_type (from_child) == MENU_LAYOUT_NODE_NAME)
        {
          ; /* just drop the Name in the old <Menu> */
        }
      else if (insert_before)
        {
          menu_layout_node_insert_before (insert_before, from_child);
          g_assert (menu_layout_node_get_next (from_child) == insert_before);
        }
      else
        {
          menu_layout_node_append_child (to, from_child);
        }

      menu_layout_node_unref (from_child);

      from_child = next;
    }
}

static int
null_safe_strcmp (const char *a,
                  const char *b)
{
  if (a == NULL && b == NULL)
    return 0;
  else if (a == NULL)
    return -1;
  else if (b == NULL)
    return 1;
  else
    return strcmp (a, b);
}

static int
node_compare_func (const void *a,
                   const void *b)
{
  MenuLayoutNode *node_a = (MenuLayoutNode*) a;
  MenuLayoutNode *node_b = (MenuLayoutNode*) b;
  MenuLayoutNodeType t_a = menu_layout_node_get_type (node_a);
  MenuLayoutNodeType t_b = menu_layout_node_get_type (node_b);

  if (t_a < t_b)
    return -1;
  else if (t_a > t_b)
    return 1;
  else
    {
      const char *c_a = menu_layout_node_get_content (node_a);
      const char *c_b = menu_layout_node_get_content (node_b);

      return null_safe_strcmp (c_a, c_b);
    }
}

static int
node_menu_compare_func (const void *a,
                        const void *b)
{
  MenuLayoutNode *node_a = (MenuLayoutNode*) a;
  MenuLayoutNode *node_b = (MenuLayoutNode*) b;
  MenuLayoutNode *parent_a = menu_layout_node_get_parent (node_a);
  MenuLayoutNode *parent_b = menu_layout_node_get_parent (node_b);

  if (parent_a < parent_b)
    return -1;
  else if (parent_a > parent_b)
    return 1;
  else
    return null_safe_strcmp (menu_layout_node_menu_get_name (node_a),
                             menu_layout_node_menu_get_name (node_b));
}

static void
cafemenu_tree_strip_duplicate_children (CafeMenuTree      *tree,
				     MenuLayoutNode *layout)
{
  MenuLayoutNode *child;
  GSList         *simple_nodes;
  GSList         *menu_layout_nodes;
  GSList         *prev;
  GSList         *tmp;

  /* to strip dups, we find all the child nodes where
   * we want to kill dups, sort them,
   * then nuke the adjacent nodes that are equal
   */

  simple_nodes = NULL;
  menu_layout_nodes = NULL;

  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      switch (menu_layout_node_get_type (child))
        {
          /* These are dups if their content is the same */
        case MENU_LAYOUT_NODE_APP_DIR:
        case MENU_LAYOUT_NODE_DIRECTORY_DIR:
        case MENU_LAYOUT_NODE_DIRECTORY:
          simple_nodes = g_slist_prepend (simple_nodes, child);
          break;

          /* These have to be merged in a more complicated way,
           * and then recursed
           */
        case MENU_LAYOUT_NODE_MENU:
          menu_layout_nodes = g_slist_prepend (menu_layout_nodes, child);
          break;

        default:
          break;
        }

      child = menu_layout_node_get_next (child);
    }

  /* Note that the lists are all backward. So we want to keep
   * the items that are earlier in the list, because they were
   * later in the file
   */

  /* stable sort the simple nodes */
  simple_nodes = g_slist_sort (simple_nodes,
                               node_compare_func);

  prev = NULL;
  tmp = simple_nodes;
  while (tmp != NULL)
    {
      GSList *next = tmp->next;

      if (prev)
        {
          MenuLayoutNode *p = prev->data;
          MenuLayoutNode *n = tmp->data;

          if (node_compare_func (p, n) == 0)
            {
              /* nuke it! */
              menu_layout_node_unlink (n);
	      simple_nodes = g_slist_delete_link (simple_nodes, tmp);
	      tmp = prev;
            }
        }

      prev = tmp;
      tmp = next;
    }

  g_slist_free (simple_nodes);
  simple_nodes = NULL;

  /* stable sort the menu nodes (the sort includes the
   * parents of the nodes in the comparison). Remember
   * the list is backward.
   */
  menu_layout_nodes = g_slist_sort (menu_layout_nodes,
				    node_menu_compare_func);

  prev = NULL;
  tmp = menu_layout_nodes;
  while (tmp != NULL)
    {
      GSList *next = tmp->next;

      if (prev)
        {
          MenuLayoutNode *p = prev->data;
          MenuLayoutNode *n = tmp->data;

          if (node_menu_compare_func (p, n) == 0)
            {
              /* Move children of first menu to the start of second
               * menu and nuke the first menu
               */
              move_children (n, p);
              menu_layout_node_unlink (n);
	      menu_layout_nodes = g_slist_delete_link (menu_layout_nodes, tmp);
	      tmp = prev;
            }
        }

      prev = tmp;
      tmp = next;
    }

  g_slist_free (menu_layout_nodes);
  menu_layout_nodes = NULL;

  /* Recursively clean up all children */
  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      if (menu_layout_node_get_type (child) == MENU_LAYOUT_NODE_MENU)
        cafemenu_tree_strip_duplicate_children (tree, child);

      child = menu_layout_node_get_next (child);
    }
}

static MenuLayoutNode *
find_submenu (MenuLayoutNode *layout,
              const char     *path,
              gboolean        create_if_not_found)
{
  MenuLayoutNode *child;
  const char     *slash;
  const char     *next_path;
  char           *name;

  menu_verbose (" (splitting \"%s\")\n", path);

  if (path[0] == '\0' || path[0] == G_DIR_SEPARATOR)
    return NULL;

  slash = strchr (path, G_DIR_SEPARATOR);
  if (slash != NULL)
    {
      name = g_strndup (path, slash - path);
      next_path = slash + 1;
      if (*next_path == '\0')
        next_path = NULL;
    }
  else
    {
      name = g_strdup (path);
      next_path = NULL;
    }

  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      switch (menu_layout_node_get_type (child))
        {
        case MENU_LAYOUT_NODE_MENU:
          {
            if (strcmp (name, menu_layout_node_menu_get_name (child)) == 0)
              {
                menu_verbose ("MenuNode %p found for path component \"%s\"\n",
                              child, name);

                g_free (name);

                if (!next_path)
                  {
                    menu_verbose (" Found menu node %p parent is %p\n",
                                  child, layout);
                    return child;
                  }

                return find_submenu (child, next_path, create_if_not_found);
              }
          }
          break;

        default:
          break;
        }

      child = menu_layout_node_get_next (child);
    }

  if (create_if_not_found)
    {
      MenuLayoutNode *name_node;

      child = menu_layout_node_new (MENU_LAYOUT_NODE_MENU);
      menu_layout_node_append_child (layout, child);

      name_node = menu_layout_node_new (MENU_LAYOUT_NODE_NAME);
      menu_layout_node_set_content (name_node, name);
      menu_layout_node_append_child (child, name_node);
      menu_layout_node_unref (name_node);

      menu_verbose (" Created menu node %p parent is %p\n",
                    child, layout);

      menu_layout_node_unref (child);
      g_free (name);

      if (!next_path)
        return child;

      return find_submenu (child, next_path, create_if_not_found);
    }
  else
    {
      g_free (name);
      return NULL;
    }
}

/* To call this you first have to strip duplicate children once,
 * otherwise when you move a menu Foo to Bar then you may only
 * move one of Foo, not all the merged Foo.
 */
static void
cafemenu_tree_execute_moves (CafeMenuTree      *tree,
			  MenuLayoutNode *layout,
			  gboolean       *need_remove_dups_p)
{
  MenuLayoutNode *child;
  gboolean        need_remove_dups;
  GSList         *move_nodes;
  GSList         *tmp;

  need_remove_dups = FALSE;

  move_nodes = NULL;

  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      switch (menu_layout_node_get_type (child))
        {
        case MENU_LAYOUT_NODE_MENU:
          /* Recurse - we recurse first and process the current node
           * second, as the spec dictates.
           */
          cafemenu_tree_execute_moves (tree, child, &need_remove_dups);
          break;

        case MENU_LAYOUT_NODE_MOVE:
          move_nodes = g_slist_prepend (move_nodes, child);
          break;

        default:
          break;
        }

      child = menu_layout_node_get_next (child);
    }

  /* We need to execute the move operations in the order that they appear */
  move_nodes = g_slist_reverse (move_nodes);

  tmp = move_nodes;
  while (tmp != NULL)
    {
      MenuLayoutNode *move_node = tmp->data;
      MenuLayoutNode *old_node;
      GSList         *next = tmp->next;
      const char     *old;
      const char     *new;

      old = menu_layout_node_move_get_old (move_node);
      new = menu_layout_node_move_get_new (move_node);
      g_assert (old != NULL && new != NULL);

      menu_verbose ("executing <Move> old = \"%s\" new = \"%s\"\n",
                    old, new);

      old_node = find_submenu (layout, old, FALSE);
      if (old_node != NULL)
        {
          MenuLayoutNode *new_node;

          /* here we can create duplicates anywhere below the
           * node
           */
          need_remove_dups = TRUE;

          /* look up new node creating it and its parents if
           * required
           */
          new_node = find_submenu (layout, new, TRUE);
          g_assert (new_node != NULL);

          move_children (old_node, new_node);

          menu_layout_node_unlink (old_node);
        }

      menu_layout_node_unlink (move_node);

      tmp = next;
    }

  g_slist_free (move_nodes);

  /* This oddness is to ensure we only remove dups once,
   * at the root, instead of recursing the tree over
   * and over.
   */
  if (need_remove_dups_p)
    *need_remove_dups_p = need_remove_dups;
  else if (need_remove_dups)
    cafemenu_tree_strip_duplicate_children (tree, layout);
}

static gboolean
cafemenu_tree_load_layout (CafeMenuTree  *tree,
                        GError    **error)
{
  GHashTable *loaded_menu_files;

  if (tree->layout)
    return TRUE;

  if (!cafemenu_tree_canonicalize_path (tree, error))
    return FALSE;

  menu_verbose ("Loading menu layout from \"%s\"\n",
                tree->canonical_path);

  tree->layout = menu_layout_load (tree->canonical_path,
                                   tree->non_prefixed_basename,
                                   error);
  if (!tree->layout)
    return FALSE;

  loaded_menu_files = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (loaded_menu_files, tree->canonical_path, GUINT_TO_POINTER (TRUE));
  cafemenu_tree_resolve_files (tree, loaded_menu_files, tree->layout);
  g_hash_table_destroy (loaded_menu_files);

  cafemenu_tree_strip_duplicate_children (tree, tree->layout);
  cafemenu_tree_execute_moves (tree, tree->layout, NULL);

  return TRUE;
}

static void
cafemenu_tree_force_reload (CafeMenuTree *tree)
{
  cafemenu_tree_force_rebuild (tree);

  if (tree->layout)
    menu_layout_node_unref (tree->layout);
  tree->layout = NULL;
}

typedef struct
{
  DesktopEntrySet *set;
  const char      *category;
} GetByCategoryForeachData;

static void
get_by_category_foreach (const char               *file_id,
			 DesktopEntry             *entry,
			 GetByCategoryForeachData *data)
{
  if (desktop_entry_has_category (entry, data->category))
    desktop_entry_set_add_entry (data->set, entry, file_id);
}

static void
get_by_category (DesktopEntrySet *entry_pool,
		 DesktopEntrySet *set,
		 const char      *category)
{
  GetByCategoryForeachData data;

  data.set      = set;
  data.category = category;

  desktop_entry_set_foreach (entry_pool,
			     (DesktopEntrySetForeachFunc) get_by_category_foreach,
			     &data);
}

static DesktopEntrySet *
process_include_rules (MenuLayoutNode  *layout,
		       DesktopEntrySet *entry_pool)
{
  DesktopEntrySet *set = NULL;

  switch (menu_layout_node_get_type (layout))
    {
    case MENU_LAYOUT_NODE_AND:
      {
        MenuLayoutNode *child;

	menu_verbose ("Processing <And>\n");

        child = menu_layout_node_get_children (layout);
        while (child != NULL)
          {
            DesktopEntrySet *child_set;

            child_set = process_include_rules (child, entry_pool);

            if (set == NULL)
              {
                set = child_set;
              }
            else
              {
                desktop_entry_set_intersection (set, child_set);
                desktop_entry_set_unref (child_set);
              }

            /* as soon as we get empty results, we can bail,
             * because it's an AND
             */
            if (desktop_entry_set_get_count (set) == 0)
              break;

            child = menu_layout_node_get_next (child);
          }
	menu_verbose ("Processed <And>\n");
      }
      break;

    case MENU_LAYOUT_NODE_OR:
      {
        MenuLayoutNode *child;

	menu_verbose ("Processing <Or>\n");

        child = menu_layout_node_get_children (layout);
        while (child != NULL)
          {
            DesktopEntrySet *child_set;

            child_set = process_include_rules (child, entry_pool);

            if (set == NULL)
              {
                set = child_set;
              }
            else
              {
                desktop_entry_set_union (set, child_set);
                desktop_entry_set_unref (child_set);
              }

            child = menu_layout_node_get_next (child);
          }
	menu_verbose ("Processed <Or>\n");
      }
      break;

    case MENU_LAYOUT_NODE_NOT:
      {
        /* First get the OR of all the rules */
        MenuLayoutNode *child;

	menu_verbose ("Processing <Not>\n");

        child = menu_layout_node_get_children (layout);
        while (child != NULL)
          {
            DesktopEntrySet *child_set;

            child_set = process_include_rules (child, entry_pool);

            if (set == NULL)
              {
                set = child_set;
              }
            else
              {
                desktop_entry_set_union (set, child_set);
                desktop_entry_set_unref (child_set);
              }

            child = menu_layout_node_get_next (child);
          }

        if (set != NULL)
          {
	    DesktopEntrySet *inverted;

	    /* Now invert the result */
	    inverted = desktop_entry_set_new ();
	    desktop_entry_set_union (inverted, entry_pool);
	    desktop_entry_set_subtract (inverted, set);
	    desktop_entry_set_unref (set);
	    set = inverted;
          }
	menu_verbose ("Processed <Not>\n");
      }
      break;

    case MENU_LAYOUT_NODE_ALL:
      menu_verbose ("Processing <All>\n");
      set = desktop_entry_set_new ();
      desktop_entry_set_union (set, entry_pool);
      menu_verbose ("Processed <All>\n");
      break;

    case MENU_LAYOUT_NODE_FILENAME:
      {
        DesktopEntry *entry;

	menu_verbose ("Processing <Filename>%s</Filename>\n",
		      menu_layout_node_get_content (layout));

        entry = desktop_entry_set_lookup (entry_pool,
					  menu_layout_node_get_content (layout));
        if (entry != NULL)
          {
            set = desktop_entry_set_new ();
            desktop_entry_set_add_entry (set,
                                         entry,
                                         menu_layout_node_get_content (layout));
          }
	menu_verbose ("Processed <Filename>%s</Filename>\n",
		      menu_layout_node_get_content (layout));
      }
      break;

    case MENU_LAYOUT_NODE_CATEGORY:
      menu_verbose ("Processing <Category>%s</Category>\n",
		    menu_layout_node_get_content (layout));
      set = desktop_entry_set_new ();
      get_by_category (entry_pool, set, menu_layout_node_get_content (layout));
      menu_verbose ("Processed <Category>%s</Category>\n",
		    menu_layout_node_get_content (layout));
      break;

    default:
      break;
    }

  if (set == NULL)
    set = desktop_entry_set_new (); /* create an empty set */

  menu_verbose ("Matched %d entries\n", desktop_entry_set_get_count (set));

  return set;
}

static void
collect_layout_info (MenuLayoutNode  *layout,
		     GSList         **layout_info)
{
  MenuLayoutNode *iter;

  g_slist_foreach (*layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (*layout_info);
  *layout_info = NULL;

  iter = menu_layout_node_get_children (layout);
  while (iter != NULL)
    {
      switch (menu_layout_node_get_type (iter))
	{
	case MENU_LAYOUT_NODE_MENUNAME:
	case MENU_LAYOUT_NODE_FILENAME:
	case MENU_LAYOUT_NODE_SEPARATOR:
	case MENU_LAYOUT_NODE_MERGE:
	  *layout_info = g_slist_prepend (*layout_info,
					  menu_layout_node_ref (iter));
	  break;

	default:
	  break;
	}

      iter = menu_layout_node_get_next (iter);
    }

  *layout_info = g_slist_reverse (*layout_info);
}

static void
entries_listify_foreach (const char         *desktop_file_id,
                         DesktopEntry       *desktop_entry,
                         CafeMenuTreeDirectory *directory)
{
  directory->entries =
    g_slist_prepend (directory->entries,
		     cafemenu_tree_entry_new (directory,
                                           desktop_entry,
                                           desktop_file_id,
                                           FALSE,
                                           FALSE));
}

static void
excluded_entries_listify_foreach (const char         *desktop_file_id,
				  DesktopEntry       *desktop_entry,
				  CafeMenuTreeDirectory *directory)
{
  directory->entries =
    g_slist_prepend (directory->entries,
		     cafemenu_tree_entry_new (directory,
					   desktop_entry,
					   desktop_file_id,
					   TRUE,
                                           FALSE));
}

static void
unallocated_entries_listify_foreach (const char         *desktop_file_id,
                                     DesktopEntry       *desktop_entry,
                                     CafeMenuTreeDirectory *directory)
{
  directory->entries =
    g_slist_prepend (directory->entries,
		     cafemenu_tree_entry_new (directory,
                                           desktop_entry,
                                           desktop_file_id,
                                           FALSE,
                                           TRUE));
}

static void
set_default_layout_values (CafeMenuTreeDirectory *parent,
                           CafeMenuTreeDirectory *child)
{
  GSList *tmp;

  /* if the child has a defined default layout, we don't want to override its
   * values. The parent might have a non-defined layout info (ie, no child of
   * the DefaultLayout node) but it doesn't meant the default layout values
   * (ie, DefaultLayout attributes) aren't different from the global defaults.
   */
  if (child->default_layout_info != NULL ||
      child->default_layout_values.mask != MENU_LAYOUT_VALUES_NONE)
    return;

  child->default_layout_values = parent->default_layout_values;

  tmp = child->subdirs;
  while (tmp != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->data;

      set_default_layout_values (child, subdir);

      tmp = tmp->next;
   }
}

static CafeMenuTreeDirectory *
process_layout (CafeMenuTree          *tree,
                CafeMenuTreeDirectory *parent,
                MenuLayoutNode     *layout,
                DesktopEntrySet    *allocated)
{
  MenuLayoutNode     *layout_iter;
  CafeMenuTreeDirectory *directory;
  DesktopEntrySet    *entry_pool;
  DesktopEntrySet    *entries;
  DesktopEntrySet    *allocated_set;
  DesktopEntrySet    *excluded_set;
  gboolean            deleted;
  gboolean            only_unallocated;
  GSList             *tmp;

  g_assert (menu_layout_node_get_type (layout) == MENU_LAYOUT_NODE_MENU);
  g_assert (menu_layout_node_menu_get_name (layout) != NULL);

  directory = cafemenu_tree_directory_new (tree, parent,
					menu_layout_node_menu_get_name (layout));

  menu_verbose ("=== Menu name = %s ===\n", directory->name);


  deleted = FALSE;
  only_unallocated = FALSE;

  entries = desktop_entry_set_new ();
  allocated_set = desktop_entry_set_new ();

  if (tree->flags & CAFEMENU_TREE_FLAGS_INCLUDE_EXCLUDED)
    excluded_set = desktop_entry_set_new ();
  else
    excluded_set = NULL;

  entry_pool = _entry_directory_list_get_all_desktops (menu_layout_node_menu_get_app_dirs (layout));

  layout_iter = menu_layout_node_get_children (layout);
  while (layout_iter != NULL)
    {
      switch (menu_layout_node_get_type (layout_iter))
        {
        case MENU_LAYOUT_NODE_MENU:
          /* recurse */
          {
            CafeMenuTreeDirectory *child_dir;

	    menu_verbose ("Processing <Menu>\n");

            child_dir = process_layout (tree,
                                        directory,
                                        layout_iter,
                                        allocated);
            if (child_dir)
              directory->subdirs = g_slist_prepend (directory->subdirs,
                                                    child_dir);

	    menu_verbose ("Processed <Menu>\n");
          }
          break;

        case MENU_LAYOUT_NODE_INCLUDE:
          {
            /* The match rule children of the <Include> are
             * independent (logical OR) so we can process each one by
             * itself
             */
            MenuLayoutNode *rule;

	    menu_verbose ("Processing <Include> (%d entries)\n",
			  desktop_entry_set_get_count (entries));

            rule = menu_layout_node_get_children (layout_iter);
            while (rule != NULL)
              {
                DesktopEntrySet *rule_set;

                rule_set = process_include_rules (rule, entry_pool);
                if (rule_set != NULL)
                  {
                    desktop_entry_set_union (entries, rule_set);
                    desktop_entry_set_union (allocated_set, rule_set);
		    if (excluded_set != NULL)
		      desktop_entry_set_subtract (excluded_set, rule_set);
                    desktop_entry_set_unref (rule_set);
                  }

                rule = menu_layout_node_get_next (rule);
              }

	    menu_verbose ("Processed <Include> (%d entries)\n",
			  desktop_entry_set_get_count (entries));
          }
          break;

        case MENU_LAYOUT_NODE_EXCLUDE:
          {
            /* The match rule children of the <Exclude> are
             * independent (logical OR) so we can process each one by
             * itself
             */
            MenuLayoutNode *rule;

	    menu_verbose ("Processing <Exclude> (%d entries)\n",
			  desktop_entry_set_get_count (entries));

            rule = menu_layout_node_get_children (layout_iter);
            while (rule != NULL)
              {
                DesktopEntrySet *rule_set;

                rule_set = process_include_rules (rule, entry_pool);
                if (rule_set != NULL)
                  {
		    if (excluded_set != NULL)
		      desktop_entry_set_union (excluded_set, rule_set);
		    desktop_entry_set_subtract (entries, rule_set);
		    desktop_entry_set_unref (rule_set);
                  }

                rule = menu_layout_node_get_next (rule);
              }

	    menu_verbose ("Processed <Exclude> (%d entries)\n",
			  desktop_entry_set_get_count (entries));
          }
          break;

        case MENU_LAYOUT_NODE_DIRECTORY:
          {
            DesktopEntry *entry;

	    menu_verbose ("Processing <Directory>%s</Directory>\n",
			  menu_layout_node_get_content (layout_iter));

	    /*
             * The last <Directory> to exist wins, so we always try overwriting
             */
            entry = entry_directory_list_get_directory (menu_layout_node_menu_get_directory_dirs (layout),
                                                        menu_layout_node_get_content (layout_iter));

            if (entry != NULL)
              {
                if (!desktop_entry_get_hidden (entry))
                  {
                    if (directory->directory_entry)
                      desktop_entry_unref (directory->directory_entry);
                    directory->directory_entry = entry; /* pass ref ownership */
                  }
                else
                  {
                    desktop_entry_unref (entry);
                  }
              }

            menu_verbose ("Processed <Directory> new directory entry = %p (%s)\n",
                          directory->directory_entry,
			  directory->directory_entry? desktop_entry_get_path (directory->directory_entry) : "null");
          }
          break;

        case MENU_LAYOUT_NODE_DELETED:
	  menu_verbose ("Processed <Deleted/>\n");
          deleted = TRUE;
          break;

        case MENU_LAYOUT_NODE_NOT_DELETED:
	  menu_verbose ("Processed <NotDeleted/>\n");
          deleted = FALSE;
          break;

        case MENU_LAYOUT_NODE_ONLY_UNALLOCATED:
	  menu_verbose ("Processed <OnlyUnallocated/>\n");
          only_unallocated = TRUE;
          break;

        case MENU_LAYOUT_NODE_NOT_ONLY_UNALLOCATED:
	  menu_verbose ("Processed <NotOnlyUnallocated/>\n");
          only_unallocated = FALSE;
          break;

	case MENU_LAYOUT_NODE_DEFAULT_LAYOUT:
	  menu_layout_node_default_layout_get_values (layout_iter,
						      &directory->default_layout_values);
	  collect_layout_info (layout_iter, &directory->default_layout_info);
	  menu_verbose ("Processed <DefaultLayout/>\n");
	  break;

	case MENU_LAYOUT_NODE_LAYOUT:
	  collect_layout_info (layout_iter, &directory->layout_info);
	  menu_verbose ("Processed <Layout/>\n");
	  break;

        default:
          break;
        }

      layout_iter = menu_layout_node_get_next (layout_iter);
    }

  desktop_entry_set_unref (entry_pool);

  directory->only_unallocated = only_unallocated;

  if (!directory->only_unallocated)
    desktop_entry_set_union (allocated, allocated_set);

  desktop_entry_set_unref (allocated_set);

  if (directory->directory_entry)
    {
      if (desktop_entry_get_no_display (directory->directory_entry))
        {
          directory->is_nodisplay = TRUE;

          if (!(tree->flags & CAFEMENU_TREE_FLAGS_INCLUDE_NODISPLAY))
            {
              menu_verbose ("Not showing menu %s because NoDisplay=true\n",
                        desktop_entry_get_name (directory->directory_entry));
              deleted = TRUE;
            }
        }

      if (!desktop_entry_get_show_in (directory->directory_entry))
        {
          menu_verbose ("Not showing menu %s because OnlyShowIn!=$DESKTOP or NotShowIn=$DESKTOP (with $DESKTOP=${XDG_CURRENT_DESKTOP:-GNOME})\n",
                        desktop_entry_get_name (directory->directory_entry));
          deleted = TRUE;
        }
    }

  if (deleted)
    {
      if (excluded_set != NULL)
	desktop_entry_set_unref (excluded_set);
      desktop_entry_set_unref (entries);
      cafemenu_tree_item_unref (directory);
      return NULL;
    }

  desktop_entry_set_foreach (entries,
                             (DesktopEntrySetForeachFunc) entries_listify_foreach,
                             directory);
  desktop_entry_set_unref (entries);

  if (excluded_set != NULL)
    {
      desktop_entry_set_foreach (excluded_set,
				 (DesktopEntrySetForeachFunc) excluded_entries_listify_foreach,
				 directory);
      desktop_entry_set_unref (excluded_set);
    }

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->data;

      set_default_layout_values (directory, subdir);

      tmp = tmp->next;
   }

  tmp = directory->entries;
  while (tmp != NULL)
    {
      CafeMenuTreeEntry *entry = tmp->data;
      GSList         *next  = tmp->next;
      gboolean        delete = FALSE;

      /* If adding a new condition to delete here, it has to be added to
       * get_still_unallocated_foreach() too */

      if (desktop_entry_get_hidden (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because Hidden=true\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      if (!(tree->flags & CAFEMENU_TREE_FLAGS_INCLUDE_NODISPLAY) &&
          desktop_entry_get_no_display (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because NoDisplay=true\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      if (!desktop_entry_get_show_in (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because OnlyShowIn!=$DESKTOP or NotShowIn=$DESKTOP (with $DESKTOP=${XDG_CURRENT_DESKTOP:-GNOME})\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      /* No need to filter out based on TryExec since GDesktopAppInfo cannot
       * deal with .desktop files with a failed TryExec. */

      if (delete)
        {
          directory->entries = g_slist_delete_link (directory->entries,
                                                   tmp);
          cafemenu_tree_item_unref_and_unset_parent (entry);
        }

      tmp = next;
    }

  g_assert (directory->name != NULL);

  return directory;
}

static void
process_only_unallocated (CafeMenuTree          *tree,
			  CafeMenuTreeDirectory *directory,
			  DesktopEntrySet    *allocated,
			  DesktopEntrySet    *unallocated_used)
{
  GSList *tmp;

  /* For any directory marked only_unallocated, we have to remove any
   * entries that were in fact allocated.
   */

  if (directory->only_unallocated)
    {
      tmp = directory->entries;
      while (tmp != NULL)
        {
          CafeMenuTreeEntry *entry = tmp->data;
          GSList         *next  = tmp->next;

          if (desktop_entry_set_lookup (allocated, entry->desktop_file_id))
            {
              directory->entries = g_slist_delete_link (directory->entries,
                                                        tmp);
              cafemenu_tree_item_unref_and_unset_parent (entry);
            }
          else
            {
              desktop_entry_set_add_entry (unallocated_used, entry->desktop_entry, entry->desktop_file_id);
            }

          tmp = next;
        }
    }

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->data;

      process_only_unallocated (tree, subdir, allocated, unallocated_used);

      tmp = tmp->next;
   }
}

typedef struct
{
  CafeMenuTree *tree;
  DesktopEntrySet *allocated;
  DesktopEntrySet *unallocated_used;
  DesktopEntrySet *still_unallocated;
} GetStillUnallocatedForeachData;

static void
get_still_unallocated_foreach (const char                     *file_id,
                               DesktopEntry                   *entry,
                               GetStillUnallocatedForeachData *data)
{
  if (desktop_entry_set_lookup (data->allocated, file_id))
    return;

  if (desktop_entry_set_lookup (data->unallocated_used, file_id))
    return;

  /* Same rules than at the end of process_layout() */
  if (desktop_entry_get_hidden (entry))
    return;

  if (!(data->tree->flags & CAFEMENU_TREE_FLAGS_INCLUDE_NODISPLAY) &&
      desktop_entry_get_no_display (entry))
    return;

  if (!desktop_entry_get_show_in (entry))
    return;

  desktop_entry_set_add_entry (data->still_unallocated, entry, file_id);
}

static void preprocess_layout_info (CafeMenuTree          *tree,
                                    CafeMenuTreeDirectory *directory);

static GSList *
get_layout_info (CafeMenuTreeDirectory *directory,
                 gboolean           *is_default_layout)
{
  CafeMenuTreeDirectory *iter;

  if (directory->layout_info != NULL)
    {
      if (is_default_layout)
        {
          *is_default_layout = FALSE;
        }
      return directory->layout_info;
    }

  /* Even if there's no layout information at all, the result will be an
   * implicit default layout */
  if (is_default_layout)
    {
      *is_default_layout = TRUE;
    }

  iter = directory;
  while (iter != NULL)
    {
      /* FIXME: this is broken: we might skip real parent in the
       * XML structure, that are hidden because of inlining. */
      if (iter->default_layout_info != NULL)
	{
	  return iter->default_layout_info;
	}

      iter = CAFEMENU_TREE_ITEM (iter)->parent;
    }

  return NULL;
}

static void
get_values_with_defaults (MenuLayoutNode   *node,
			  MenuLayoutValues *layout_values,
			  MenuLayoutValues *default_layout_values)
{
  menu_layout_node_menuname_get_values (node, layout_values);

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_SHOW_EMPTY))
    layout_values->show_empty = default_layout_values->show_empty;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_MENUS))
    layout_values->inline_menus = default_layout_values->inline_menus;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_LIMIT))
    layout_values->inline_limit = default_layout_values->inline_limit;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_HEADER))
    layout_values->inline_header = default_layout_values->inline_header;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_ALIAS))
    layout_values->inline_alias = default_layout_values->inline_alias;
}

static guint
get_real_subdirs_len (CafeMenuTreeDirectory *directory)
{
  guint   len;
  GSList *tmp;

  len = 0;

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->data;

      tmp = tmp->next;

      if (subdir->will_inline_header != G_MAXUINT16)
        {
          len += get_real_subdirs_len (subdir) + g_slist_length (subdir->entries) + 1;
        }
      else
        len += 1;
    }

  return len;
}

static void
preprocess_layout_info_subdir_helper (CafeMenuTree          *tree,
                                      CafeMenuTreeDirectory *directory,
                                      CafeMenuTreeDirectory *subdir,
                                      MenuLayoutValues   *layout_values,
                                      gboolean           *contents_added,
                                      gboolean           *should_remove)
{
  preprocess_layout_info (tree, subdir);

  *should_remove = FALSE;
  *contents_added = FALSE;

  if (subdir->subdirs == NULL && subdir->entries == NULL)
    {
      if (!(tree->flags & CAFEMENU_TREE_FLAGS_SHOW_EMPTY) &&
          !layout_values->show_empty)
	{
	  menu_verbose ("Not showing empty menu '%s'\n", subdir->name);
	  *should_remove = TRUE;
	}
    }

  else if (layout_values->inline_menus)
    {
      guint real_subdirs_len;

      real_subdirs_len = get_real_subdirs_len (subdir);

      if (layout_values->inline_alias &&
          real_subdirs_len + g_slist_length (subdir->entries) == 1)
        {
          CafeMenuTreeAlias *alias;
          CafeMenuTreeItem  *item;
          GSList         *list;

          if (subdir->subdirs != NULL)
            list = subdir->subdirs;
          else
            list = subdir->entries;

          item = CAFEMENU_TREE_ITEM (list->data);

          menu_verbose ("Inline aliasing '%s' to '%s'\n",
                        item->type == CAFEMENU_TREE_ITEM_ENTRY ?
                          g_app_info_get_name (G_APP_INFO (cafemenu_tree_entry_get_app_info (CAFEMENU_TREE_ENTRY (item)))) :
                          (item->type == CAFEMENU_TREE_ITEM_DIRECTORY ?
                             cafemenu_tree_directory_get_name (CAFEMENU_TREE_DIRECTORY (item)) :
                             cafemenu_tree_directory_get_name (CAFEMENU_TREE_ALIAS (item)->directory)),
                        subdir->name);

          alias = cafemenu_tree_alias_new (directory, subdir, item);

          g_slist_foreach (list,
                           (GFunc) cafemenu_tree_item_unref_and_unset_parent,
                           NULL);
          g_slist_free (list);
          subdir->subdirs = NULL;
          subdir->entries = NULL;

          if (item->type == CAFEMENU_TREE_ITEM_DIRECTORY)
            directory->subdirs = g_slist_append (directory->subdirs, alias);
          else
            directory->entries = g_slist_append (directory->entries, alias);

          *contents_added = TRUE;
          *should_remove = TRUE;
        }

      else if (layout_values->inline_limit == 0 ||
               layout_values->inline_limit >= real_subdirs_len + g_slist_length (subdir->entries))
        {
          if (layout_values->inline_header)
            {
              menu_verbose ("Creating inline header with name '%s'\n", subdir->name);
              /* we're limited to 16-bits to spare some memory; if the limit is
               * higher than that (would be crazy), we just consider it's
               * unlimited */
              if (layout_values->inline_limit < G_MAXUINT16)
                subdir->will_inline_header = layout_values->inline_limit;
              else
                subdir->will_inline_header = 0;
            }
          else
            {
              g_slist_foreach (subdir->subdirs,
                               (GFunc) cafemenu_tree_item_set_parent,
                               directory);
              directory->subdirs = g_slist_concat (directory->subdirs,
                                                   subdir->subdirs);
              subdir->subdirs = NULL;

              g_slist_foreach (subdir->entries,
                               (GFunc) cafemenu_tree_item_set_parent,
                               directory);
              directory->entries = g_slist_concat (directory->entries,
                                                   subdir->entries);
              subdir->entries = NULL;

              *contents_added = TRUE;
              *should_remove = TRUE;
            }

          menu_verbose ("Inlining directory contents of '%s' to '%s'\n",
                        subdir->name, directory->name);
        }
    }
}

static void
preprocess_layout_info (CafeMenuTree          *tree,
                        CafeMenuTreeDirectory *directory)
{
  GSList   *tmp;
  GSList   *layout_info;
  gboolean  using_default_layout;
  GSList   *last_subdir;
  gboolean  strip_duplicates;
  gboolean  contents_added;
  gboolean  should_remove;
  GSList   *subdirs_sentinel;

  /* Note: we need to preprocess all menus, even if the layout mask for a menu
   * is MENU_LAYOUT_VALUES_NONE: in this case, we need to remove empty menus;
   * and the layout mask can be different for a submenu anyway */

  menu_verbose ("Processing menu layout inline hints for %s\n", directory->name);
  g_assert (!directory->preprocessed);

  strip_duplicates = FALSE;
  /* we use last_subdir to track the last non-inlined subdirectory */
  last_subdir = g_slist_last (directory->subdirs);

  /*
   * First process subdirectories with explicit layout
   */
  layout_info = get_layout_info (directory, &using_default_layout);
  tmp = layout_info;
  /* see comment below about Menuname to understand why we leave the loop if
   * last_subdir is NULL */
  while (tmp != NULL && last_subdir != NULL)
    {
      MenuLayoutNode     *node = tmp->data;
      MenuLayoutValues    layout_values;
      const char         *name;
      CafeMenuTreeDirectory *subdir;
      GSList             *subdir_l;

      tmp = tmp->next;

      /* only Menuname nodes are relevant here */
      if (menu_layout_node_get_type (node) != MENU_LAYOUT_NODE_MENUNAME)
        continue;

      get_values_with_defaults (node,
                                &layout_values,
                                &directory->default_layout_values);

      /* find the subdirectory that is affected by those attributes */
      name = menu_layout_node_get_content (node);
      subdir = NULL;
      subdir_l = directory->subdirs;
      while (subdir_l != NULL)
        {
          subdir = subdir_l->data;

          if (!strcmp (subdir->name, name))
            break;

          subdir = NULL;
          subdir_l = subdir_l->next;

          /* We do not want to use Menuname on a menu that appeared via
           * inlining: without inlining, the Menuname wouldn't have matched
           * anything, and we want to keep the same behavior.
           * Unless the layout is a default layout, in which case the Menuname
           * does match the subdirectory. */
          if (!using_default_layout && subdir_l == last_subdir)
            {
              subdir_l = NULL;
              break;
            }
        }

      if (subdir == NULL)
        continue;

      preprocess_layout_info_subdir_helper (tree, directory,
                                            subdir, &layout_values,
                                            &contents_added, &should_remove);
      strip_duplicates = strip_duplicates || contents_added;
      if (should_remove)
        {
          if (last_subdir == subdir_l)
            {
              /* we need to recompute last_subdir since we'll remove it from
               * the list */
              GSList *buf;

              if (subdir_l == directory->subdirs)
                last_subdir = NULL;
              else
                {
                  buf = directory->subdirs;
                  while (buf != NULL && buf->next != subdir_l)
                    buf = buf->next;
                  last_subdir = buf;
                }
            }

          directory->subdirs = g_slist_remove (directory->subdirs, subdir);
          cafemenu_tree_item_unref_and_unset_parent (CAFEMENU_TREE_ITEM (subdir));
        }
    }

  /*
   * Now process the subdirectories with no explicit layout
   */
  /* this is bogus data, but we just need the pointer anyway */
  subdirs_sentinel = g_slist_prepend (directory->subdirs, PACKAGE);
  directory->subdirs = subdirs_sentinel;

  tmp = directory->subdirs;
  while (tmp->next != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->next->data;

      if (subdir->preprocessed)
        {
          tmp = tmp->next;
          continue;
        }

      preprocess_layout_info_subdir_helper (tree, directory,
                                            subdir, &directory->default_layout_values,
                                            &contents_added, &should_remove);
      strip_duplicates = strip_duplicates || contents_added;
      if (should_remove)
        {
          tmp = g_slist_delete_link (tmp, tmp->next);
          cafemenu_tree_item_unref_and_unset_parent (CAFEMENU_TREE_ITEM (subdir));
        }
      else
        tmp = tmp->next;
    }

  /* remove the sentinel */
  directory->subdirs = g_slist_delete_link (directory->subdirs,
                                            directory->subdirs);

  /*
   * Finally, remove duplicates if needed
   */
  if (strip_duplicates)
    {
      /* strip duplicate entries; there should be no duplicate directories */
      directory->entries = g_slist_sort (directory->entries,
                                         (GCompareFunc) cafemenu_tree_entry_compare_by_id);
      tmp = directory->entries;
      while (tmp != NULL && tmp->next != NULL)
        {
          CafeMenuTreeItem *a = tmp->data;
          CafeMenuTreeItem *b = tmp->next->data;

          if (a->type == CAFEMENU_TREE_ITEM_ALIAS)
            a = CAFEMENU_TREE_ALIAS (a)->aliased_item;

          if (b->type == CAFEMENU_TREE_ITEM_ALIAS)
            b = CAFEMENU_TREE_ALIAS (b)->aliased_item;

          if (strcmp (CAFEMENU_TREE_ENTRY (a)->desktop_file_id,
                      CAFEMENU_TREE_ENTRY (b)->desktop_file_id) == 0)
            {
              tmp = g_slist_delete_link (tmp, tmp->next);
              cafemenu_tree_item_unref (b);
            }
          else
            tmp = tmp->next;
        }
    }

  directory->preprocessed = TRUE;
}

static void process_layout_info (CafeMenuTree          *tree,
				 CafeMenuTreeDirectory *directory);

static void
check_pending_separator (CafeMenuTreeDirectory *directory)
{
  if (directory->layout_pending_separator)
    {
      menu_verbose ("Adding pending separator in '%s'\n", directory->name);

      directory->contents = g_slist_append (directory->contents,
					    cafemenu_tree_separator_new (directory));
      directory->layout_pending_separator = FALSE;
    }
}

static void
merge_alias (CafeMenuTree          *tree,
	     CafeMenuTreeDirectory *directory,
	     CafeMenuTreeAlias     *alias)
{
  menu_verbose ("Merging alias '%s' in directory '%s'\n",
		alias->directory->name, directory->name);

  if (alias->aliased_item->type == CAFEMENU_TREE_ITEM_DIRECTORY)
    {
      process_layout_info (tree, CAFEMENU_TREE_DIRECTORY (alias->aliased_item));
    }

  check_pending_separator (directory);

  directory->contents = g_slist_append (directory->contents,
					cafemenu_tree_item_ref (alias));
}

static void
merge_subdir (CafeMenuTree          *tree,
	      CafeMenuTreeDirectory *directory,
	      CafeMenuTreeDirectory *subdir)
{
  menu_verbose ("Merging subdir '%s' in directory '%s'\n",
		subdir->name, directory->name);

  process_layout_info (tree, subdir);

  check_pending_separator (directory);

  if (subdir->will_inline_header == 0 ||
      (subdir->will_inline_header != G_MAXUINT16 &&
       g_slist_length (subdir->contents) <= subdir->will_inline_header))
    {
      CafeMenuTreeHeader *header;

      header = cafemenu_tree_header_new (directory, subdir);
      directory->contents = g_slist_append (directory->contents, header);

      g_slist_foreach (subdir->contents,
                       (GFunc) cafemenu_tree_item_set_parent,
                       directory);
      directory->contents = g_slist_concat (directory->contents,
                                            subdir->contents);
      subdir->contents = NULL;
      subdir->will_inline_header = G_MAXUINT16;

      cafemenu_tree_item_set_parent (CAFEMENU_TREE_ITEM (subdir), NULL);
    }
  else
    {
      directory->contents = g_slist_append (directory->contents,
					    cafemenu_tree_item_ref (subdir));
    }
}

static void
merge_subdir_by_name (CafeMenuTree          *tree,
		      CafeMenuTreeDirectory *directory,
		      const char         *subdir_name)
{
  GSList *tmp;

  menu_verbose ("Attempting to merge subdir '%s' in directory '%s'\n",
		subdir_name, directory->name);

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->data;
      GSList             *next = tmp->next;

      /* if it's an alias, then it cannot be affected by
       * the Merge nodes in the layout */
      if (CAFEMENU_TREE_ITEM (subdir)->type == CAFEMENU_TREE_ITEM_ALIAS)
        continue;

      if (!strcmp (subdir->name, subdir_name))
	{
	  directory->subdirs = g_slist_delete_link (directory->subdirs, tmp);
	  merge_subdir (tree, directory, subdir);
	  cafemenu_tree_item_unref (subdir);
	}

      tmp = next;
    }
}

static void
merge_entry (CafeMenuTree          *tree G_GNUC_UNUSED,
	     CafeMenuTreeDirectory *directory,
	     CafeMenuTreeEntry     *entry)
{
  menu_verbose ("Merging entry '%s' in directory '%s'\n",
		entry->desktop_file_id, directory->name);

  check_pending_separator (directory);
  directory->contents = g_slist_append (directory->contents,
					cafemenu_tree_item_ref (entry));
}

static void
merge_entry_by_id (CafeMenuTree          *tree,
		   CafeMenuTreeDirectory *directory,
		   const char         *file_id)
{
  GSList *tmp;

  menu_verbose ("Attempting to merge entry '%s' in directory '%s'\n",
		file_id, directory->name);

  tmp = directory->entries;
  while (tmp != NULL)
    {
      CafeMenuTreeEntry *entry = tmp->data;
      GSList         *next = tmp->next;

      /* if it's an alias, then it cannot be affected by
       * the Merge nodes in the layout */
      if (CAFEMENU_TREE_ITEM (entry)->type == CAFEMENU_TREE_ITEM_ALIAS)
        continue;

      if (!strcmp (entry->desktop_file_id, file_id))
	{
	  directory->entries = g_slist_delete_link (directory->entries, tmp);
	  merge_entry (tree, directory, entry);
	  cafemenu_tree_item_unref (entry);
	}

      tmp = next;
    }
}

static inline gboolean
find_name_in_list (const char *name,
		   GSList     *list)
{
  while (list != NULL)
    {
      if (!strcmp (name, list->data))
	return TRUE;

      list = list->next;
    }

  return FALSE;
}

static void
merge_subdirs (CafeMenuTree          *tree,
	       CafeMenuTreeDirectory *directory,
	       GSList             *except)
{
  GSList *subdirs;
  GSList *tmp;

  menu_verbose ("Merging subdirs in directory '%s'\n", directory->name);

  subdirs = directory->subdirs;
  directory->subdirs = NULL;

  subdirs = g_slist_sort_with_data (subdirs,
				    (GCompareDataFunc) cafemenu_tree_item_compare,
                                    GINT_TO_POINTER (CAFEMENU_TREE_FLAGS_NONE));

  tmp = subdirs;
  while (tmp != NULL)
    {
      CafeMenuTreeDirectory *subdir = tmp->data;

      if (CAFEMENU_TREE_ITEM (subdir)->type == CAFEMENU_TREE_ITEM_ALIAS)
        {
	  merge_alias (tree, directory, CAFEMENU_TREE_ALIAS (subdir));
	  cafemenu_tree_item_unref (subdir);
        }
      else if (!find_name_in_list (subdir->name, except))
	{
	  merge_subdir (tree, directory, subdir);
	  cafemenu_tree_item_unref (subdir);
	}
      else
	{
	  menu_verbose ("Not merging directory '%s' yet\n", subdir->name);
	  directory->subdirs = g_slist_append (directory->subdirs, subdir);
	}

      tmp = tmp->next;
    }

  g_slist_free (subdirs);
  g_slist_free (except);
}

static void
merge_entries (CafeMenuTree          *tree,
	       CafeMenuTreeDirectory *directory,
	       GSList             *except)
{
  GSList *entries;
  GSList *tmp;

  menu_verbose ("Merging entries in directory '%s'\n", directory->name);

  entries = directory->entries;
  directory->entries = NULL;

  entries = g_slist_sort_with_data (entries,
				    (GCompareDataFunc) cafemenu_tree_item_compare,
                                    GINT_TO_POINTER (tree->flags));

  tmp = entries;
  while (tmp != NULL)
    {
      CafeMenuTreeEntry *entry = tmp->data;

      if (CAFEMENU_TREE_ITEM (entry)->type == CAFEMENU_TREE_ITEM_ALIAS)
        {
	  merge_alias (tree, directory, CAFEMENU_TREE_ALIAS (entry));
	  cafemenu_tree_item_unref (entry);
        }
      else if (!find_name_in_list (entry->desktop_file_id, except))
	{
	  merge_entry (tree, directory, entry);
	  cafemenu_tree_item_unref (entry);
	}
      else
	{
	  menu_verbose ("Not merging entry '%s' yet\n", entry->desktop_file_id);
	  directory->entries = g_slist_append (directory->entries, entry);
	}

      tmp = tmp->next;
    }

  g_slist_free (entries);
  g_slist_free (except);
}

static void
merge_subdirs_and_entries (CafeMenuTree          *tree,
			   CafeMenuTreeDirectory *directory,
			   GSList             *except_subdirs,
			   GSList             *except_entries)
{
  GSList *items;
  GSList *tmp;

  menu_verbose ("Merging subdirs and entries together in directory %s\n",
		directory->name);

  items = g_slist_concat (directory->subdirs, directory->entries);

  directory->subdirs = NULL;
  directory->entries = NULL;

  items = g_slist_sort_with_data (items,
				  (GCompareDataFunc) cafemenu_tree_item_compare,
                                  GINT_TO_POINTER (tree->flags));

  tmp = items;
  while (tmp != NULL)
    {
      CafeMenuTreeItem     *item = tmp->data;
      CafeMenuTreeItemType  type;

      type = item->type;

      if (type == CAFEMENU_TREE_ITEM_ALIAS)
        {
          merge_alias (tree, directory, CAFEMENU_TREE_ALIAS (item));
          cafemenu_tree_item_unref (item);
        }
      else if (type == CAFEMENU_TREE_ITEM_DIRECTORY)
	{
	  if (!find_name_in_list (CAFEMENU_TREE_DIRECTORY (item)->name, except_subdirs))
	    {
	      merge_subdir (tree,
			    directory,
			    CAFEMENU_TREE_DIRECTORY (item));
	      cafemenu_tree_item_unref (item);
	    }
	  else
	    {
	      menu_verbose ("Not merging directory '%s' yet\n",
			    CAFEMENU_TREE_DIRECTORY (item)->name);
	      directory->subdirs = g_slist_append (directory->subdirs, item);
	    }
	}
      else if (type == CAFEMENU_TREE_ITEM_ENTRY)
	{
	  if (!find_name_in_list (CAFEMENU_TREE_ENTRY (item)->desktop_file_id, except_entries))
	    {
	      merge_entry (tree, directory, CAFEMENU_TREE_ENTRY (item));
	      cafemenu_tree_item_unref (item);
	    }
	  else
	    {
	      menu_verbose ("Not merging entry '%s' yet\n",
			    CAFEMENU_TREE_ENTRY (item)->desktop_file_id);
	      directory->entries = g_slist_append (directory->entries, item);
	    }
	}
      else
        {
          g_assert_not_reached ();
        }

      tmp = tmp->next;
    }

  g_slist_free (items);
  g_slist_free (except_subdirs);
  g_slist_free (except_entries);
}

static GSList *
get_subdirs_from_layout_info (GSList *layout_info)
{
  GSList *subdirs;
  GSList *tmp;

  subdirs = NULL;

  tmp = layout_info;
  while (tmp != NULL)
    {
      MenuLayoutNode *node = tmp->data;

      if (menu_layout_node_get_type (node) == MENU_LAYOUT_NODE_MENUNAME)
	{
	  subdirs = g_slist_append (subdirs,
				    (char *) menu_layout_node_get_content (node));
	}

      tmp = tmp->next;
    }

  return subdirs;
}

static GSList *
get_entries_from_layout_info (GSList *layout_info)
{
  GSList *entries;
  GSList *tmp;

  entries = NULL;

  tmp = layout_info;
  while (tmp != NULL)
    {
      MenuLayoutNode *node = tmp->data;

      if (menu_layout_node_get_type (node) == MENU_LAYOUT_NODE_FILENAME)
	{
	  entries = g_slist_append (entries,
				    (char *) menu_layout_node_get_content (node));
	}

      tmp = tmp->next;
    }

  return entries;
}

static void
process_layout_info (CafeMenuTree          *tree,
		     CafeMenuTreeDirectory *directory)
{
  GSList *layout_info;

  menu_verbose ("Processing menu layout hints for %s\n", directory->name);

  g_slist_foreach (directory->contents,
		   (GFunc) cafemenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->contents);
  directory->contents = NULL;
  directory->layout_pending_separator = FALSE;

  layout_info = get_layout_info (directory, NULL);

  if (layout_info == NULL)
    {
      merge_subdirs (tree, directory, NULL);
      merge_entries (tree, directory, NULL);
    }
  else
    {
      GSList *tmp;

      tmp = layout_info;
      while (tmp != NULL)
	{
	  MenuLayoutNode *node = tmp->data;

	  switch (menu_layout_node_get_type (node))
	    {
	    case MENU_LAYOUT_NODE_MENUNAME:
              merge_subdir_by_name (tree,
                                    directory,
                                    menu_layout_node_get_content (node));
	      break;

	    case MENU_LAYOUT_NODE_FILENAME:
	      merge_entry_by_id (tree,
				 directory,
				 menu_layout_node_get_content (node));
	      break;

	    case MENU_LAYOUT_NODE_SEPARATOR:
	      /* Unless explicitly told to show all separators, do not show a
	       * separator at the beginning of a menu. Note that we don't add
	       * the separators now, and instead make it pending. This way, we
	       * won't show two consecutive separators nor will we show a
	       * separator at the end of a menu. */
              if (tree->flags & CAFEMENU_TREE_FLAGS_SHOW_ALL_SEPARATORS)
		{
		  directory->layout_pending_separator = TRUE;
		  check_pending_separator (directory);
		}
	      else if (directory->contents)
		{
		  menu_verbose ("Adding a potential separator in '%s'\n",
				directory->name);

		  directory->layout_pending_separator = TRUE;
		}
	      else
		{
		  menu_verbose ("Skipping separator at the beginning of '%s'\n",
				directory->name);
		}
	      break;

	    case MENU_LAYOUT_NODE_MERGE:
	      switch (menu_layout_node_merge_get_type (node))
		{
		case MENU_LAYOUT_MERGE_NONE:
		  break;

		case MENU_LAYOUT_MERGE_MENUS:
		  merge_subdirs (tree,
				 directory,
				 get_subdirs_from_layout_info (tmp->next));
		  break;

		case MENU_LAYOUT_MERGE_FILES:
		  merge_entries (tree,
				 directory,
				 get_entries_from_layout_info (tmp->next));
		  break;

		case MENU_LAYOUT_MERGE_ALL:
		  merge_subdirs_and_entries (tree,
					     directory,
					     get_subdirs_from_layout_info (tmp->next),
					     get_entries_from_layout_info (tmp->next));
		  break;

		default:
		  g_assert_not_reached ();
		  break;
		}
	      break;

	    default:
	      g_assert_not_reached ();
	      break;
	    }

	  tmp = tmp->next;
	}
    }

  g_slist_foreach (directory->subdirs,
		   (GFunc) cafemenu_tree_item_unref,
		   NULL);
  g_slist_free (directory->subdirs);
  directory->subdirs = NULL;

  g_slist_foreach (directory->entries,
		   (GFunc) cafemenu_tree_item_unref,
		   NULL);
  g_slist_free (directory->entries);
  directory->entries = NULL;

  g_slist_foreach (directory->default_layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->default_layout_info);
  directory->default_layout_info = NULL;

  g_slist_foreach (directory->layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->layout_info);
  directory->layout_info = NULL;
}

static void
handle_entries_changed (MenuLayoutNode *layout,
                        CafeMenuTree       *tree)
{
  if (tree->layout == layout)
    {
      cafemenu_tree_force_rebuild (tree);
      cafemenu_tree_invoke_monitors (tree);
    }
}

static void
update_entry_index (CafeMenuTree           *tree,
		    CafeMenuTreeDirectory  *dir)
{
  CafeMenuTreeIter *iter = cafemenu_tree_directory_iter (dir);
  CafeMenuTreeItemType next_type;

  while ((next_type = cafemenu_tree_iter_next (iter)) != CAFEMENU_TREE_ITEM_INVALID)
    {
      gpointer item = NULL;

      switch (next_type)
        {
        case CAFEMENU_TREE_ITEM_ENTRY:
          {
	    const char *id;

            item = cafemenu_tree_iter_get_entry (iter);
            id = cafemenu_tree_entry_get_desktop_file_id (item);
            if (id != NULL)
              g_hash_table_insert (tree->entries_by_id, (char*)id, item);
          }
          break;
        case CAFEMENU_TREE_ITEM_DIRECTORY:
          {
            item = cafemenu_tree_iter_get_directory (iter);
            update_entry_index (tree, (CafeMenuTreeDirectory*)item);
          }
          break;
        default:
          break;
        }
      if (item != NULL)
        cafemenu_tree_item_unref (item);
    }

  cafemenu_tree_iter_unref (iter);
}

static gboolean
cafemenu_tree_build_from_layout (CafeMenuTree  *tree,
                              GError    **error)
{
  DesktopEntrySet *allocated;

  if (tree->root)
    return TRUE;

  if (!cafemenu_tree_load_layout (tree, error))
    return FALSE;

  menu_verbose ("Building menu tree from layout\n");

  allocated = desktop_entry_set_new ();

  /* create the menu structure */
  tree->root = process_layout (tree,
                               NULL,
                               find_menu_child (tree->layout),
                               allocated);
  if (tree->root)
    {
      DesktopEntrySet *unallocated_used;

      unallocated_used = desktop_entry_set_new ();

      process_only_unallocated (tree, tree->root, allocated, unallocated_used);
      if (tree->flags & CAFEMENU_TREE_FLAGS_INCLUDE_UNALLOCATED)
        {
          DesktopEntrySet *entry_pool;
          DesktopEntrySet *still_unallocated;
          GetStillUnallocatedForeachData data;

          entry_pool = _entry_directory_list_get_all_desktops (menu_layout_node_menu_get_app_dirs (find_menu_child (tree->layout)));
          still_unallocated = desktop_entry_set_new ();

          data.tree = tree;
          data.allocated = allocated;
          data.unallocated_used = unallocated_used;
          data.still_unallocated = still_unallocated;

          desktop_entry_set_foreach (entry_pool,
                                     (DesktopEntrySetForeachFunc) get_still_unallocated_foreach,
                                     &data);

          desktop_entry_set_unref (entry_pool);

          desktop_entry_set_foreach (still_unallocated,
                                     (DesktopEntrySetForeachFunc) unallocated_entries_listify_foreach,
                                     tree->root);

          desktop_entry_set_unref (still_unallocated);
        }

      desktop_entry_set_unref (unallocated_used);

      /* process the layout info part that can move/remove items:
       * inline, show_empty, etc. */
      preprocess_layout_info (tree, tree->root);
      /* populate the menu structure that we got with the items, and order it
       * according to the layout info */
      process_layout_info (tree, tree->root);

      update_entry_index (tree, tree->root);

      menu_layout_node_root_add_entries_monitor (tree->layout,
                                                 (MenuLayoutNodeEntriesChangedFunc) handle_entries_changed,
                                                 tree);
    }

  desktop_entry_set_unref (allocated);

  return TRUE;
}

static void
cafemenu_tree_force_rebuild (CafeMenuTree *tree)
{
  if (tree->root)
    {
      g_hash_table_remove_all (tree->entries_by_id);
      cafemenu_tree_item_unref (tree->root);
      tree->root = NULL;
      tree->loaded = FALSE;

      g_assert (tree->layout != NULL);

      menu_layout_node_root_remove_entries_monitor (tree->layout,
                                                    (MenuLayoutNodeEntriesChangedFunc) handle_entries_changed,
                                                    tree);
    }
}

GType
cafemenu_tree_iter_get_type (void)
{
  static GType gtype = G_TYPE_INVALID;
  if (gtype == G_TYPE_INVALID)
    {
      gtype = g_boxed_type_register_static ("CafeMenuTreeIter",
          (GBoxedCopyFunc)cafemenu_tree_iter_ref,
          (GBoxedFreeFunc)cafemenu_tree_iter_unref);
    }
  return gtype;
}

GType
cafemenu_tree_directory_get_type (void)
{
  static GType gtype = G_TYPE_INVALID;
  if (gtype == G_TYPE_INVALID)
    {
      gtype = g_boxed_type_register_static ("CafeMenuTreeDirectory",
          (GBoxedCopyFunc)cafemenu_tree_item_ref,
          (GBoxedFreeFunc)cafemenu_tree_item_unref);
    }
  return gtype;
}

GType
cafemenu_tree_entry_get_type (void)
{
  static GType gtype = G_TYPE_INVALID;
  if (gtype == G_TYPE_INVALID)
    {
      gtype = g_boxed_type_register_static ("CafeMenuTreeEntry",
          (GBoxedCopyFunc)cafemenu_tree_item_ref,
          (GBoxedFreeFunc)cafemenu_tree_item_unref);
    }
  return gtype;
}

GType
cafemenu_tree_separator_get_type (void)
{
  static GType gtype = G_TYPE_INVALID;
  if (gtype == G_TYPE_INVALID)
    {
      gtype = g_boxed_type_register_static ("CafeMenuTreeSeparator",
          (GBoxedCopyFunc)cafemenu_tree_item_ref,
          (GBoxedFreeFunc)cafemenu_tree_item_unref);
    }
  return gtype;
}

GType
cafemenu_tree_header_get_type (void)
{
  static GType gtype = G_TYPE_INVALID;
  if (gtype == G_TYPE_INVALID)
    {
      gtype = g_boxed_type_register_static ("CafeMenuTreeHeader",
          (GBoxedCopyFunc)cafemenu_tree_item_ref,
          (GBoxedFreeFunc)cafemenu_tree_item_unref);
    }
  return gtype;
}

GType
cafemenu_tree_alias_get_type (void)
{
  static GType gtype = G_TYPE_INVALID;
  if (gtype == G_TYPE_INVALID)
    {
      gtype = g_boxed_type_register_static ("CafeMenuTreeAlias",
          (GBoxedCopyFunc)cafemenu_tree_item_ref,
          (GBoxedFreeFunc)cafemenu_tree_item_unref);
    }
  return gtype;
}

GType
cafemenu_tree_flags_get_type (void)
{
  static GType enum_type_id = 0;
  if (G_UNLIKELY (!enum_type_id))
    {
      static const GFlagsValue values[] = {
        { CAFEMENU_TREE_FLAGS_NONE, "CAFEMENU_TREE_FLAGS_NONE", "none" },
        { CAFEMENU_TREE_FLAGS_INCLUDE_EXCLUDED, "CAFEMENU_TREE_FLAGS_INCLUDE_EXCLUDED", "include-excluded" },
        { CAFEMENU_TREE_FLAGS_SHOW_EMPTY, "CAFEMENU_TREE_FLAGS_SHOW_EMPTY", "show-empty" },
        { CAFEMENU_TREE_FLAGS_INCLUDE_NODISPLAY, "CAFEMENU_TREE_FLAGS_INCLUDE_NODISPLAY", "include-nodisplay" },
        { CAFEMENU_TREE_FLAGS_SHOW_ALL_SEPARATORS, "CAFEMENU_TREE_FLAGS_SHOW_ALL_SEPARATORS", "show-all-separators" },
        { CAFEMENU_TREE_FLAGS_SORT_DISPLAY_NAME, "CAFEMENU_TREE_FLAGS_SORT_DISPLAY_NAME", "sort-display-name" },
        { CAFEMENU_TREE_FLAGS_INCLUDE_UNALLOCATED, "CAFEMENU_TREE_FLAGS_INCLUDE_UNALLOCATED,", "include-unallocated" },
        { 0, NULL, NULL }
      };
      enum_type_id = g_flags_register_static ("CafeMenuTreeFlags", values);
    }
  return enum_type_id;
}
