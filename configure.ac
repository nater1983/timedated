AC_PREREQ([2.60])
AC_INIT([timedated], [0.1], [https://github.com/nater1983/timedated/issues], [timedated], [https://github.com/nater1983/timedated/])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([check-news foreign 1.11 -Wall -Wno-portability silent-rules tar-pax no-dist-gzip dist-xz])
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AC_CONFIG_MACRO_DIR([m4])
AM_SILENT_RULES([yes])
LT_INIT([disable-static pic-only])

AC_PROG_MKDIR_P
PKG_CHECK_MODULES(TIMEDATED,
                  [gio-unix-2.0 >= 2.44
                   gio-2.0 >= 2.44
                   glib-2.0 >= 2.44
                   dbus-1
                   polkit-gobject-1
                   libdaemon])
AC_SUBST(BLOCALED_CFLAGS)
AC_SUBST(BLOCALED_LIBS)

AC_PATH_PROG(GDBUS_CODEGEN, gdbus-codegen)
if test "x$GDBUS_CODEGEN" = x; then
    AC_MSG_ERROR([Failed to find gdbus-codegen])
fi

AC_ARG_WITH([pidfile], AS_HELP_STRING([--with-pidfile=FILENAME], [pid filename @<:@default=/run/timedated.pid@:>@]), [], [with_pidfile=/run/timedated.pid])
AC_SUBST([pidfile], [$with_pidfile])

AC_ARG_WITH([timedateconfig], AS_HELP_STRING([--with-timedateconfig=FILENAME], [timedate config filename @<:@default=/etc/timedate.conf@:>@]), [], [with_timedateconfig=/etc/timedate.conf])
AC_SUBST([timedateconfig], [$with_timedateconfig])

AC_MSG_CHECKING([dbus interfaces directory])
dbusinterfacesdir=`$PKG_CONFIG --variable=interfaces_dir dbus-1 \
                               --define-variable=prefix=$prefix`
if test "x$dbusinterfacesdir" = "x"; then
    AC_MSG_ERROR([not found])
else
    AC_MSG_RESULT([$dbusinterfacesdir])
fi
AC_SUBST([dbusinterfacesdir])

AC_MSG_CHECKING([dbus system services directory])
dbussystemservicesdir=`$PKG_CONFIG --variable=system_bus_services_dir dbus-1 \
                                   --define-variable=prefix=$prefix`
if test "x$dbussystemservicesdir" = "x"; then
    AC_MSG_ERROR([not found])
else
    AC_MSG_RESULT([$dbussystemservicesdir])
fi
AC_SUBST([dbussystemservicesdir])

AC_MSG_CHECKING([polkit actions directory])
polkitactiondir=`$PKG_CONFIG --variable=actiondir polkit-gobject-1 \
                             --define-variable=prefix=$prefix`
if test "x$polkitactiondir" = "x"; then
    AC_MSG_ERROR([not found])
else
    AC_MSG_RESULT([$polkitactiondir])
fi
AC_SUBST([polkitactiondir])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
    Makefile
    data/timedated.conf
    data/timedated.8
    tests/Makefile
    tests/scratch/org.freedesktop.timedate1.service
    tests/scratch/test-session.xml
])

AC_OUTPUT
AC_MSG_RESULT([
        $PACKAGE $VERSION
        =====

        prefix:                   ${prefix}
        sysconfdir:               ${sysconfdir}

        pid file:                 ${with_pidfile}
        timedate config file:       ${with_timedateconfig}

        compiler:                 ${CC}
        cflags:                   ${CFLAGS}
        ldflags:                  ${LDFLAGS}
])
