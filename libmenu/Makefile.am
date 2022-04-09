lib_LTLIBRARIES = libcafe-menu.la

AM_CPPFLAGS = \
	$(GIO_UNIX_CFLAGS)		\
	-DMATEMENU_I_KNOW_THIS_IS_UNSTABLE	\
	$(DISABLE_DEPRECATED_CFLAGS) \
	$(DEBUG_CFLAGS)

AM_CFLAGS = $(WARN_CFLAGS)

libcafe_menu_includedir = $(includedir)/cafe-menus
libcafe_menu_include_HEADERS = \
	cafemenu-tree.h

libcafe_menu_sources = \
	desktop-entries.c \
	entry-directories.c \
	cafemenu-tree.c \
	menu-layout.c \
	menu-monitor.c \
	menu-util.c

libcafe_menu_la_SOURCES = \
	$(libcafe_menu_sources) \
	desktop-entries.h \
	entry-directories.h \
	cafemenu-tree.h \
	menu-layout.h \
	menu-monitor.h \
	menu-util.h

libcafe_menu_la_LIBADD = \
	$(GIO_UNIX_LIBS)

libcafe_menu_la_LDFLAGS = \
	-version-info $(LIB_MENU_LT_VERSION) \
	-no-undefined \
	-export-symbols-regex cafemenu_tree

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libcafe-menu.pc

EXTRA_DIST = \
	libcafe-menu.pc.in \
	libcafe-menu-uninstalled.pc.in

CLEANFILES =

# Introspection
-include $(INTROSPECTION_MAKEFILE)
INTROSPECTION_GIRS =
INTROSPECTION_SCANNER_ARGS = --warn-all --add-include-path=$(srcdir)
INTROSPECTION_COMPILER_ARGS = --includedir=$(srcdir)

if HAVE_INTROSPECTION
# Note: we only include the headers here so far because there's no gtk-doc at all anyway
introspection_sources = $(libcafe_menu_include_HEADERS) cafemenu-tree.c

CafeMenu-2.0.gir: libcafe-menu.la
CafeMenu_2_0_gir_INCLUDES = Gio-2.0
CafeMenu_2_0_gir_CFLAGS = $(AM_CPPFLAGS)
CafeMenu_2_0_gir_LIBS = libcafe-menu.la
CafeMenu_2_0_gir_SCANNERFLAGS = --identifier-prefix=CafeMenu --symbol-prefix=cafemenu --pkg-export=libcafe-menu --c-include=cafemenu-tree.h
CafeMenu_2_0_gir_FILES = $(addprefix $(srcdir)/,$(introspection_sources))
INTROSPECTION_GIRS += CafeMenu-2.0.gir

girdir = $(datadir)/gir-1.0/
gir_DATA = $(INTROSPECTION_GIRS)

typelibdir = $(libdir)/girepository-1.0/
typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)

CLEANFILES += $(gir_DATA) $(typelib_DATA)
endif

-include $(top_srcdir)/git.mk
