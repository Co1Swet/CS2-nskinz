#pragma once

#include <string_view>

namespace nskinzcs2::build
{
inline constexpr std::string_view kVersion = "0.6.3";
inline constexpr std::wstring_view kVersionWide = L"0.6.3";
inline constexpr std::wstring_view kDisplayName = L"nSkinzCS2 v0.6.3";

#ifdef NSKINZCS2_UI_LANG_ZH_CN
inline constexpr bool kUiLanguageChinese = true;
inline constexpr std::wstring_view kCatalogLocalizationFileName = L"csgo_schinese.txt";
inline constexpr std::string_view kCatalogLocalizationVpkPath = "resource/csgo_schinese.txt";
#else
inline constexpr bool kUiLanguageChinese = false;
inline constexpr std::wstring_view kCatalogLocalizationFileName = L"csgo_english.txt";
inline constexpr std::string_view kCatalogLocalizationVpkPath = "resource/csgo_english.txt";
#endif
}
