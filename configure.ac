AC_PREREQ(2.62)

AC_INIT([cafe-menus], [1.25.0], [https://cafe-desktop.org])
AC_CONFIG_SRCDIR(libmenu/cafemenu-tree.h)

AM_INIT_AUTOMAKE([1.9 foreign no-dist-gzip dist-xz check-news])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS(config.h)

AM_MAINTAINER_MODE
CAFE_MAINTAINER_MODE_DEFINES

# Before making a release, the LT_VERSION string should be modified.
# The string is of the form C:R:A.
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to C+1:0:A+1
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to C+1:0:0
# - If the interface is the same as the previous version, change to C:R+1:A

LIB_MENU_LT_VERSION=6:9:4
AC_SUBST(LIB_MENU_LT_VERSION)

AM_GNU_GETTEXT_VERSION([0.19.8])
AM_GNU_GETTEXT_REQUIRE_VERSION([0.19.8])
AM_GNU_GETTEXT([external])

GETTEXT_PACKAGE=cafe-menus
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, "$GETTEXT_PACKAGE", [Name of default gettext domain])
AC_SUBST(GETTEXT_PACKAGE)

AC_PROG_CC
AC_ARG_PROGRAM
LT_INIT

PKG_CHECK_MODULES(GIO_UNIX, gio-unix-2.0 >= 2.50.0)
AC_SUBST(GIO_UNIX_CFLAGS)
AC_SUBST(GIO_UNIX_LIBS)

CAFE_COMPILE_WARNINGS(yes)

AC_ARG_ENABLE(deprecation_flags,
              [AS_HELP_STRING([--enable-deprecation-flags],
                              [use *_DISABLE_DEPRECATED flags @<:@default=no@:>@])],,
              [enable_deprecation_flags=no])

if test "x$enable_deprecation_flags" = "xyes"; then
   DISABLE_DEPRECATED_CFLAGS=$DISABLE_DEPRECATED
   AC_SUBST(DISABLE_DEPRECATED_CFLAGS)
fi

dnl --enable-debug=(yes|minimum|no)
AC_ARG_ENABLE(debug,
              [AS_HELP_STRING([--enable-debug],
                              [turn on debugging @<:@default=minimum@:>@])],,
              [enable_debug=minimum])
if test "$enable_debug" = "yes"; then
  DEBUG_CFLAGS="-DG_ENABLE_DEBUG"
else
  if test "x$enable_debug" = "xno"; then
    DEBUG_CFLAGS="-DG_DISABLE_ASSERT -DG_DISABLE_CHECKS"
  else
    DEBUG_CFLAGS=""
    # make sure we have the right string for the configure summary
    enable_debug="minimum"
  fi
fi
AC_SUBST(DEBUG_CFLAGS)

GOBJECT_INTROSPECTION_CHECK([0.6.7])

AC_CONFIG_FILES([
Makefile
libmenu/Makefile
libmenu/libcafe-menu.pc
libmenu/libcafe-menu-uninstalled.pc
layout/Makefile
desktop-directories/Makefile
util/Makefile
po/Makefile.in
])

AC_OUTPUT

dnl ---------------------------------------------------------------------------
dnl - Show summary
dnl ---------------------------------------------------------------------------

echo "
              cafe-menus $VERSION
              `echo cafe-menus $VERSION | sed "s/./=/g"`

        prefix:                       ${prefix}
        exec_prefix:                  ${exec_prefix}
        libdir:                       ${libdir}
        bindir:                       ${bindir}
        sbindir:                      ${sbindir}
        sysconfdir:                   ${sysconfdir}
        localstatedir:                ${localstatedir}
        datadir:                      ${datadir}
        source code location:         ${srcdir}
        compiler:                     ${CC}
        cflags:                       ${CFLAGS}
        Warning flags:                ${WARN_CFLAGS}
        Maintainer mode:              ${USE_MAINTAINER_MODE}
        Use *_DISABLE_DEPRECATED:     ${enable_deprecation_flags}

        Turn on debugging:            ${enable_debug}
        Build introspection support:  ${found_introspection}

"
