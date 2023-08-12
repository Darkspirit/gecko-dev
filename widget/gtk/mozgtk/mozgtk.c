/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Types.h"

#include <gdk/gdk.h>

// Dummy call to gtk3 library to prevent the linker from removing
// the gtk3 dependency with --as-needed.
// see toolkit/library/moz.build for details.
MOZ_EXPORT void mozgtk_linker_holder() { gdk_display_get_default(); }
