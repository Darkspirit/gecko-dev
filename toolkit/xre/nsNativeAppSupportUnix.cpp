/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNativeAppSupportBase.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsXPCOM.h"
#include "nsISupportsPrimitives.h"
#include "nsIObserverService.h"
#include "nsIAppStartup.h"
#include "nsServiceManagerUtils.h"
#include "prlink.h"
#include "nsXREDirProvider.h"
#include "nsReadableUtils.h"

#include "nsIFile.h"
#include "nsDirectoryServiceDefs.h"
#include "nsPIDOMWindow.h"
#include "nsIWidget.h"
#include "mozilla/Logging.h"
#include "mozilla/Services.h"
#include "mozilla/XREAppData.h"

#include <stdlib.h>
#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#ifdef MOZ_ENABLE_DBUS
#  include <dbus/dbus.h>
#endif

#define MIN_GTK_MAJOR_VERSION 2
#define MIN_GTK_MINOR_VERSION 10
#define UNSUPPORTED_GTK_MSG \
  "We're sorry, this application requires a version of the GTK+ library that is not installed on your computer.\n\n\
You have GTK+ %d.%d.\nThis application requires GTK+ %d.%d or newer.\n\n\
Please upgrade your GTK+ library if you wish to use this application."

class nsNativeAppSupportUnix : public nsNativeAppSupportBase {
 public:
  NS_IMETHOD Start(bool* aRetVal) override;
  NS_IMETHOD Enable() override;
};

static void RemoveArg(char** argv) {
  do {
    *argv = *(argv + 1);
    ++argv;
  } while (*argv);

  --gArgc;
}

NS_IMETHODIMP
nsNativeAppSupportUnix::Start(bool* aRetVal) {
  NS_ASSERTION(gAppData, "gAppData must not be null.");

// The dbus library is used by both nsWifiScannerDBus and BluetoothDBusService,
// from diffrent threads. This could lead to race conditions if the dbus is not
// initialized before making any other library calls.
#ifdef MOZ_ENABLE_DBUS
  dbus_threads_init_default();
#endif

  *aRetVal = true;
  return NS_OK;
}

NS_IMETHODIMP
nsNativeAppSupportUnix::Enable() { return NS_OK; }

nsresult NS_CreateNativeAppSupport(nsINativeAppSupport** aResult) {
  nsNativeAppSupportBase* native = new nsNativeAppSupportUnix();
  if (!native) return NS_ERROR_OUT_OF_MEMORY;

  *aResult = native;
  NS_ADDREF(*aResult);

  return NS_OK;
}
