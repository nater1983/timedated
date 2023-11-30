ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

AUTOMAKE_OPTIONS = subdir-objects

# TODO add a Makefile.am to src, and add it to SUBDIRS
SUBDIRS = . tests

EXTRA_DIST = \
        data/timedated.8.in \
        data/timedated.conf.in \
	data/org.freedesktop.timedate1.service.in \
	AUTHORS \
	COPYING \
	README \
	NEWS \
	TODO \
	$(NULL)

dist_pkgdata_DATA = \
	$(NULL)

sysconf_DATA = data/timedated.conf

dbusinterfacesdir = @dbusinterfacesdir@
dist_dbusinterfaces_DATA = \
	data/org.freedesktop.timedate1.xml \
	$(NULL)

dbusservicesdir = @dbussystemservicesdir@
dbusservices_DATA = \
	data/org.freedesktop.timedate1.service \
	$(NULL)

dbusbusconfigdir = $(sysconfdir)/dbus-1/system.d
dist_dbusbusconfig_DATA = \
	data/org.freedesktop.timedate1.conf \
	$(NULL)

polkitactiondir = @polkitactiondir@
dist_polkitaction_DATA = \
	data/org.freedesktop.timedate1.policy \
	$(NULL)

pidfile = @pidfile@
do_subst = $(SED) -e 's,[@]libexecdir[@],$(libexecdir),g' \
	-e 's,[@]pidfile[@],$(pidfile),g' \
	$(NULL)

%.service : %.service.in
#	$(do_subst) < $(srcdir)/$< > $(builddir)/$@
	$(do_subst) < $< > $@

man_MANS = data/timedated.8

AM_CPPFLAGS = \
	-include $(top_builddir)/config.h \
	-DSYSCONFDIR=\""$(sysconfdir)"\" \
	-DDATADIR=\""$(datadir)"\" \
	-DLIBEXECDIR=\""$(libexecdir)"\" \
	-DPKGDATADIR=\""$(pkgdatadir)"\" \
	-DPIDFILE=\""$(pidfile)"\" \
	-DTIMEDATECONFIG=\""$(timedateconfig)"\" \
	$(TIMEDATED_CFLAGS) \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src \
	$(NULL)

LDADD = $(TIMEDATED_LIBS)

libexec_PROGRAMS = timedated

timedated_built_sources = \
	src/timedate1-generated.c \
	src/timedate1-generated.h \
	$(NULL)

timedated_SOURCES = \
	src/timedated.c \
	src/timedated.h \
	src/shellparser.c \
	src/shellparser.h \
	src/polkitasync.c \
	src/polkitasync.h \
	src/main.h \
	src/main.c \
	$(NULL)

nodist_timedated_SOURCES = \
	$(timedated_built_sources) \
	$(NULL)

$(timedated_built_sources) : data/org.freedesktop.timedate1.xml
	$(AM_V_GEN)( pushd "$(builddir)/src" > /dev/null; \
	$(GDBUS_CODEGEN) \
	--interface-prefix org.freedesktop. \
	--c-namespace Timedated \
	--generate-c-code timedate1-generated \
	$(abs_srcdir)/data/org.freedesktop.timedate1.xml; \
	popd > /dev/null )

BUILT_SOURCES = \
	$(timedated_built_sources) \
	$(NULL)

CLEANFILES = \
	$(timedated_built_sources) \
	$(dbusservices_DATA) \
	$(NULL)
