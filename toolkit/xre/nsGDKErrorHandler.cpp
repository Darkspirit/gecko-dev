/* -*- Mode: C++; tab-width: 40; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsGDKErrorHandler.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nsDebug.h"
#include "nsString.h"
#include "prenv.h"

/* See https://bugzilla.gnome.org/show_bug.cgi?id=629608#c8
 *
 * GDK implements X11 error traps to ignore X11 errors.
 * Unfortunatelly We don't know which X11 events can be ignored
 * so we have to utilize the Gdk error handler to avoid
 * false alarms in Gtk3.
 */
static void GdkErrorHandler(const gchar* log_domain, GLogLevelFlags log_level,
                            const gchar* message, gpointer user_data) {
  {
    g_log_default_handler(log_domain, log_level, message, user_data);
    MOZ_CRASH_UNSAFE(message);
  }
}

void InstallGdkErrorHandler() {
  g_log_set_handler("Gdk",
                    (GLogLevelFlags)(G_LOG_LEVEL_ERROR | G_LOG_FLAG_FATAL |
                                     G_LOG_FLAG_RECURSION),
                    GdkErrorHandler, nullptr);
}
