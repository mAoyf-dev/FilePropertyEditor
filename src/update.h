#pragma once
#include "core.h"
namespace fp {
struct ReleaseAsset {
  std::wstring name, url, sha256;
  ULONGLONG size = 0;
};
struct Release {
  std::wstring version, name, notes, url;
  std::vector<ReleaseAsset> assets;
};
bool NewerVersion(const std::wstring &candidate, const std::wstring &current);
bool TrustedReleaseUrl(const std::wstring &url, bool asset);
Release ParseRelease(const std::string &json);
Release ParseReleaseAssets(const std::wstring &releaseUrl,
                           const std::string &html);
ReleaseAsset ChooseAsset(const Release &release, bool x64);
Release CheckRelease(const std::atomic_bool &cancel);
Release CheckPublicRelease(const std::atomic_bool &cancel);
void DownloadRelease(const ReleaseAsset &asset, const std::wstring &destination,
                     const std::atomic_bool &cancel, Progress progress = {});
} // namespace fp
