/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/unix/AppUtilUnix.h"

#include "base/Log.h"

// X11LayoutsParser is needed both for X11 layout detection AND for
// converting XKB layout names to ISO codes in the GSettings fallback.
#include "deskflow/unix/X11LayoutsParser.h"

#if WINAPI_XWINDOWS
#include <X11/XKBlib.h>
#elif defined(Q_OS_MAC)
#include <Carbon/Carbon.h>
#include <platform/OSXAutoTypes.h>
#endif

#include <array>
#include <cstdio>
#include <filesystem>

AppUtilUnix::AppUtilUnix(const IEventQueue *)
{
  // do nothing
}

int startStatic()
{
  return AppUtil::instance().app().start();
}

int AppUtilUnix::run()
{
  return app().runInner(&startStatic);
}

void AppUtilUnix::startNode()
{
  app().startNode();
}

std::vector<std::string> AppUtilUnix::getKeyboardLayoutList()
{
  std::vector<std::string> layoutLangCodes;

#if WINAPI_XWINDOWS
  // Check /usr/local first used on bsd and some systems
  m_evdev = "/usr/local/share/X11/xkb/rules/evdev.xml";
  if (!std::filesystem::exists(m_evdev))
    m_evdev = "/usr/share/X11/xkb/rules/evdev.xml";
  layoutLangCodes = X11LayoutsParser::getX11LanguageList(m_evdev);

#elif defined(Q_OS_MAC)
  CFStringRef keys[] = {kTISPropertyInputSourceCategory};
  CFStringRef values[] = {kTISCategoryKeyboardInputSource};
  AutoCFDictionary dict(
      CFDictionaryCreate(nullptr, (const void **)keys, (const void **)values, 1, nullptr, nullptr), CFRelease
  );
  AutoCFArray kbds(TISCreateInputSourceList(dict.get(), false), CFRelease);

  for (CFIndex i = 0; i < CFArrayGetCount(kbds.get()); ++i) {
    TISInputSourceRef keyboardLayout = (TISInputSourceRef)CFArrayGetValueAtIndex(kbds.get(), i);
    auto layoutLanguages = (CFArrayRef)TISGetInputSourceProperty(keyboardLayout, kTISPropertyInputSourceLanguages);
    char temporaryCString[128] = {0};
    for (CFIndex index = 0; index < CFArrayGetCount(layoutLanguages) && layoutLanguages; index++) {
      auto languageCode = (CFStringRef)CFArrayGetValueAtIndex(layoutLanguages, index);
      if (!languageCode || !CFStringGetCString(languageCode, temporaryCString, 128, kCFStringEncodingUTF8)) {
        continue;
      }

      std::string langCode(temporaryCString);
      if (langCode.size() == 2 &&
          std::find(layoutLangCodes.begin(), layoutLangCodes.end(), langCode) == layoutLangCodes.end()) {
        layoutLangCodes.push_back(langCode);
      }

      // Save only first language code
      break;
    }
  }
#endif

  // Wayland fallback: X11 path may fail if no X server is available.
  // Try reading GNOME keyboard layout from GSettings/dconf.
  if (layoutLangCodes.empty()) {
    layoutLangCodes = getKeyboardLayoutListFromGSettings();
  }

  return layoutLangCodes;
}

std::vector<std::string> AppUtilUnix::getKeyboardLayoutListFromGSettings()
{
  std::vector<std::string> layoutLangCodes;

  // Try gsettings first, then fall back to dconf
  const char *commands[] = {
      "gsettings get org.gnome.desktop.input-sources sources 2>/dev/null",
      "dconf read /org/gnome/desktop/input-sources/sources 2>/dev/null",
  };

  std::string output;
  for (const auto *cmd : commands) {
    std::array<char, 256> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe)
      continue;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      output += buffer.data();
    }
    if (!output.empty())
      break;
  }

  if (output.empty()) {
    LOG_DEBUG("gsettings/dconf: no output, skipping GSettings layout detection");
    return layoutLangCodes;
  }

  // Parse Gnome-style input source list: [('xkb', 'cn'), ('ibus', 'libpinyin')]
  // or dconf-style: [('xkb', 'cn')]
  std::string xkbLayout;
  size_t pos = 0;
  while ((pos = output.find("'xkb'", pos)) != std::string::npos) {
    // Find the layout name after 'xkb'
    size_t layoutStart = output.find("'", pos + 5);
    if (layoutStart == std::string::npos)
      break;
    size_t layoutEnd = output.find("'", layoutStart + 1);
    if (layoutEnd == std::string::npos)
      break;
    xkbLayout = output.substr(layoutStart + 1, layoutEnd - layoutStart - 1);
    pos = layoutEnd;

    if (!xkbLayout.empty()) {
      // Convert XKB layout name to ISO 639-1 via evdev.xml
      std::string evdevPath = "/usr/share/X11/xkb/rules/evdev.xml";
      if (!std::filesystem::exists(evdevPath))
        evdevPath = "/usr/local/share/X11/xkb/rules/evdev.xml";

      std::string isoCode = X11LayoutsParser::convertLayoutToISO(evdevPath, xkbLayout, false);
      if (!isoCode.empty() &&
          std::find(layoutLangCodes.begin(), layoutLangCodes.end(), isoCode) == layoutLangCodes.end()) {
        layoutLangCodes.push_back(isoCode);
        LOG_INFO("gsettings: detected layout '%s' -> ISO code '%s'", xkbLayout.c_str(), isoCode.c_str());
      } else if (isoCode.empty()) {
        LOG_WARN("gsettings: layout '%s' not found in evdev.xml", xkbLayout.c_str());
      }
    }
  }

  return layoutLangCodes;
}

std::string AppUtilUnix::getCurrentLanguageCode()
{
  std::string result = "";
#if WINAPI_XWINDOWS

  auto display = XOpenDisplay(nullptr);
  if (!display) {
    LOG_WARN("failed to open x11 default display");
    return result;
  }

  auto kbdDescr = XkbAllocKeyboard();
  if (!kbdDescr) {
    LOG_WARN("failed to get x11 keyboard description");
    return result;
  }
  XkbGetNames(display, XkbSymbolsNameMask, kbdDescr);

  Atom symNameAtom = kbdDescr->names->symbols;
  auto rawLayouts = std::string(XGetAtomName(display, symNameAtom));

  XkbStateRec state;
  XkbGetState(display, XkbUseCoreKbd, &state);
  auto neededGroupIndex = static_cast<int>(state.group);

  size_t groupIdx = 0;
  size_t groupStartI = 0;
  for (size_t strI = 0; strI < rawLayouts.size(); strI++) {
    if (rawLayouts[strI] != '+') {
      continue;
    }

    if (auto group = rawLayouts.substr(groupStartI, strI - groupStartI);
        group.find("group", 0, 5) == std::string::npos && group.find("inet", 0, 4) == std::string::npos &&
        group.find("pc", 0, 2) == std::string::npos) {
      if (neededGroupIndex == groupIdx) {
        result = group.substr(0, std::min(group.find('(', 0), group.find(':', 0)));
        break;
      }
      groupIdx++;
    }

    groupStartI = strI + 1;
  }

  XkbFreeNames(kbdDescr, XkbSymbolsNameMask, true);
  XFree(kbdDescr);
  XCloseDisplay(display);

  result = X11LayoutsParser::convertLayoutToISO(m_evdev, result);

#elif defined(Q_OS_MAC)
  auto layoutLanguages =
      (CFArrayRef)TISGetInputSourceProperty(TISCopyCurrentKeyboardInputSource(), kTISPropertyInputSourceLanguages);
  char temporaryCString[128] = {0};
  for (CFIndex index = 0; index < CFArrayGetCount(layoutLanguages) && layoutLanguages; index++) {
    auto languageCode = (CFStringRef)CFArrayGetValueAtIndex(layoutLanguages, index);
    if (!languageCode || !CFStringGetCString(languageCode, temporaryCString, 128, kCFStringEncodingUTF8)) {
      continue;
    }

    result = std::string(temporaryCString);
    break;
  }
#endif
  return result;
}
