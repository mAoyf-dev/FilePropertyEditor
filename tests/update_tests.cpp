#include "internal.h"
#include "update.h"
#include <fstream>
#include <iostream>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << "FAIL " << __LINE__ << ": " << #x << "\n";                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main(int argc, char **argv) {
  using namespace fp;
  CHECK(NewerVersion(L"v2.10.0", L"2.9.0"));
  CHECK(!NewerVersion(L"v2.1.0", L"2.1.0"));
  CHECK(!NewerVersion(L"v2.0.99", L"2.1.0"));
  CHECK(!NewerVersion(L"2.2.0-beta", L"2.1.0"));
  CHECK(!NewerVersion(L"2.2.0junk", L"2.1.0"));
  CHECK(!NewerVersion(L"99999999999999999999.0", L"2.1.0"));
  CHECK(NewerVersion(L"2.1.1.0", L"2.1"));
  CHECK(TrustedReleaseUrl(L"https://github.com/mAoyf-dev/FilePropertyEditor/"
                          L"releases/download/v2.2/a.zip",
                          true));
  CHECK(!TrustedReleaseUrl(
      L"https://github.com/other/repo/releases/download/v2/a.zip", true));
  CHECK(!TrustedReleaseUrl(L"http://github.com/mAoyf-dev/FilePropertyEditor/"
                           L"releases/download/v2/a.zip",
                           true));
  CHECK(!TrustedReleaseUrl(L"https://github.com.evil.test/mAoyf-dev/"
                           L"FilePropertyEditor/releases/download/v2/a.zip",
                           true));
  CHECK(!TrustedReleaseUrl(L"https://github.com/mAoyf-dev/FilePropertyEditor/"
                           L"releases/download/../../evil",
                           true));
  const std::string json =
      R"({"tag_name":"v2.2.0","name":"\u6587\u4ef6","body":"new\nversion","draft":false,"prerelease":false,"html_url":"https://github.com/mAoyf-dev/FilePropertyEditor/releases/tag/v2.2.0","assets":[{"name":"app_x64.zip","size":3,"browser_download_url":"https://github.com/mAoyf-dev/FilePropertyEditor/releases/download/v2.2.0/app_x64.zip","digest":"sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},{"name":"app_x86.zip","size":4,"browser_download_url":"https://github.com/mAoyf-dev/FilePropertyEditor/releases/download/v2.2.0/app_x86.zip"}]})";
  auto r = ParseRelease(json);
  CHECK(r.version == L"v2.2.0" && r.name == L"文件");
  CHECK(r.notes == L"new\nversion");
  CHECK(ChooseAsset(r, true).name == L"app_x64.zip");
  CHECK(ChooseAsset(r, false).name == L"app_x86.zip");
  r.assets.erase(r.assets.begin());
  CHECK(ChooseAsset(r, true).url.empty());
  r.assets.push_back({L"文件属性查改器_V2.2_32-64位.zip",
                      L"https://github.com/mAoyf-dev/FilePropertyEditor/"
                      L"releases/download/v2.2.0/all.zip",
                      L"", 9});
  CHECK(ChooseAsset(r, true).size == 9);
  Release architecture;
  architecture.assets.push_back({L"app_x86_64.zip", L"unused", L"", 3});
  CHECK(ChooseAsset(architecture, false).url.empty());
  CHECK(ChooseAsset(architecture, true).size == 3);
  architecture.assets[0].name = L"app_x86_and_x64.zip";
  CHECK(ChooseAsset(architecture, false).url.empty());
  CHECK(ChooseAsset(architecture, true).url.empty());
  architecture.assets[0].name = L"app_x86-x64.zip";
  CHECK(ChooseAsset(architecture, false).size == 3);
  CHECK(ChooseAsset(architecture, true).size == 3);
  for (const auto &bad :
       {std::string("{}"), std::string("{\"tag_name\":\"v2.2\"}"),
        json + "junk", std::string("{\"x\":\"\\uD800\"}"),
        std::string("{\"x\":01}"), std::string(2 * 1024 * 1024 + 1, ' ')}) {
    bool failed = false;
    try {
      ParseRelease(bad);
    } catch (const std::exception &) {
      failed = true;
    }
    CHECK(failed);
  }
  std::atomic_bool cancel{true};
  bool stopped = false;
  try {
    DownloadRelease(ChooseAsset(ParseRelease(json), true),
                    L"build/should-not-exist.zip", cancel);
  } catch (const std::exception &) {
    stopped = true;
  }
  CHECK(stopped);
  CHECK(GetFileAttributesW(L"build/should-not-exist.zip") ==
        INVALID_FILE_ATTRIBUTES);
  if (argc > 1 && std::string(argv[1]) == "--online") {
    cancel = false;
    auto live = CheckRelease(cancel);
    auto asset = ChooseAsset(live, sizeof(void *) == 8);
    CHECK(!asset.url.empty() && !asset.sha256.empty());
    std::cout << "live tag: " << Utf8(live.version)
              << ", asset: " << Utf8(asset.name) << "\n";
    const std::wstring target = FullPath(L"build/update-integration.zip");
    WriteAtomic(target, "keep");
    auto kept = [&] {
      std::ifstream stream(target, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(stream), {}) == "keep";
    };
    auto noTemp = [&] {
      WIN32_FIND_DATAW data;
      HANDLE found =
          FindFirstFileW((target + L".fpv-*.download").c_str(), &data);
      if (found == INVALID_HANDLE_VALUE)
        return true;
      FindClose(found);
      return false;
    };
    bool failed = false;
    try {
      DownloadRelease(asset, target, cancel,
                      [&](ULONGLONG, ULONGLONG) { cancel = true; });
    } catch (const std::exception &) {
      failed = true;
    }
    CHECK(failed && kept() && noTemp());
    cancel = false;
    auto damaged = asset;
    damaged.sha256.assign(64, L'0');
    failed = false;
    try {
      DownloadRelease(damaged, target, cancel);
    } catch (const std::exception &) {
      failed = true;
    }
    CHECK(failed && kept() && noTemp());
    damaged = asset;
    damaged.size = 1;
    failed = false;
    try {
      DownloadRelease(damaged, target, cancel);
    } catch (const std::exception &) {
      failed = true;
    }
    CHECK(failed && kept() && noTemp());
    ULONGLONG last = 0;
    DownloadRelease(asset, target, cancel,
                    [&](ULONGLONG done, ULONGLONG total) {
                      if (done < last || done > total)
                        throw std::runtime_error("invalid progress");
                      last = done;
                    });
    CHECK(last == asset.size && noTemp());
    CHECK(HashFile(target, false, true, cancel).sha256 == asset.sha256);
    DeleteFileW(target.c_str());
    std::cout << "online download/cancel/checksum/size/cleanup tests passed\n";
  }
  std::cout << "update tests passed\n";
}
