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

#ifndef __MATEMENU_TREE_H__
#define __MATEMENU_TREE_H__

#ifndef MATEMENU_I_KNOW_THIS_IS_UNSTABLE
#error "libcafe-menu should only be used if you understand that it's subject to frequent change, and is not supported as a fixed API/ABI or as part of the platform"
#endif

#include <gio/gdesktopappinfo.h>

G_BEGIN_DECLS

#define MATEMENU_TYPE_TREE         (cafemenu_tree_get_type ())
#define MATEMENU_TREE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), MATEMENU_TYPE_TREE, CafeMenuTree))
#define MATEMENU_TREE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), MATEMENU_TYPE_TREE, CafeMenuTreeClass))
#define MATEMENU_IS_TREE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), MATEMENU_TYPE_TREE))
#define MATEMENU_IS_TREE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), MATEMENU_TYPE_TREE))
#define MATEMENU_TREE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DESKTOP_APP_INFO, CafeMenuTreeClass))

typedef struct _CafeMenuTree        CafeMenuTree;
typedef struct _CafeMenuTreeClass   CafeMenuTreeClass;

struct _CafeMenuTreeClass
{
  GObjectClass parent_class;
};

GType cafemenu_tree_get_type (void) G_GNUC_CONST;

typedef struct CafeMenuTreeIter      CafeMenuTreeIter;
typedef struct CafeMenuTreeDirectory CafeMenuTreeDirectory;
typedef struct CafeMenuTreeEntry     CafeMenuTreeEntry;
typedef struct CafeMenuTreeSeparator CafeMenuTreeSeparator;
typedef struct CafeMenuTreeHeader    CafeMenuTreeHeader;
typedef struct CafeMenuTreeAlias     CafeMenuTreeAlias;

typedef enum
{
  MATEMENU_TREE_ITEM_INVALID = 0,
  MATEMENU_TREE_ITEM_DIRECTORY,
  MATEMENU_TREE_ITEM_ENTRY,
  MATEMENU_TREE_ITEM_SEPARATOR,
  MATEMENU_TREE_ITEM_HEADER,
  MATEMENU_TREE_ITEM_ALIAS
} CafeMenuTreeItemType;

GType cafemenu_tree_iter_get_type (void);

/* Explicitly skip item, it's a "hidden" base class */
GType cafemenu_tree_directory_get_type (void);
GType cafemenu_tree_entry_get_type (void);
GType cafemenu_tree_separator_get_type (void);
GType cafemenu_tree_header_get_type (void);
GType cafemenu_tree_alias_get_type (void);

typedef enum
{
  MATEMENU_TREE_FLAGS_NONE                = 0,
  MATEMENU_TREE_FLAGS_INCLUDE_EXCLUDED    = 1 << 0,
  MATEMENU_TREE_FLAGS_INCLUDE_NODISPLAY   = 1 << 1,
  MATEMENU_TREE_FLAGS_INCLUDE_UNALLOCATED = 1 << 2,
  /* leave some space for more include flags */
  MATEMENU_TREE_FLAGS_SHOW_EMPTY          = 1 << 8,
  MATEMENU_TREE_FLAGS_SHOW_ALL_SEPARATORS = 1 << 9,
  /* leave some space for more show flags */
  MATEMENU_TREE_FLAGS_SORT_DISPLAY_NAME   = 1 << 16
} CafeMenuTreeFlags;
GType cafemenu_tree_flags_get_type (void);
#define MATEMENU_TYPE_TREE_FLAGS (cafemenu_tree_flags_get_type ())

CafeMenuTree *cafemenu_tree_new (const char     *menu_basename,
                           CafeMenuTreeFlags  flags);

CafeMenuTree *cafemenu_tree_new_for_path (const char     *menu_path,
                                    CafeMenuTreeFlags  flags);

gboolean   cafemenu_tree_load_sync (CafeMenuTree  *tree,
                                 GError    **error);

const char         *cafemenu_tree_get_canonical_menu_path (CafeMenuTree  *tree);
CafeMenuTreeDirectory *cafemenu_tree_get_root_directory      (CafeMenuTree  *tree);
CafeMenuTreeDirectory *cafemenu_tree_get_directory_from_path (CafeMenuTree  *tree,
							const char *path);
CafeMenuTreeEntry     *cafemenu_tree_get_entry_by_id         (CafeMenuTree  *tree,
							const char *id);

gpointer cafemenu_tree_item_ref   (gpointer item);
void     cafemenu_tree_item_unref (gpointer item);

CafeMenuTreeDirectory *cafemenu_tree_directory_get_parent    (CafeMenuTreeDirectory *directory);
const char *cafemenu_tree_directory_get_name              (CafeMenuTreeDirectory *directory);
const char *cafemenu_tree_directory_get_generic_name      (CafeMenuTreeDirectory *directory);
const char *cafemenu_tree_directory_get_comment           (CafeMenuTreeDirectory *directory);
GIcon      *cafemenu_tree_directory_get_icon              (CafeMenuTreeDirectory *directory);
const char *cafemenu_tree_directory_get_desktop_file_path (CafeMenuTreeDirectory *directory);
const char *cafemenu_tree_directory_get_menu_id           (CafeMenuTreeDirectory *directory);
CafeMenuTree  *cafemenu_tree_directory_get_tree              (CafeMenuTreeDirectory *directory);

gboolean cafemenu_tree_directory_get_is_nodisplay (CafeMenuTreeDirectory *directory);

CafeMenuTreeIter      *cafemenu_tree_directory_iter            (CafeMenuTreeDirectory *directory);

CafeMenuTreeIter      *cafemenu_tree_iter_ref                  (CafeMenuTreeIter *iter);
void                cafemenu_tree_iter_unref                (CafeMenuTreeIter *iter);

CafeMenuTreeItemType   cafemenu_tree_iter_next                 (CafeMenuTreeIter *iter);
CafeMenuTreeDirectory *cafemenu_tree_iter_get_directory        (CafeMenuTreeIter *iter);
CafeMenuTreeEntry     *cafemenu_tree_iter_get_entry            (CafeMenuTreeIter *iter);
CafeMenuTreeHeader    *cafemenu_tree_iter_get_header           (CafeMenuTreeIter *iter);
CafeMenuTreeAlias     *cafemenu_tree_iter_get_alias            (CafeMenuTreeIter *iter);
CafeMenuTreeSeparator *cafemenu_tree_iter_get_separator        (CafeMenuTreeIter *iter);

char *cafemenu_tree_directory_make_path (CafeMenuTreeDirectory *directory,
				      CafeMenuTreeEntry     *entry);


GDesktopAppInfo    *cafemenu_tree_entry_get_app_info       (CafeMenuTreeEntry *entry);
CafeMenuTreeDirectory *cafemenu_tree_entry_get_parent         (CafeMenuTreeEntry *entry);
CafeMenuTree          *cafemenu_tree_entry_get_tree           (CafeMenuTreeEntry *entry);

const char *cafemenu_tree_entry_get_desktop_file_path (CafeMenuTreeEntry *entry);
const char *cafemenu_tree_entry_get_desktop_file_id   (CafeMenuTreeEntry *entry);

gboolean cafemenu_tree_entry_get_is_nodisplay_recurse  (CafeMenuTreeEntry *entry);
gboolean cafemenu_tree_entry_get_is_excluded  (CafeMenuTreeEntry *entry);
gboolean cafemenu_tree_entry_get_is_unallocated  (CafeMenuTreeEntry *entry);

CafeMenuTreeDirectory *cafemenu_tree_header_get_directory (CafeMenuTreeHeader *header);
CafeMenuTree          *cafemenu_tree_header_get_tree      (CafeMenuTreeHeader *header);
CafeMenuTreeDirectory *cafemenu_tree_header_get_parent    (CafeMenuTreeHeader *header);

CafeMenuTreeDirectory *cafemenu_tree_alias_get_directory         (CafeMenuTreeAlias *alias);
CafeMenuTreeItemType   cafemenu_tree_alias_get_aliased_item_type (CafeMenuTreeAlias *alias);
CafeMenuTreeDirectory *cafemenu_tree_alias_get_aliased_directory (CafeMenuTreeAlias *alias);
CafeMenuTreeEntry     *cafemenu_tree_alias_get_aliased_entry     (CafeMenuTreeAlias *alias);
CafeMenuTree          *cafemenu_tree_alias_get_tree              (CafeMenuTreeAlias *alias);
CafeMenuTreeDirectory *cafemenu_tree_alias_get_parent            (CafeMenuTreeAlias *alias);

CafeMenuTree          *cafemenu_tree_separator_get_tree (CafeMenuTreeSeparator *separator);
CafeMenuTreeDirectory *cafemenu_tree_separator_get_parent (CafeMenuTreeSeparator *separator);

G_END_DECLS

#endif /* __MATEMENU_TREE_H__ */
