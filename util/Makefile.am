noinst_PROGRAMS = cafe-menu-spec-test

AM_CPPFLAGS = \
	$(GIO_UNIX_CFLAGS)					\
	-I$(srcdir)/../libmenu \
	-DMATEMENU_I_KNOW_THIS_IS_UNSTABLE \
	-DMATELOCALEDIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	$(DISABLE_DEPRECATED_CFLAGS) \
	$(DEBUG_CFLAGS)

AM_CFLAGS = $(WARN_CFLAGS)

cafe_menu_spec_test_SOURCES = \
	test-menu-spec.c

cafe_menu_spec_test_LDADD = \
	$(GIO_UNIX_LIBS)		\
	../libmenu/libcafe-menu.la

exampledir = $(pkgdatadir)/examples
example_DATA = cafe-menus-ls.py

EXTRA_DIST = $(example_DATA)

-include $(top_srcdir)/git.mk

