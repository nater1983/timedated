/*
  Copyright 2012 Alexandre Rostovtsev

  Some parts are based on the code from the systemd project; these are
  copyright 2011 Lennart Poettering and others.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dbus/dbus-protocol.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#if HAVE_OPENRC
#include <rc.h>
#endif

#include "copypaste/hwclock.h"
#include "timedated.h"
#include "timedate1-generated.h"
#include "main.h"
#include "utils.h"

#define SERVICE_NAME "timedated"

static guint bus_id = 0;
static gboolean read_only = FALSE;

static TimedatedTimedate1 *timedate1 = NULL;

static GFile *hwclock_file = NULL;
static GFile *timezone_file = NULL;
static GFile *localtime_file = NULL;

#define ZONEINFODIR DATADIR "/zoneinfo"

gboolean local_rtc = FALSE;
gchar *timezone_name = NULL;
G_LOCK_DEFINE_STATIC (clock);

gboolean use_ntp = FALSE;
static const gchar *ntp_preferred_service = NULL;
static const gchar *ntp_default_services[] = { "ntpd", "chronyd", "busybox-ntpd", NULL };
#define NTP_DEFAULT_SERVICES_PACKAGES "ntp, openntpd, chrony, busybox-ntpd"
G_LOCK_DEFINE_STATIC (ntp);

static gboolean
get_local_rtc (GError **error)
{
    gchar *clock = NULL;
    gboolean ret = FALSE;

    clock = shell_source_var (hwclock_file, "${clock}", error);
    if (!g_strcmp0 (clock, "local"))
        ret = TRUE;
    g_free (clock);
    return ret;
}

static gchar *
get_timezone_name (GError **error)
{
    g_autoptr(GTimeZone) tz = g_time_zone_new_identifier (NULL);

    return g_strdup (g_time_zone_get_identifier (tz));
}

static gboolean
set_timezone_file (const gchar *identifier,
                   GError **error)
{
    g_autofree gchar *timezone_filename = NULL;

    g_return_val_if_fail (error != NULL, FALSE);

    /* We don't actually own the timezone file, but it's something distros
     * need to take care of installing if they use it, which not all do.
     * So if it doesn't exist, don't create it, it's not our responsibility */
    if (!g_file_query_exists (timezone_file, NULL))
        return TRUE;

    timezone_filename = g_file_get_path (timezone_file);
    if (!g_file_replace_contents (timezone_file, identifier, strlen (identifier),
                                  NULL, FALSE, 0, NULL, NULL, error)) {
        g_prefix_error (error, "Unable to write '%s':", timezone_filename);
        return FALSE;
    }
    if(g_chmod (timezone_filename, 0664) != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unable to set 0664 permissions on timezone file '%s'", timezone_filename);
    }

    return TRUE;
}

static gboolean
set_localtime_file (const gchar *identifier,
                    GError **error)
{
    g_autofree gchar *filebuf = NULL, *localtime_filename = NULL, *identifier_filename = NULL;
    g_autoptr(GFile) identifier_file = NULL;
    gsize length = 0;

    g_return_val_if_fail (error != NULL, FALSE);

    localtime_filename = g_file_get_path (localtime_file);
    identifier_filename = g_strdup_printf (ZONEINFODIR "/%s", identifier);
    identifier_file = g_file_new_for_path (identifier_filename);

    if (g_file_test(localtime_filename, G_FILE_TEST_IS_SYMLINK)) {
        if (!g_file_delete (localtime_file, NULL, error)) {
            g_prefix_error (error, "Unable to delete file to make new symlink %s:", localtime_filename);
            return FALSE;
        }
        if (!g_file_make_symbolic_link (localtime_file, identifier_filename, NULL, error)) {
            g_prefix_error (error, "Unable to create symlink %s -> %s:", localtime_filename, identifier_filename);
            return FALSE;
        }
    } else if (g_file_test(localtime_filename, G_FILE_TEST_IS_REGULAR)) {
        if (!g_file_load_contents (identifier_file, NULL, &filebuf, &length, NULL, error)) {
            g_prefix_error (error, "Unable to read '%s':", identifier_filename);
            return FALSE;
        }
        if (!g_file_replace_contents (localtime_file, filebuf, length, NULL, FALSE, 0, NULL, NULL, error)) {
            g_prefix_error (error, "Unable to write '%s':", localtime_filename);
            return FALSE;
        }
        if(g_chmod (localtime_filename, 0664) != 0) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unable to set 0664 permissions on localtime file '%s'", localtime_filename);
        }
    } else {
        // File doesn't exist yet -> make a new symlink
        if (!g_file_make_symbolic_link (localtime_file, identifier_filename, NULL, error)) {
            g_prefix_error (error, "Unable to create symlink %s -> %s:", localtime_filename, identifier_filename);
            return FALSE;
        }
    }

    return TRUE;
}
static gboolean
set_local_rtc (const gchar *identifier,
               GError **error)
{
    gchar *clock = NULL;

    g_return_val_if_fail (error != NULL, FALSE);

    clock = g_file_read_link (hwclock_file, NULL, error);
    if (!clock) {
        g_prefix_error (error, "Unable to read RTC clock symlink:");
        return FALSE;
    }

    if (g_strcmp0 (clock, identifier) == 0)
        return TRUE;

    if (!g_file_delete (hwclock_file, NULL, error)) {
        g_prefix_error (error, "Unable to delete file to make new symlink %s:", g_file_get_path (hwclock_file));
        return FALSE;
    }
    if (!g_file_make_symbolic_link (hwclock_file, identifier, NULL, error)) {
        g_prefix_error (error, "Unable to create symlink %s -> %s:", g_file_get_path (hwclock_file), identifier);
        return FALSE;
    }

    return TRUE;
}

static gboolean
check_hwclock (void)
{
    gboolean is_local_rtc;
    gchar *rtc_identifier = NULL;
    gboolean is_local_rtc_systemd;

    G_LOCK (clock);
    is_local_rtc = get_local_rtc (NULL);
    rtc_identifier = get_timezone_name (NULL);
    G_UNLOCK (clock);

    is_local_rtc_systemd = is_local_rtc;

    if (is_local_rtc) {
        gboolean ret;
        G_LOCK (clock);
        ret = set_local_rtc (rtc_identifier, NULL);
        G_UNLOCK (clock);
        if (!ret)
            is_local_rtc_systemd = FALSE;
    }

    return is_local_rtc_systemd;
}

static gboolean
check_hardware (void)
{
    gboolean check;
    g_autofree gchar *rtcname = NULL;

    if (!g_file_test ("/dev/rtc", G_FILE_TEST_EXISTS))
        return FALSE;

    G_LOCK (clock);
    rtcname = shell_source_var (hwclock_file, "${rtc}", NULL);
    G_UNLOCK (clock);

    if (rtcname && g_strcmp0 (rtcname, "/dev/rtc") == 0)
        check = TRUE;
    else
        check = FALSE;

    return check;
}

#if HAVE_OPENRC
static gboolean
read_openrc_hwclock (void)
{
    g_autofree gchar *hwclock_value = NULL;

    hwclock_value = rc_conf_value (NULL, NULL, "clock");
    if (hwclock_value && g_ascii_strcasecmp (hwclock_value, "localtime") == 0)
        return TRUE;

    return FALSE;
}
#endif

static gboolean
check_hwclock_from_proc_cmdline (void)
{
    gboolean check;
    g_autofree gchar *cmdline = NULL;
    g_autofree gchar **tokens = NULL;
    gchar **iter;

    cmdline = g_file_get_contents ("/proc/cmdline", NULL, NULL);
    if (!cmdline)
        return FALSE;

    tokens = g_strsplit_set (cmdline, " \t", -1);
    for (iter = tokens; iter && *iter; iter++) {
        gchar *token = *iter;
        gchar **param = g_strsplit (token, "=", 2);
        if (param && g_strv_length (param) == 2) {
            if (g_strcmp0 (param[0], "rtc") == 0 && g_strcmp0 (param[1], "local") == 0) {
                check = TRUE;
                g_strfreev (param);
                break;
            }
        }
        g_strfreev (param);
    }

    g_strfreev (tokens);

    return check;
}

static gboolean
check_hwclock_from_proc_cmdline_initrd (void)
{
    gboolean check;
    g_autofree gchar *cmdline = NULL;
    g_autofree gchar **tokens = NULL;
    gchar **iter;

    cmdline = g_file_get_contents ("/proc/cmdline", NULL, NULL);
    if (!cmdline)
        return FALSE;

    tokens = g_strsplit_set (cmdline, " \t", -1);
    for (iter = tokens; iter && *iter; iter++) {
        gchar *token = *iter;
        gchar **param = g_strsplit (token, "=", 2);
        if (param && g_strv_length (param) == 2) {
            if (g_strcmp0 (param[0], "rd.rtc") == 0 && g_strcmp0 (param[1], "local") == 0) {
                check = TRUE;
                g_strfreev (param);
                break;
            }
        }
        g_strfreev (param);
    }

    g_strfreev (tokens);

    return check;
}

static gboolean
get_ntp_servers (const gchar *service,
                 GStrv *servers,
                 GError **error)
{
    g_autoptr(GKeyFile) keyfile = NULL;
    g_autoptr(GFile) file = NULL;
    g_autofree gchar *ntp_conf = NULL;

    file = g_file_new_for_path ("/etc/ntp.conf");
    if (!g_file_query_exists (file, NULL)) {
        *servers = g_strdupv (ntp_default_services);
        return TRUE;
    }

    ntp_conf = g_file_get_path (file);
    keyfile = g_key_file_new ();
    if (!g_key_file_load_from_file (keyfile, ntp_conf, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, error)) {
        g_prefix_error (error, "Failed to load NTP configuration from '%s':", ntp_conf);
        return FALSE;
    }

    *servers = g_key_file_get_string_list (keyfile, service, "servers", NULL, NULL);
    return TRUE;
}

static gboolean
set_ntp (void)
{
    gboolean ok = FALSE;
    GStrv servers = NULL;
    g_autofree gchar *chosen_service = NULL;

    G_LOCK (ntp);

    if (!use_ntp) {
        ok = TRUE;
        goto out;
    }

    if (ntp_preferred_service)
        chosen_service = g_strdup (ntp_preferred_service);
    else
        chosen_service = g_strdupv (ntp_default_services)[0];

    if (!get_ntp_servers (chosen_service, &servers, NULL))
        goto out;

    if (g_strv_length (servers) > 0) {
        ok = TRUE;
    } else {
        use_ntp = FALSE;
    }

out:
    G_UNLOCK (ntp);
    g_strfreev (servers);

    return ok;
}

static gboolean
write_hwclock (void)
{
    gboolean ok = TRUE;
    gboolean ret;
    g_autofree gchar *tz = NULL;

    G_LOCK (clock);
    tz = get_timezone_name (NULL);
    G_UNLOCK (clock);

    if (local_rtc) {
        G_LOCK (clock);
        ret = set_local_rtc (tz, NULL);
        G_UNLOCK (clock);
        if (!ret)
            ok = FALSE;
    }

    return ok;
}

static void
on_dbus_name_acquired (GDBusConnection *connection,
                       const gchar *name,
                       gpointer user_data)
{
    GDBusObjectManagerServer *manager = user_data;

    bus_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                             name,
                             G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_dbus_bus_acquired,
                             on_dbus_name_acquired,
                             on_dbus_name_lost,
                             manager,
                             NULL);
}

static void
on_dbus_name_lost (GDBusConnection *connection,
                   const gchar *name,
                   gpointer user_data)
{
    /* Lost the name on the bus. Quit. */
    g_main_loop_quit (user_data);
}

int
main (int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(GDBusObjectManagerServer) manager = NULL;
    g_autoptr(GDBusConnection) connection = NULL;
    gint ret = EXIT_FAILURE;

    context = g_option_context_new ("- timedated");
    g_option_context_add_group (context, g_irepository_get_option_group (timedate1_get_type ()));
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("Error parsing command line arguments: %s\n", error->message);
        return EXIT_FAILURE;
    }

    loop = g_main_loop_new (NULL, FALSE);

    manager = g_dbus_object_manager_server_new (MANAGER_PATH);
    g_dbus_object_manager_server_set_connection (manager, NULL);

    connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        g_printerr ("Error connecting to D-Bus: %s\n", error->message);
        return EXIT_FAILURE;
    }

    bus_id = g_bus_own_name_on_connection (connection,
                                           SERVICE_NAME,
                                           G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                           G_BUS_NAME_OWNER_FLAGS_REPLACE_EXISTING,
                                           on_dbus_name_acquired,
                                           on_dbus_name_lost,
                                           manager,
                                           NULL,
                                           NULL);

    g_main_loop_run (loop);

    if (bus_id > 0) {
        g_bus_unown_name (bus_id);
        bus_id = 0;
    }

    return ret;
}
