/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define PANGO_ENABLE_BACKEND
#define PANGO_ENABLE_ENGINE

#include "gfxPlatformGtk.h"

#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>

#include "base/task.h"
#include "base/thread.h"
#include "base/message_loop.h"
#include "cairo.h"
#include "gfx2DGlue.h"
#include "gfxFcPlatformFontList.h"
#include "gfxConfig.h"
#include "gfxContext.h"
#include "gfxImageSurface.h"
#include "gfxUserFontSet.h"
#include "gfxUtils.h"
#include "gfxFT2FontBase.h"
#include "gfxTextRun.h"
#include "GLContextProvider.h"
#include "mozilla/Atomics.h"
#include "mozilla/Components.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/Monitor.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_layers.h"
#include "mozilla/StaticPrefs_media.h"
#include "nsAppRunner.h"
#include "nsIGfxInfo.h"
#include "nsMathUtils.h"
#include "nsUnicharUtils.h"
#include "nsUnicodeProperties.h"
#include "prenv.h"
#include "VsyncSource.h"
#include "mozilla/WidgetUtilsGtk.h"

#ifdef MOZ_X11
#  include "mozilla/gfx/XlibDisplay.h"
#  include <gdk/gdkx.h>
#  include <X11/extensions/Xrandr.h>
#  include "cairo-xlib.h"
#  include "gfxXlibSurface.h"
#  include "mozilla/X11Util.h"
#  include "SoftwareVsyncSource.h"

/* Undefine the Status from Xlib since it will conflict with system headers on
 * OSX */
#  if defined(__APPLE__) && defined(Status)
#    undef Status
#  endif
#endif /* MOZ_X11 */

#ifdef MOZ_WAYLAND
#  include <gdk/gdkwayland.h>
#  include "mozilla/widget/nsWaylandDisplay.h"
#endif
#ifdef MOZ_WIDGET_GTK
#  include "mozilla/widget/DMABufLibWrapper.h"
#  include "mozilla/StaticPrefs_widget.h"
#endif

#define GDK_PIXMAP_SIZE_MAX 32767

#define GFX_PREF_MAX_GENERIC_SUBSTITUTIONS \
  "gfx.font_rendering.fontconfig.max_generic_substitutions"

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::unicode;
using namespace mozilla::widget;

static FT_Library gPlatformFTLibrary = nullptr;
static int32_t sDPI;

static void screen_resolution_changed(GdkScreen* aScreen, GParamSpec* aPspec,
                                      gpointer aClosure) {
  sDPI = 0;
}

gfxPlatformGtk::gfxPlatformGtk() {
  if (!gfxPlatform::IsHeadless()) {
    gtk_init(nullptr, nullptr);
  }

  mIsX11Display = gfxPlatform::IsHeadless() ? false : GdkIsX11Display();
  if (XRE_IsParentProcess()) {
    InitDmabufConfig();
    if (gfxConfig::IsEnabled(Feature::DMABUF)) {
      gfxVars::SetUseDMABuf(true);
    }
  }

  InitBackendPrefs(GetBackendPrefs());

  gPlatformFTLibrary = Factory::NewFTLibrary();
  MOZ_RELEASE_ASSERT(gPlatformFTLibrary);
  Factory::SetFTLibrary(gPlatformFTLibrary);

  GdkScreen* gdkScreen = gdk_screen_get_default();
  if (gdkScreen) {
    g_signal_connect(gdkScreen, "notify::resolution",
                     G_CALLBACK(screen_resolution_changed), nullptr);
  }
}

gfxPlatformGtk::~gfxPlatformGtk() {
  Factory::ReleaseFTLibrary(gPlatformFTLibrary);
  gPlatformFTLibrary = nullptr;
}

void gfxPlatformGtk::InitDmabufConfig() {
  FeatureState& feature = gfxConfig::GetFeature(Feature::DMABUF);
  feature.EnableByDefault();

  if (StaticPrefs::widget_dmabuf_force_enabled_AtStartup()) {
    feature.UserForceEnable("Force enabled by pref");
  }

  nsCString failureId;
  int32_t status;
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  if (NS_FAILED(gfxInfo->GetFeatureStatus(nsIGfxInfo::FEATURE_DMABUF, failureId,
                                          &status))) {
    feature.Disable(FeatureStatus::BlockedNoGfxInfo, "gfxInfo is broken",
                    "FEATURE_FAILURE_NO_GFX_INFO"_ns);
  } else if (status != nsIGfxInfo::FEATURE_STATUS_OK) {
    feature.Disable(FeatureStatus::Blocklisted, "Blocklisted by gfxInfo",
                    failureId);
  }
  if (feature.IsEnabled()) {
    nsAutoCString drmRenderDevice;
    gfxInfo->GetDrmRenderDevice(drmRenderDevice);
    gfxVars::SetDrmRenderDevice(drmRenderDevice);

    if (!GetDMABufDevice()->IsEnabled(failureId)) {
      feature.ForceDisable(FeatureStatus::Failed, "Failed to configure",
                           failureId);
    }
  }
}

bool gfxPlatformGtk::InitVAAPIConfig(bool aForceEnabledByUser) {
  FeatureState& feature =
      gfxConfig::GetFeature(Feature::HARDWARE_VIDEO_DECODING);
  // We're already configured in parent process
  if (!XRE_IsParentProcess()) {
    return feature.IsEnabled();
  }
  feature.EnableByDefault();

  int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  nsCString failureId;
  if (NS_FAILED(gfxInfo->GetFeatureStatus(
          nsIGfxInfo::FEATURE_HARDWARE_VIDEO_DECODING, failureId, &status))) {
    feature.Disable(FeatureStatus::BlockedNoGfxInfo, "gfxInfo is broken",
                    "FEATURE_FAILURE_NO_GFX_INFO"_ns);
  } else if (status == nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST) {
    feature.ForceDisable(FeatureStatus::Unavailable,
                         "Force disabled by gfxInfo", failureId);
  } else if (status != nsIGfxInfo::FEATURE_STATUS_OK) {
    feature.Disable(FeatureStatus::Blocklisted, "Blocklisted by gfxInfo",
                    failureId);
  }
  if (aForceEnabledByUser) {
    feature.UserForceEnable("Force enabled by pref");
  }

  // Configure zero-copy playback feature.
  if (feature.IsEnabled()) {
    FeatureState& featureZeroCopy =
        gfxConfig::GetFeature(Feature::HW_DECODED_VIDEO_ZERO_COPY);

    featureZeroCopy.EnableByDefault();
    uint32_t state =
        StaticPrefs::media_ffmpeg_vaapi_force_surface_zero_copy_AtStartup();
    if (state == 0) {
      featureZeroCopy.UserDisable("Force disable by pref",
                                  "FEATURE_FAILURE_USER_FORCE_DISABLED"_ns);
    } else if (state == 1) {
      featureZeroCopy.UserEnable("Force enabled by pref");
    } else {
      nsCString failureId;
      int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
      nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
      if (NS_FAILED(gfxInfo->GetFeatureStatus(
              nsIGfxInfo::FEATURE_HW_DECODED_VIDEO_ZERO_COPY, failureId,
              &status))) {
        featureZeroCopy.Disable(FeatureStatus::BlockedNoGfxInfo,
                                "gfxInfo is broken",
                                "FEATURE_FAILURE_NO_GFX_INFO"_ns);
      } else if (status == nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST) {
        featureZeroCopy.ForceDisable(FeatureStatus::Unavailable,
                                     "Force disabled by gfxInfo", failureId);
      } else if (status != nsIGfxInfo::FEATURE_ALLOW_ALWAYS) {
        featureZeroCopy.Disable(FeatureStatus::Blocklisted,
                                "Blocklisted by gfxInfo", failureId);
      }
    }
    if (featureZeroCopy.IsEnabled()) {
      gfxVars::SetHwDecodedVideoZeroCopy(true);
    }
  }
  return feature.IsEnabled();
}

void gfxPlatformGtk::InitWebRenderConfig() {
  gfxPlatform::InitWebRenderConfig();

  if (!XRE_IsParentProcess()) {
    return;
  }

  FeatureState& feature = gfxConfig::GetFeature(Feature::WEBRENDER_COMPOSITOR);
#ifdef RELEASE_OR_BETA
  feature.ForceDisable(FeatureStatus::Blocked,
                       "Cannot be enabled in release or beta",
                       "FEATURE_FAILURE_DISABLE_RELEASE_OR_BETA"_ns);
#else
  if (feature.IsEnabled()) {
    if (!IsWaylandDisplay()) {
      feature.ForceDisable(FeatureStatus::Unavailable,
                           "Wayland support missing",
                           "FEATURE_FAILURE_NO_WAYLAND"_ns);
    }
#  ifdef MOZ_WAYLAND
    else if (gfxConfig::IsEnabled(Feature::WEBRENDER) &&
             !gfxConfig::IsEnabled(Feature::DMABUF)) {
      // We use zwp_linux_dmabuf_v1 and GBM directly to manage FBOs. In theory
      // this is also possible vie EGLstreams, but we don't bother to implement
      // it as recent NVidia drivers support GBM and DMABuf as well.
      feature.ForceDisable(FeatureStatus::Unavailable,
                           "Hardware Webrender requires DMAbuf support",
                           "FEATURE_FAILURE_NO_DMABUF"_ns);
    } else if (!widget::WaylandDisplayGet()->GetViewporter()) {
      feature.ForceDisable(FeatureStatus::Unavailable,
                           "Requires wp_viewporter protocol support",
                           "FEATURE_FAILURE_REQUIRES_WPVIEWPORTER"_ns);
    }
#  endif  // MOZ_WAYLAND
  }
#endif    // RELEASE_OR_BETA

  gfxVars::SetUseWebRenderCompositor(feature.IsEnabled());
}

void gfxPlatformGtk::InitPlatformGPUProcessPrefs() {
#ifdef MOZ_WAYLAND
  if (IsWaylandDisplay()) {
    FeatureState& gpuProc = gfxConfig::GetFeature(Feature::GPU_PROCESS);
    gpuProc.ForceDisable(FeatureStatus::Blocked,
                         "Wayland does not work in the GPU process",
                         "FEATURE_FAILURE_WAYLAND"_ns);
  }
#endif
}

already_AddRefed<gfxASurface> gfxPlatformGtk::CreateOffscreenSurface(
    const IntSize& aSize, gfxImageFormat aFormat) {
  if (!Factory::AllowedSurfaceSize(aSize)) {
    return nullptr;
  }

  RefPtr<gfxASurface> newSurface;
  bool needsClear = true;
  // XXX we really need a different interface here, something that passes
  // in more context, including the display and/or target surface type that
  // we should try to match
  GdkScreen* gdkScreen = gdk_screen_get_default();
  if (gdkScreen) {
    newSurface = new gfxImageSurface(aSize, aFormat);
    // The gfxImageSurface ctor zeroes this for us, no need to
    // waste time clearing again
    needsClear = false;
  }

  if (!newSurface) {
    // We couldn't create a native surface for whatever reason;
    // e.g., no display, no RENDER, bad size, etc.
    // Fall back to image surface for the data.
    newSurface = new gfxImageSurface(aSize, aFormat);
  }

  if (newSurface->CairoStatus()) {
    newSurface = nullptr;  // surface isn't valid for some reason
  }

  if (newSurface && needsClear) {
    gfxUtils::ClearThebesSurface(newSurface);
  }

  return newSurface.forget();
}

nsresult gfxPlatformGtk::GetFontList(nsAtom* aLangGroup,
                                     const nsACString& aGenericFamily,
                                     nsTArray<nsString>& aListOfFonts) {
  gfxPlatformFontList::PlatformFontList()->GetFontList(
      aLangGroup, aGenericFamily, aListOfFonts);
  return NS_OK;
}

// xxx - this is ubuntu centric, need to go through other distros and flesh
// out a more general list
static const char kFontDejaVuSans[] = "DejaVu Sans";
static const char kFontDejaVuSerif[] = "DejaVu Serif";
static const char kFontFreeSans[] = "FreeSans";
static const char kFontFreeSerif[] = "FreeSerif";
static const char kFontTakaoPGothic[] = "TakaoPGothic";
static const char kFontTwemojiMozilla[] = "Twemoji Mozilla";
static const char kFontDroidSansFallback[] = "Droid Sans Fallback";
static const char kFontWenQuanYiMicroHei[] = "WenQuanYi Micro Hei";
static const char kFontNanumGothic[] = "NanumGothic";
static const char kFontSymbola[] = "Symbola";
static const char kFontNotoSansSymbols[] = "Noto Sans Symbols";
static const char kFontNotoSansSymbols2[] = "Noto Sans Symbols2";

void gfxPlatformGtk::GetCommonFallbackFonts(uint32_t aCh, Script aRunScript,
                                            eFontPresentation aPresentation,
                                            nsTArray<const char*>& aFontList) {
  if (PrefersColor(aPresentation)) {
    aFontList.AppendElement(kFontTwemojiMozilla);
  }

  aFontList.AppendElement(kFontDejaVuSerif);
  aFontList.AppendElement(kFontFreeSerif);
  aFontList.AppendElement(kFontDejaVuSans);
  aFontList.AppendElement(kFontFreeSans);
  aFontList.AppendElement(kFontSymbola);
  aFontList.AppendElement(kFontNotoSansSymbols);
  aFontList.AppendElement(kFontNotoSansSymbols2);

  // add fonts for CJK ranges
  // xxx - this isn't really correct, should use the same CJK font ordering
  // as the pref font code
  if (aCh >= 0x3000 && ((aCh < 0xe000) || (aCh >= 0xf900 && aCh < 0xfff0) ||
                        ((aCh >> 16) == 2))) {
    aFontList.AppendElement(kFontTakaoPGothic);
    aFontList.AppendElement(kFontDroidSansFallback);
    aFontList.AppendElement(kFontWenQuanYiMicroHei);
    aFontList.AppendElement(kFontNanumGothic);
  }
}

void gfxPlatformGtk::ReadSystemFontList(
    mozilla::dom::SystemFontList* retValue) {
  gfxFcPlatformFontList::PlatformFontList()->ReadSystemFontList(retValue);
}

bool gfxPlatformGtk::CreatePlatformFontList() {
  return gfxPlatformFontList::Initialize(new gfxFcPlatformFontList);
}

int32_t gfxPlatformGtk::GetFontScaleDPI() {
  MOZ_ASSERT(XRE_IsParentProcess(),
             "You can access this via LookAndFeel if you need it in child "
             "processes");
  if (MOZ_LIKELY(sDPI != 0)) {
    return sDPI;
  }
  GdkScreen* screen = gdk_screen_get_default();
  // Ensure settings in config files are processed.
  gtk_settings_get_for_screen(screen);
  int32_t dpi = int32_t(round(gdk_screen_get_resolution(screen)));
  if (dpi <= 0) {
    // Fall back to something reasonable
    dpi = 96;
  }
  sDPI = dpi;
  return dpi;
}

double gfxPlatformGtk::GetFontScaleFactor() {
  // Integer scale factors work well with GTK window scaling, image scaling, and
  // pixel alignment, but there is a range where 1 is too small and 2 is too
  // big.
  //
  // An additional step of 1.5 is added because this is common scale on WINNT
  // and at this ratio the advantages of larger rendering outweigh the
  // disadvantages from scaling and pixel mis-alignment.
  //
  // A similar step for 1.25 is added as well, because this is the scale that
  // "Large text" settings use in gnome, and it seems worth to allow, especially
  // on already-hidpi environments.
  int32_t dpi = GetFontScaleDPI();
  if (dpi < 120) {
    return 1.0;
  }
  if (dpi < 132) {
    return 1.25;
  }
  if (dpi < 168) {
    return 1.5;
  }
  return round(dpi / 96.0);
}

gfxImageFormat gfxPlatformGtk::GetOffscreenFormat() {
  // Make sure there is a screen
  GdkScreen* screen = gdk_screen_get_default();
  if (screen && gdk_visual_get_depth(gdk_visual_get_system()) == 16) {
    return SurfaceFormat::R5G6B5_UINT16;
  }

  return SurfaceFormat::X8R8G8B8_UINT32;
}

void gfxPlatformGtk::FontsPrefsChanged(const char* aPref) {
  // only checking for generic substitions, pass other changes up
  if (strcmp(GFX_PREF_MAX_GENERIC_SUBSTITUTIONS, aPref) != 0) {
    gfxPlatform::FontsPrefsChanged(aPref);
    return;
  }

  gfxFcPlatformFontList* pfl = gfxFcPlatformFontList::PlatformFontList();
  pfl->ClearGenericMappings();
  FlushFontAndWordCaches();
}

bool gfxPlatformGtk::AccelerateLayersByDefault() { return true; }

#if defined(MOZ_X11)

static nsTArray<uint8_t> GetDisplayICCProfile(Display* dpy, Window& root) {
  const char kIccProfileAtomName[] = "_ICC_PROFILE";
  Atom iccAtom = XInternAtom(dpy, kIccProfileAtomName, TRUE);
  if (!iccAtom) {
    return nsTArray<uint8_t>();
  }

  Atom retAtom;
  int retFormat;
  unsigned long retLength, retAfter;
  unsigned char* retProperty;

  if (XGetWindowProperty(dpy, root, iccAtom, 0, INT_MAX /* length */, X11False,
                         AnyPropertyType, &retAtom, &retFormat, &retLength,
                         &retAfter, &retProperty) != Success) {
    return nsTArray<uint8_t>();
  }

  nsTArray<uint8_t> result;

  if (retLength > 0) {
    result.AppendElements(static_cast<uint8_t*>(retProperty), retLength);
  }

  XFree(retProperty);

  return result;
}

nsTArray<uint8_t> gfxPlatformGtk::GetPlatformCMSOutputProfileData() {
  nsTArray<uint8_t> prefProfileData = GetPrefCMSOutputProfileData();
  if (!prefProfileData.IsEmpty()) {
    return prefProfileData;
  }

  if (XRE_IsContentProcess()) {
    MOZ_ASSERT(NS_IsMainThread());
    // This will be passed in during InitChild so we can avoid sending a
    // sync message back to the parent during init.
    const mozilla::gfx::ContentDeviceData* contentDeviceData =
        GetInitContentDeviceData();
    if (contentDeviceData) {
      // On Windows, we assert that the profile isn't empty, but on
      // Linux it can legitimately be empty if the display isn't
      // calibrated.  Thus, no assertion here.
      return contentDeviceData->cmsOutputProfileData().Clone();
    }

    // Otherwise we need to ask the parent for the updated color profile
    mozilla::dom::ContentChild* cc = mozilla::dom::ContentChild::GetSingleton();
    nsTArray<uint8_t> result;
    Unused << cc->SendGetOutputColorProfileData(&result);
    return result;
  }

  if (!mIsX11Display) {
    return nsTArray<uint8_t>();
  }

  GdkDisplay* display = gdk_display_get_default();
  Display* dpy = GDK_DISPLAY_XDISPLAY(display);
  // In xpcshell tests, we never initialize X and hence don't have a Display.
  // In this case, there's no output colour management to be done, so we just
  // return with nullptr.
  if (!dpy) {
    return nsTArray<uint8_t>();
  }

  Window root = gdk_x11_get_default_root_xwindow();

  // First try ICC Profile
  nsTArray<uint8_t> iccResult = GetDisplayICCProfile(dpy, root);
  if (!iccResult.IsEmpty()) {
    return iccResult;
  }

  // If ICC doesn't work, then try EDID
  const char kEdid1AtomName[] = "XFree86_DDC_EDID1_RAWDATA";
  Atom edidAtom = XInternAtom(dpy, kEdid1AtomName, TRUE);
  if (!edidAtom) {
    return nsTArray<uint8_t>();
  }

  Atom retAtom;
  int retFormat;
  unsigned long retLength, retAfter;
  unsigned char* retProperty;

  if (XGetWindowProperty(dpy, root, edidAtom, 0, 32, X11False, AnyPropertyType,
                         &retAtom, &retFormat, &retLength, &retAfter,
                         &retProperty) != Success) {
    return nsTArray<uint8_t>();
  }

  if (retLength != 128) {
    return nsTArray<uint8_t>();
  }

  // Format documented in "VESA E-EDID Implementation Guide"
  float gamma = (100 + (float)retProperty[0x17]) / 100.0f;

  qcms_CIE_xyY whitePoint;
  whitePoint.x =
      ((retProperty[0x21] << 2) | (retProperty[0x1a] >> 2 & 3)) / 1024.0;
  whitePoint.y =
      ((retProperty[0x22] << 2) | (retProperty[0x1a] >> 0 & 3)) / 1024.0;
  whitePoint.Y = 1.0;

  qcms_CIE_xyYTRIPLE primaries;
  primaries.red.x =
      ((retProperty[0x1b] << 2) | (retProperty[0x19] >> 6 & 3)) / 1024.0;
  primaries.red.y =
      ((retProperty[0x1c] << 2) | (retProperty[0x19] >> 4 & 3)) / 1024.0;
  primaries.red.Y = 1.0;

  primaries.green.x =
      ((retProperty[0x1d] << 2) | (retProperty[0x19] >> 2 & 3)) / 1024.0;
  primaries.green.y =
      ((retProperty[0x1e] << 2) | (retProperty[0x19] >> 0 & 3)) / 1024.0;
  primaries.green.Y = 1.0;

  primaries.blue.x =
      ((retProperty[0x1f] << 2) | (retProperty[0x1a] >> 6 & 3)) / 1024.0;
  primaries.blue.y =
      ((retProperty[0x20] << 2) | (retProperty[0x1a] >> 4 & 3)) / 1024.0;
  primaries.blue.Y = 1.0;

  XFree(retProperty);

  void* mem = nullptr;
  size_t size = 0;
  qcms_data_create_rgb_with_gamma(whitePoint, primaries, gamma, &mem, &size);
  if (!mem) {
    return nsTArray<uint8_t>();
  }

  nsTArray<uint8_t> result;
  result.AppendElements(static_cast<uint8_t*>(mem), size);
  free(mem);

  // XXX: It seems like we get wrong colors when using this constructed profile:
  // See bug 1696819. For now just forget that we made it.
  return nsTArray<uint8_t>();
}

#else  // defined(MOZ_X11)

nsTArray<uint8_t> gfxPlatformGtk::GetPlatformCMSOutputProfileData() {
  return nsTArray<uint8_t>();
}

#endif

bool gfxPlatformGtk::CheckVariationFontSupport() {
  // Although there was some variation/multiple-master support in FreeType
  // in older versions, it seems too incomplete/unstable for us to use
  // until at least 2.7.1.
  FT_Int major, minor, patch;
  FT_Library_Version(Factory::GetFTLibrary(), &major, &minor, &patch);
  return major * 1000000 + minor * 1000 + patch >= 2007001;
}

#ifdef MOZ_X11
class XrandrSoftwareVsyncSource final
    : public mozilla::gfx::SoftwareVsyncSource {
 public:
  XrandrSoftwareVsyncSource() : SoftwareVsyncSource(ComputeVsyncRate()) {
    MOZ_ASSERT(NS_IsMainThread());

    GdkScreen* defaultScreen = gdk_screen_get_default();
    g_signal_connect(defaultScreen, "monitors-changed",
                     G_CALLBACK(monitors_changed), this);
  }

 private:
  // Request the current refresh rate via xrandr. It is hard to find the
  // "correct" one, thus choose the highest one, assuming this will usually
  // give the best user experience.
  static mozilla::TimeDuration ComputeVsyncRate() {
    struct _XDisplay* dpy = gdk_x11_get_default_xdisplay();

    // Use the default software refresh rate as lower bound. Allowing lower
    // rates makes a bunch of tests start to fail on CI. The main goal of this
    // VsyncSource is to support refresh rates greater than the default one.
    double highestRefreshRate = gfxPlatform::GetSoftwareVsyncRate();

    // When running on remote X11 the xrandr version may be stuck on an
    // ancient version. There are still setups using remote X11 out there, so
    // make sure we don't crash.
    int eventBase, errorBase, major, minor;
    if (XRRQueryExtension(dpy, &eventBase, &errorBase) &&
        XRRQueryVersion(dpy, &major, &minor) &&
        (major > 1 || (major == 1 && minor >= 3))) {
      Window root = gdk_x11_get_default_root_xwindow();
      XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);

      if (res) {
        // We can't use refresh rates far below the default one (60Hz) because
        // otherwise random CI tests start to fail. However, many users have
        // screens just below the default rate, e.g. 59.95Hz. So slightly
        // decrease the lower bound.
        highestRefreshRate -= 1.0;

        for (int i = 0; i < res->noutput; i++) {
          XRROutputInfo* outputInfo =
              XRRGetOutputInfo(dpy, res, res->outputs[i]);
          if (outputInfo) {
            if (outputInfo->crtc) {
              XRRCrtcInfo* crtcInfo =
                  XRRGetCrtcInfo(dpy, res, outputInfo->crtc);
              if (crtcInfo) {
                for (int j = 0; j < res->nmode; j++) {
                  if (res->modes[j].id == crtcInfo->mode) {
                    double refreshRate = mode_refresh(&res->modes[j]);
                    if (refreshRate > highestRefreshRate) {
                      highestRefreshRate = refreshRate;
                    }
                    break;
                  }
                }

                XRRFreeCrtcInfo(crtcInfo);
              }
            }

            XRRFreeOutputInfo(outputInfo);
          }
        }
      }
      XRRFreeScreenResources(res);
    }

    const double rate = 1000.0 / highestRefreshRate;
    return mozilla::TimeDuration::FromMilliseconds(rate);
  }

  static void monitors_changed(GdkScreen* aScreen, gpointer aClosure) {
    XrandrSoftwareVsyncSource* self =
        static_cast<XrandrSoftwareVsyncSource*>(aClosure);
    self->SetVsyncRate(ComputeVsyncRate());
  }

  // from xrandr.c
  static double mode_refresh(const XRRModeInfo* mode_info) {
    double rate;
    double vTotal = mode_info->vTotal;

    if (mode_info->modeFlags & RR_DoubleScan) {
      /* doublescan doubles the number of lines */
      vTotal *= 2;
    }

    if (mode_info->modeFlags & RR_Interlace) {
      /* interlace splits the frame into two fields */
      /* the field rate is what is typically reported by monitors */
      vTotal /= 2;
    }

    if (mode_info->hTotal && vTotal) {
      rate = ((double)mode_info->dotClock /
              ((double)mode_info->hTotal * (double)vTotal));
    } else {
      rate = 0;
    }
    return rate;
  }
};
#endif

already_AddRefed<gfx::VsyncSource>
gfxPlatformGtk::CreateGlobalHardwareVsyncSource() {
#ifdef MOZ_X11
  if (IsHeadless() || IsWaylandDisplay()) {
    // On Wayland we can not create a global hardware based vsync source, thus
    // use a software based one here. We create window specific ones later.
    return GetSoftwareVsyncSource();
  }
  RefPtr<VsyncSource> softwareVsync = new XrandrSoftwareVsyncSource();
  return softwareVsync.forget();
#else
  return GetSoftwareVsyncSource();
#endif
}

void gfxPlatformGtk::BuildContentDeviceData(ContentDeviceData* aOut) {
  gfxPlatform::BuildContentDeviceData(aOut);

  aOut->cmsOutputProfileData() = GetPlatformCMSOutputProfileData();
}
