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
set_timezone (const gchar *identifier,
              GError **error)
{
    if (!set_timezone_file (identifier, error)) {
        g_autofree gchar *timezone_filename = g_file_get_path (timezone_file);
        g_debug ("Error setting %s: %s", timezone_filename, (*error)->message);
        g_clear_error (error);
    }
    if (!set_localtime_file (identifier, error))
        return FALSE;

    return TRUE;
}

/* Return the ntp rc service we will use; return value should NOT be freed */
static const gchar *
ntp_service ()
{
#if HAVE_OPENRC
    const gchar * const *s = NULL;
    const gchar *service = NULL;
    gchar *runlevel = NULL;

    if (ntp_preferred_service != NULL)
        return ntp_preferred_service;

    runlevel = rc_runlevel_get();
    for (s = ntp_default_services; *s != NULL; s++) {
        if (!rc_service_exists (*s))
            continue;
        if (service == NULL)
            service = *s;
        if (rc_service_in_runlevel (*s, runlevel)) {
            service = *s;
            break;
        }
    }
    free (runlevel);

    return service;
#else
    return NULL;
#endif
}

static gboolean
service_started (const gchar *service,
                 GError **error)
{
#if HAVE_OPENRC
    RC_SERVICE state;

    g_assert (service != NULL);

    if (!rc_service_exists (service)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s rc service not found", service);
        return FALSE;
    }

    state = rc_service_state (service);
    return state == RC_SERVICE_STARTED || state == RC_SERVICE_STARTING || state == RC_SERVICE_INACTIVE;
#else
    return FALSE;
#endif
}

static gboolean
service_disable (const gchar *service,
                 GError **error)
{
#if HAVE_OPENRC
    gchar *runlevel = NULL;
    gchar *service_script = NULL;
    const gchar *argv[3] = { NULL, "stop", NULL };
    gboolean ret = FALSE;
    gint exit_status = 0;

    g_assert (service != NULL);

    if (!rc_service_exists (service)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s rc service not found", service);
        goto out;
    }

    runlevel = rc_runlevel_get();
    if (rc_service_in_runlevel (service, runlevel)) {
        g_debug ("Removing %s rc service from %s runlevel", service, runlevel);
        if (!rc_service_delete (runlevel, service))
            g_warning ("Failed to remove %s rc service from %s runlevel", service, runlevel);
    }

    if ((service_script = rc_service_resolve (service)) == NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s rc service does not resolve", service);
        goto out;
    }

    g_debug ("Stopping %s rc service", service);
    argv[0] = service_script;
    if (!g_spawn_sync (NULL, (gchar **)argv, NULL, 0, NULL, NULL, NULL, NULL, &exit_status, error)) {
        g_prefix_error (error, "Failed to spawn %s rc service:", service);
        goto out;
    }
    if (exit_status) {
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "%s rc service failed to stop with exit status %d", service, exit_status);
        goto out;
    }
    ret = TRUE;

  out:
    if (runlevel != NULL)
        free (runlevel);
    if (service_script != NULL)
        free (service_script);
    return ret;
#else
    return FALSE;
#endif
}

static gboolean
service_enable (const gchar *service,
                GError **error)
{
#if HAVE_OPENRC
    gchar *runlevel = NULL;
    gchar *service_script = NULL;
    const gchar *argv[3] = { NULL, "start", NULL };
    gboolean ret = FALSE;
    gint exit_status = 0;

    g_assert (service != NULL);

    if (!rc_service_exists (service)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s rc service not found", service);
        goto out;
    }

    runlevel = rc_runlevel_get();
    if (!rc_service_in_runlevel (service, runlevel)) {
        g_debug ("Adding %s rc service to %s runlevel", service, runlevel);
        if (!rc_service_add (runlevel, service))
            g_warning ("Failed to add %s rc service to %s runlevel", service, runlevel);
    }

    if ((service_script = rc_service_resolve (service)) == NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s rc service does not resolve", service);
        goto out;
    }

    g_debug ("Starting %s rc service", service);
    argv[0] = service_script;
    if (!g_spawn_sync (NULL, (gchar **)argv, NULL, 0, NULL, NULL, NULL, NULL, &exit_status, error)) {
        g_prefix_error (error, "Failed to spawn %s rc service:", service);
        goto out;
    }
    if (exit_status) {
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "%s rc service failed to start with exit status %d", service, exit_status);
        goto out;
    }
    ret = TRUE;

  out:
    if (runlevel != NULL)
        free (runlevel);
    if (service_script != NULL)
        free (service_script);
    return ret;
#else
    return FALSE;
#endif
}

struct invoked_set_time {
    GDBusMethodInvocation *invocation;
    gint64 usec_utc;
    gboolean relative;
};

static void
on_handle_set_time_authorized_cb (GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data)
{
    GError *err = NULL;
    struct invoked_set_time *data;
    struct timespec ts = { 0, 0 };
    struct tm *tm = NULL;

    data = (struct invoked_set_time *) user_data;
    if (!check_polkit_finish (res, &err)) {
        g_dbus_method_invocation_return_gerror (data->invocation, err);
        goto out;
    }

    G_LOCK (clock);
    if (!data->relative && data->usec_utc < 0) {
        g_dbus_method_invocation_return_dbus_error (data->invocation, DBUS_ERROR_INVALID_ARGS, "Attempt to set time before epoch");
        goto unlock;
    }

    if (data->relative)
        if (clock_gettime (CLOCK_REALTIME, &ts)) {
            int errsv = errno;
            g_dbus_method_invocation_return_dbus_error (data->invocation, DBUS_ERROR_FAILED, strerror (errsv));
            goto unlock;
        }
    ts.tv_sec += data->usec_utc / 1000000;
    ts.tv_nsec += (data->usec_utc % 1000000) * 1000;
    if (clock_settime (CLOCK_REALTIME, &ts)) {
        int errsv = errno;
        g_dbus_method_invocation_return_dbus_error (data->invocation, DBUS_ERROR_FAILED, strerror (errsv));
        goto unlock;
    }

    if (local_rtc)
        tm = localtime(&ts.tv_sec);
    else
        tm = gmtime(&ts.tv_sec);
    hwclock_set_time(tm);

    timedated_timedate1_complete_set_time (timedate1, data->invocation);

  unlock:
    G_UNLOCK (clock);

  out:
    g_free (data);
    if (err != NULL)
        g_error_free (err);
}

static gboolean
on_handle_set_time (TimedatedTimedate1 *timedate1,
                    GDBusMethodInvocation *invocation,
                    const gint64 usec_utc,
                    const gboolean relative,
                    const gboolean user_interaction,
                    gpointer user_data)
{
    if (read_only)
        g_dbus_method_invocation_return_dbus_error (invocation,
                                                    DBUS_ERROR_NOT_SUPPORTED,
                                                    SERVICE_NAME " is in read-only mode");
    else {
        struct invoked_set_time *data;
        data = g_new0 (struct invoked_set_time, 1);
        data->invocation = invocation;
        data->usec_utc = usec_utc;
        data->relative = relative;
        check_polkit_async (g_dbus_method_invocation_get_sender (invocation), "org.freedesktop.timedate1.set-time", user_interaction, on_handle_set_time_authorized_cb, data);
    }

    return TRUE;
}

struct invoked_set_timezone {
    GDBusMethodInvocation *invocation;
    gchar *timezone; /* newly allocated */
};

static void
on_handle_set_timezone_authorized_cb (GObject *source_object,
                                      GAsyncResult *res,
                                      gpointer user_data)
{
    GError *err = NULL;
    struct invoked_set_timezone *data;

    data = (struct invoked_set_timezone *) user_data;
    if (!check_polkit_finish (res, &err)) {
        g_dbus_method_invocation_return_gerror (data->invocation, err);
        goto out;
    }

    G_LOCK (clock);
    if (!set_timezone(data->timezone, &err)) {
        g_dbus_method_invocation_return_gerror (data->invocation, err);
        goto unlock;
    }

    if (local_rtc) {
        struct timespec ts;
        struct tm *tm;

        /* Update kernel's view of the rtc timezone */
        hwclock_apply_localtime_delta (NULL);
        clock_gettime (CLOCK_REALTIME, &ts);
        tm = localtime (&ts.tv_sec);
        hwclock_set_time (tm);
    }

    timedated_timedate1_complete_set_timezone (timedate1, data->invocation);
    g_free (timezone_name);
    timezone_name = data->timezone;
    timedated_timedate1_set_timezone (timedate1, timezone_name);

  unlock:
    G_UNLOCK (clock);

  out:
    g_free (data);
    if (err != NULL)
        g_error_free (err);
}

static gboolean
on_handle_set_timezone (TimedatedTimedate1 *timedate1,
                        GDBusMethodInvocation *invocation,
                        const gchar *timezone,
                        const gboolean user_interaction,
                        gpointer user_data)
{
    if (read_only)
        g_dbus_method_invocation_return_dbus_error (invocation,
                                                    DBUS_ERROR_NOT_SUPPORTED,
                                                    SERVICE_NAME " is in read-only mode");
    else {
        struct invoked_set_timezone *data;
        data = g_new0 (struct invoked_set_timezone, 1);
        data->invocation = invocation;
        data->timezone = g_strdup (timezone);
        check_polkit_async (g_dbus_method_invocation_get_sender (invocation), "org.freedesktop.timedate1.set-timezone", user_interaction, on_handle_set_timezone_authorized_cb, data);
    }

    return TRUE;
}

struct invoked_set_local_rtc {
    GDBusMethodInvocation *invocation;
    gboolean local_rtc;
    gboolean fix_system;
};

static void
on_handle_set_local_rtc_authorized_cb (GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
    GError *err = NULL;
    struct invoked_set_local_rtc *data;
    gchar *clock = NULL;
    const gchar *clock_types[2] = { "UTC", "local" };

    data = (struct invoked_set_local_rtc *) user_data;
    if (!check_polkit_finish (res, &err)) {
        g_dbus_method_invocation_return_gerror (data->invocation, err);
        goto out;
    }

    G_LOCK (clock);
    clock = shell_source_var (hwclock_file, "${clock}", NULL);
    if (clock != NULL || data->local_rtc)
        if (!shell_parser_set_and_save (hwclock_file, &err, "clock", NULL, clock_types[data->local_rtc], NULL)) {
            g_dbus_method_invocation_return_gerror (data->invocation, err);
            goto unlock;
        }

    if (data->local_rtc != local_rtc) {
        /* The clock sync code below taken almost verbatim from systemd's timedated.c, and is
         * copyright 2011 Lennart Poettering */
        struct timespec ts;

        /* Update kernel's view of the rtc timezone */
        if (data->local_rtc)
            hwclock_apply_localtime_delta (NULL);
        else
            hwclock_reset_localtime_delta ();

        clock_gettime (CLOCK_REALTIME, &ts);
        if (data->fix_system) {
            struct tm tm;

            /* Sync system clock from RTC; first,
             * initialize the timezone fields of
             * struct tm. */
            if (data->local_rtc)
                tm = *localtime(&ts.tv_sec);
            else
                tm = *gmtime(&ts.tv_sec);

            /* Override the main fields of
             * struct tm, but not the timezone
             * fields */
            if (hwclock_get_time(&tm) >= 0) {
                /* And set the system clock
                 * with this */
                if (data->local_rtc)
                    ts.tv_sec = mktime(&tm);
                else
                    ts.tv_sec = timegm(&tm);

                clock_settime(CLOCK_REALTIME, &ts);
            }

        } else {
            struct tm *tm;

            /* Sync RTC from system clock */
            if (data->local_rtc)
                tm = localtime(&ts.tv_sec);
            else
                tm = gmtime(&ts.tv_sec);

            hwclock_set_time(tm);
        }
    }

    timedated_timedate1_complete_set_timezone (timedate1, data->invocation);
    local_rtc = data->local_rtc;
    timedated_timedate1_set_local_rtc (timedate1, local_rtc);

  unlock:
    G_UNLOCK (clock);

  out:
    g_free (clock);
    g_free (data);
    if (err != NULL)
        g_error_free (err);
}

static gboolean
on_handle_set_local_rtc (TimedatedTimedate1 *timedate1,
                         GDBusMethodInvocation *invocation,
                         const gboolean _local_rtc,
                         const gboolean fix_system,
                         const gboolean user_interaction,
                         gpointer user_data)
{
    if (read_only)
        g_dbus_method_invocation_return_dbus_error (invocation,
                                                    DBUS_ERROR_NOT_SUPPORTED,
                                                    SERVICE_NAME " is in read-only mode");
    else {
        struct invoked_set_local_rtc *data;
        data = g_new0 (struct invoked_set_local_rtc, 1);
        data->invocation = invocation;
        data->local_rtc = _local_rtc;
        data->fix_system = fix_system;
        check_polkit_async (g_dbus_method_invocation_get_sender (invocation), "org.freedesktop.timedate1.set-local-rtc", user_interaction, on_handle_set_local_rtc_authorized_cb, data);
    }

    return TRUE;
}

struct invoked_set_ntp {
    GDBusMethodInvocation *invocation;
    gboolean use_ntp;
};

static void
on_handle_set_ntp_authorized_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
    GError *err = NULL;
    struct invoked_set_ntp *data;

    data = (struct invoked_set_ntp *) user_data;
    if (!check_polkit_finish (res, &err)) {
        g_dbus_method_invocation_return_gerror (data->invocation, err);
        goto out;
    }

    G_LOCK (ntp);
    if (ntp_service () == NULL) {
        g_dbus_method_invocation_return_dbus_error (data->invocation, DBUS_ERROR_FAILED,
                                                    "No ntp implementation found. Please install one of the following packages: "
                                                    NTP_DEFAULT_SERVICES_PACKAGES);
        goto unlock;
    }
    if ((data->use_ntp && !service_enable (ntp_service (), &err)) ||
        (!data->use_ntp && !service_disable (ntp_service (), &err)))
    {
        g_dbus_method_invocation_return_gerror (data->invocation, err);
        goto unlock;
    }

    timedated_timedate1_complete_set_ntp (timedate1, data->invocation);
    use_ntp = data->use_ntp;
    timedated_timedate1_set_ntp (timedate1, use_ntp);

  unlock:
    G_UNLOCK (ntp);

  out:
    g_free (data);
    if (err != NULL)
        g_error_free (err);
}

static gboolean
on_handle_set_ntp (TimedatedTimedate1 *timedate1,
                   GDBusMethodInvocation *invocation,
                   const gboolean _use_ntp,
                   const gboolean user_interaction,
                   gpointer user_data)
{
    if (read_only)
        g_dbus_method_invocation_return_dbus_error (invocation,
                                                    DBUS_ERROR_NOT_SUPPORTED,
                                                    SERVICE_NAME " is in read-only mode");
    else {
        struct invoked_set_ntp *data;
        data = g_new0 (struct invoked_set_ntp, 1);
        data->invocation = invocation;
        data->use_ntp = _use_ntp;
        check_polkit_async (g_dbus_method_invocation_get_sender (invocation), "org.freedesktop.timedate1.set-ntp", user_interaction, on_handle_set_ntp_authorized_cb, data);
    }

    return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *bus_name,
                 gpointer         user_data)
{
    gchar *name;
    GError *err = NULL;

    g_debug ("Acquired a message bus connection");

    timedate1 = timedated_timedate1_skeleton_new ();

    timedated_timedate1_set_timezone (timedate1, timezone_name);
    timedated_timedate1_set_local_rtc (timedate1, local_rtc);
    timedated_timedate1_set_ntp (timedate1, use_ntp);

    g_signal_connect (timedate1, "handle-set-time", G_CALLBACK (on_handle_set_time), NULL);
    g_signal_connect (timedate1, "handle-set-timezone", G_CALLBACK (on_handle_set_timezone), NULL);
    g_signal_connect (timedate1, "handle-set-local-rtc", G_CALLBACK (on_handle_set_local_rtc), NULL);
    g_signal_connect (timedate1, "handle-set-ntp", G_CALLBACK (on_handle_set_ntp), NULL);

    if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (timedate1),
                                           connection,
                                           "/org/freedesktop/timedate1",
                                           &err)) {
        if (err != NULL) {
            g_critical ("Failed to export interface on /org/freedesktop/timedate1: %s", err->message);
            exit (1);
        }
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *bus_name,
                  gpointer         user_data)
{
    g_debug ("Acquired the name %s", bus_name);
    component_started ();
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *bus_name,
              gpointer         user_data)
{
    if (connection == NULL)
        g_critical ("Failed to acquire a dbus connection");
    else
        g_critical ("Failed to acquire dbus name %s", bus_name);
    exit (1);
}

void
timedated_init (gboolean _read_only,
                const gchar *_ntp_preferred_service)
{
    GError *err = NULL;

    read_only = _read_only;
    ntp_preferred_service = _ntp_preferred_service;

    hwclock_file = g_file_new_for_path (SYSCONFDIR "/conf.d/hwclock");
    timezone_file = g_file_new_for_path (SYSCONFDIR "/timezone");
    localtime_file = g_file_new_for_path (SYSCONFDIR "/localtime");

    local_rtc = get_local_rtc (&err);
    if (err != NULL) {
        g_debug ("%s", err->message);
        g_clear_error (&err);
    }
    timezone_name = get_timezone_name (&err);
    if (err != NULL) {
        g_warning ("%s", err->message);
        g_clear_error (&err);
    }
    if (ntp_service () == NULL) {
        g_warning ("No ntp implementation found. Please install one of the following packages: " NTP_DEFAULT_SERVICES_PACKAGES);
        use_ntp = FALSE;
    } else {
        use_ntp = service_started (ntp_service (), &err);
        if (err != NULL) {
            g_warning ("%s", err->message);
            g_clear_error (&err);
        }
    }

    bus_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                             "org.freedesktop.timedate1",
                             G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);
}

void
timedated_destroy (void)
{
    g_bus_unown_name (bus_id);
    bus_id = 0;
    read_only = FALSE;
    ntp_preferred_service = NULL;

    g_object_unref (hwclock_file);
    g_object_unref (timezone_file);
    g_object_unref (localtime_file);
}
