#include "update.h"
#include "internal.h"
#include "version.h"
#include <array>
#include <map>
#include <winhttp.h>

namespace fp {
namespace {
constexpr size_t JsonLimit = 2 * 1024 * 1024;
void Canceled(const std::atomic_bool &cancel) {
  if (cancel)
    Fail(L"已取消更新操作");
}
struct Json {
  char type = 0;
  std::wstring text;
  std::map<std::wstring, Json> object;
  std::vector<Json> array;
  const Json &at(const wchar_t *key) const {
    static const Json empty;
    auto i = object.find(key);
    return i == object.end() ? empty : i->second;
  }
};
class Parser {
  std::wstring input;
  size_t pos = 0;
  [[noreturn]] void bad() { Fail(L"GitHub 返回的更新数据无效"); }
  void space() {
    while (pos < input.size() && (input[pos] == L' ' || input[pos] == L'\n' ||
                                  input[pos] == L'\r' || input[pos] == L'\t'))
      ++pos;
  }
  bool take(wchar_t c) {
    space();
    if (pos < input.size() && input[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }
  unsigned hex4() {
    unsigned n = 0;
    for (int i = 0; i < 4; ++i) {
      if (pos == input.size())
        bad();
      wchar_t c = input[pos++];
      unsigned d = c >= L'0' && c <= L'9'   ? c - L'0'
                   : c >= L'a' && c <= L'f' ? c - L'a' + 10
                   : c >= L'A' && c <= L'F' ? c - L'A' + 10
                                            : 16;
      if (d == 16)
        bad();
      n = n * 16 + d;
    }
    return n;
  }
  std::wstring string() {
    if (!take(L'"'))
      bad();
    std::wstring out;
    while (pos < input.size()) {
      wchar_t c = input[pos++];
      if (c == L'"')
        return out;
      if (c < 32)
        bad();
      if (c != L'\\') {
        out += c;
        continue;
      }
      if (pos == input.size())
        bad();
      c = input[pos++];
      switch (c) {
      case L'"':
      case L'\\':
      case L'/':
        out += c;
        break;
      case L'b':
        out += L'\b';
        break;
      case L'f':
        out += L'\f';
        break;
      case L'n':
        out += L'\n';
        break;
      case L'r':
        out += L'\r';
        break;
      case L't':
        out += L'\t';
        break;
      case L'u': {
        unsigned h = hex4();
        if (h >= 0xDC00 && h <= 0xDFFF)
          bad();
        out += static_cast<wchar_t>(h);
        if (h >= 0xD800 && h <= 0xDBFF) {
          if (pos + 2 > input.size() || input[pos++] != L'\\' ||
              input[pos++] != L'u')
            bad();
          unsigned l = hex4();
          if (l < 0xDC00 || l > 0xDFFF)
            bad();
          out += static_cast<wchar_t>(l);
        }
        break;
      }
      default:
        bad();
      }
    }
    bad();
  }
  Json value(unsigned depth) {
    if (depth > 32)
      bad();
    space();
    if (pos == input.size())
      bad();
    Json v;
    if (take(L'{')) {
      v.type = 'o';
      if (take(L'}'))
        return v;
      do {
        auto key = string();
        if (!take(L':'))
          bad();
        if (!v.object.emplace(key, value(depth + 1)).second)
          bad();
      } while (take(L','));
      if (!take(L'}'))
        bad();
    } else if (take(L'[')) {
      v.type = 'a';
      if (take(L']'))
        return v;
      do {
        v.array.push_back(value(depth + 1));
      } while (take(L','));
      if (!take(L']'))
        bad();
    } else if (input[pos] == L'"') {
      v.type = 's';
      v.text = string();
    } else if (input[pos] == L't' || input[pos] == L'f' || input[pos] == L'n') {
      const wchar_t *literal = input[pos] == L't'   ? L"true"
                               : input[pos] == L'f' ? L"false"
                                                    : L"null";
      size_t len = wcslen(literal);
      if (input.compare(pos, len, literal) != 0)
        bad();
      v.type = input[pos] == L'n' ? '0' : 'b';
      v.text = literal;
      pos += len;
    } else {
      v.type = 'n';
      size_t begin = pos;
      if (input[pos] == L'-')
        ++pos;
      auto digit = [&]() {
        return pos < input.size() && input[pos] >= L'0' && input[pos] <= L'9';
      };
      if (!digit())
        bad();
      if (input[pos] == L'0')
        ++pos;
      else
        while (digit())
          ++pos;
      if (pos < input.size() && input[pos] == L'.') {
        ++pos;
        if (!digit())
          bad();
        while (digit())
          ++pos;
      }
      if (pos < input.size() && (input[pos] == L'e' || input[pos] == L'E')) {
        ++pos;
        if (pos < input.size() && (input[pos] == L'+' || input[pos] == L'-'))
          ++pos;
        if (!digit())
          bad();
        while (digit())
          ++pos;
      }
      v.text = input.substr(begin, pos - begin);
    }
    return v;
  }

public:
  Json parse(const std::string &bytes) {
    if (bytes.empty() || bytes.size() > JsonLimit)
      bad();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                static_cast<int>(bytes.size()), nullptr, 0);
    if (!n)
      bad();
    input.resize(n);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                        static_cast<int>(bytes.size()), input.data(), n);
    Json out = value(0);
    space();
    if (pos != input.size())
      bad();
    return out;
  }
};
bool Version(const std::wstring &s, std::array<unsigned, 4> &out) {
  size_t p = !s.empty() && (s[0] == L'v' || s[0] == L'V') ? 1 : 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (p == s.size() || s[p] < L'0' || s[p] > L'9')
      return false;
    unsigned n = 0;
    while (p < s.size() && s[p] >= L'0' && s[p] <= L'9') {
      n = n * 10 + (s[p++] - L'0');
      if (n > 65535)
        return false;
    }
    out[i] = n;
    if (p == s.size())
      return true;
    if (s[p++] != L'.')
      return false;
  }
  return false;
}
ULONGLONG Size(const Json &v) {
  if (v.type != 'n' || v.text.empty())
    Fail(L"发行附件大小无效");
  ULONGLONG n = 0;
  for (wchar_t c : v.text) {
    if (c < L'0' || c > L'9' ||
        n > (4ULL * 1024 * 1024 * 1024 - (c - L'0')) / 10)
      Fail(L"发行附件大小无效或超过 4 GiB");
    n = n * 10 + c - L'0';
  }
  return n;
}
std::wstring Required(const Json &v, const wchar_t *key) {
  auto &s = v.at(key);
  if (s.type != 's' || s.text.empty())
    Fail(L"发行数据缺少必要字段");
  return s.text;
}
bool Hex(const std::wstring &s) {
  return s.size() == 64 &&
         s.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos;
}
struct Internet {
  HINTERNET h = nullptr;
  explicit Internet(HINTERNET value) : h(value) {
    if (!h)
      Fail(ErrorText());
  }
  ~Internet() { WinHttpCloseHandle(h); }
  Internet(const Internet &) = delete;
};
enum class RequestKind { Api, Page, Download };
struct HttpError : std::runtime_error {
  DWORD code;
  explicit HttpError(DWORD status)
      : std::runtime_error(
            Utf8(L"GitHub 请求失败，HTTP " + std::to_wstring(status))),
        code(status) {}
};
struct Url {
  std::wstring host, path;
};
Url SplitUrl(const std::wstring &url) {
  URL_COMPONENTS c{};
  c.dwStructSize = sizeof(c);
  c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength =
      static_cast<DWORD>(-1);
  WinCheck(WinHttpCrackUrl(url.c_str(), 0, 0, &c));
  if (c.nScheme != INTERNET_SCHEME_HTTPS ||
      c.nPort != INTERNET_DEFAULT_HTTPS_PORT || c.dwUserNameLength ||
      c.dwPasswordLength)
    Fail(L"更新链接不是有效的 HTTPS 地址");
  return {std::wstring(c.lpszHostName, c.dwHostNameLength),
          std::wstring(c.lpszUrlPath, c.dwUrlPathLength) +
              std::wstring(c.lpszExtraInfo ? c.lpszExtraInfo : L"",
                           c.dwExtraInfoLength)};
}
bool RedirectUrl(const std::wstring &url) {
  if (TrustedReleaseUrl(url, true))
    return true;
  try {
    auto u = SplitUrl(url);
    return u.host == L"release-assets.githubusercontent.com" ||
           u.host == L"objects.githubusercontent.com";
  } catch (...) {
    return false;
  }
}
template <class Sink>
void Request(std::wstring url, const std::atomic_bool &cancel, RequestKind kind,
             Sink sink, std::wstring *finalUrl = nullptr,
             ULONGLONG *headSize = nullptr) {
  Canceled(cancel);
  Internet session(WinHttpOpen(
      L"FilePropertyEditor/" FP_VERSION_WIDE, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  WinCheck(WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 5000));
  DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
  WinCheck(WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls,
                            sizeof(tls)));
  for (int redirects = 0; redirects <= 5; ++redirects) {
    Canceled(cancel);
    auto u = SplitUrl(url);
    Internet connection(WinHttpConnect(session.h, u.host.c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0));
    Internet request(WinHttpOpenRequest(
        connection.h, finalUrl || headSize ? L"HEAD" : L"GET", u.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    WinCheck(WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE,
                              &disabled, sizeof(disabled)));
    const wchar_t *headers =
        kind == RequestKind::Api    ? L"Accept: application/vnd.github+json\r\n"
        : kind == RequestKind::Page ? L"Accept: text/html\r\n"
                                    : L"Accept: application/octet-stream\r\n";
    WinCheck(WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
    Canceled(cancel);
    WinCheck(WinHttpReceiveResponse(request.h, nullptr));
    DWORD code = 0, length = sizeof(code);
    WinCheck(WinHttpQueryHeaders(
        request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &length, WINHTTP_NO_HEADER_INDEX));
    if (code == 301 || code == 302 || code == 303 || code == 307 ||
        code == 308) {
      wchar_t target[16384];
      length = sizeof(target);
      WinCheck(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION,
                                   WINHTTP_HEADER_NAME_BY_INDEX, target,
                                   &length, WINHTTP_NO_HEADER_INDEX));
      url = target;
      if (!(kind == RequestKind::Download
                ? RedirectUrl(url)
                : kind == RequestKind::Page && TrustedReleaseUrl(url, false)))
        Fail(L"拒绝不可信的下载重定向");
      continue;
    }
    if (code == 404)
      Fail(kind == RequestKind::Download
               ? L"发行附件不存在，可能已被删除"
               : L"仓库尚无正式 Release，或仓库不可访问");
    if (code != 200)
      throw HttpError(code);
    if (headSize) {
      wchar_t number[32];
      length = sizeof(number);
      WinCheck(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH,
                                   WINHTTP_HEADER_NAME_BY_INDEX, number,
                                   &length, WINHTTP_NO_HEADER_INDEX));
      Json size;
      size.type = 'n';
      size.text = number;
      *headSize = Size(size);
      if (!*headSize)
        Fail(L"发行附件为空");
    }
    if (finalUrl)
      *finalUrl = url;
    if (finalUrl || headSize)
      return;
    char buffer[65536];
    for (;;) {
      Canceled(cancel);
      DWORD received = 0;
      WinCheck(WinHttpReadData(request.h, buffer, sizeof(buffer), &received));
      Canceled(cancel);
      if (!received)
        return;
      sink(buffer, received);
    }
  }
  Fail(L"下载重定向次数过多");
}
} // namespace

bool NewerVersion(const std::wstring &candidate, const std::wstring &current) {
  std::array<unsigned, 4> a{}, b{};
  return Version(candidate, a) && Version(current, b) && a > b;
}
bool TrustedReleaseUrl(const std::wstring &url, bool asset) {
  const std::wstring prefix =
      std::wstring(FP_REPO) +
      (asset ? L"/releases/download/" : L"/releases/tag/");
  if (url.compare(0, prefix.size(), prefix) || url.size() <= prefix.size())
    return false;
  if (url.find_first_of(L"\r\n\\?#") != std::wstring::npos ||
      url.find(L"..") != std::wstring::npos)
    return false;
  for (wchar_t c : url)
    if (c <= 32 || c == 127)
      return false;
  auto lower = url;
  CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
  return lower.find(L"%2e") == std::wstring::npos &&
         lower.find(L"%2f") == std::wstring::npos &&
         lower.find(L"%5c") == std::wstring::npos;
}
Release ParseRelease(const std::string &json) {
  auto v = Parser().parse(json);
  if (v.type != 'o' || v.at(L"draft").type != 'b' ||
      v.at(L"draft").text != L"false" || v.at(L"prerelease").type != 'b' ||
      v.at(L"prerelease").text != L"false")
    Fail(L"未找到正式发行版本");
  Release r;
  r.version = Required(v, L"tag_name");
  std::array<unsigned, 4> version{};
  if (!Version(r.version, version))
    Fail(L"发行标签应为 v主版本.次版本.修订版本，例如 v2.2.0");
  r.name = v.at(L"name").type == 's' ? v.at(L"name").text : r.version;
  r.notes = v.at(L"body").type == 's' ? v.at(L"body").text : L"";
  r.url = Required(v, L"html_url");
  if (!TrustedReleaseUrl(r.url, false))
    Fail(L"发行页链接不属于本仓库");
  const auto &assets = v.at(L"assets");
  if (assets.type != 'a')
    Fail(L"发行附件列表无效");
  for (const auto &a : assets.array) {
    ReleaseAsset item;
    item.name = Required(a, L"name");
    item.url = Required(a, L"browser_download_url");
    item.size = Size(a.at(L"size"));
    const auto &d = a.at(L"digest");
    if (d.type == 's' && !d.text.empty()) {
      if (d.text.compare(0, 7, L"sha256:") || !Hex(d.text.substr(7)))
        Fail(L"发行附件摘要格式无效");
      item.sha256 = d.text.substr(7);
      CharLowerBuffW(item.sha256.data(),
                     static_cast<DWORD>(item.sha256.size()));
    }
    if (TrustedReleaseUrl(item.url, true) && item.size &&
        item.name.find_first_of(L"/\\:<>|?*\r\n") == std::wstring::npos &&
        item.name != L"." && item.name != L"..")
      r.assets.push_back(std::move(item));
  }
  return r;
}
ReleaseAsset ChooseAsset(const Release &release, bool x64) {
  ReleaseAsset chosen;
  int best = 0;
  for (const auto &a : release.assets) {
    auto name = a.name;
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    auto ext = name.substr(name.find_last_of(L'.') == std::wstring::npos
                               ? name.size()
                               : name.find_last_of(L'.'));
    if (ext != L".zip" && ext != L".exe")
      continue;
    bool unix64 = name.find(L"x86_64") != std::wstring::npos ||
                  name.find(L"x86-64") != std::wstring::npos;
    bool is64 = unix64 || name.find(L"x64") != std::wstring::npos ||
                name.find(L"amd64") != std::wstring::npos ||
                name.find(L"64位") != std::wstring::npos ||
                name.find(L"64-bit") != std::wstring::npos;
    bool is32 = (!unix64 && name.find(L"x86") != std::wstring::npos) ||
                name.find(L"32位") != std::wstring::npos ||
                name.find(L"32-bit") != std::wstring::npos;
    bool both = name.find(L"32-64") != std::wstring::npos ||
                name.find(L"x86-x64") != std::wstring::npos;
    if (name.find(L"arm") != std::wstring::npos ||
        name.find(L"source") != std::wstring::npos ||
        name.find(L"symbols") != std::wstring::npos)
      continue;
    int rank = both ? 1 : (x64 ? is64 && !is32 : is32 && !is64) ? 2 : 0;
    if (rank > best) {
      best = rank;
      chosen = a;
    }
  }
  return chosen;
}
Release ParseReleaseAssets(const std::wstring &releaseUrl,
                           const std::string &html) {
  if (!TrustedReleaseUrl(releaseUrl, false) || html.size() > JsonLimit)
    Fail(L"公开发布页地址或内容无效");
  Release r;
  r.url = releaseUrl;
  r.version = releaseUrl.substr(std::wstring(FP_REPO L"/releases/tag/").size());
  std::array<unsigned, 4> version{};
  if (!Version(r.version, version))
    Fail(L"公开发布页版本标签无效");
  r.name = r.version;
  r.notes = L"API 暂不可用，已通过 GitHub "
            L"公开发布页获取正式版本。完整说明请查看项目发布页。";
  std::string prefix = Utf8(
      L"/mAoyf-dev/FilePropertyEditor/releases/download/" + r.version + L"/");
  size_t pos = 0;
  while ((pos = html.find("<li", pos)) != std::string::npos) {
    if (pos + 3 >= html.size() ||
        (html[pos + 3] != ' ' && html[pos + 3] != '>')) {
      pos += 3;
      continue;
    }
    auto end = html.find("</li>", pos);
    if (end == std::string::npos)
      Fail(L"公开附件列表结构不完整");
    auto row = html.substr(pos, end - pos);
    pos = end + 5;
    auto href = row.find("href=\"" + prefix);
    if (href == std::string::npos)
      continue;
    href += 6;
    auto quote = row.find('"', href);
    if (quote == std::string::npos)
      Fail(L"公开附件链接不完整");
    ReleaseAsset a;
    a.url = L"https://github.com" + Wide(row.substr(href, quote - href));
    if (!TrustedReleaseUrl(a.url, true))
      continue;
    auto digest = row.find("sha256:");
    if (digest == std::string::npos)
      continue;
    a.sha256 = Wide(row.substr(digest + 7, 64));
    if (!Hex(a.sha256))
      Fail(L"公开附件 SHA-256 无效");
    CharLowerBuffW(a.sha256.data(), static_cast<DWORD>(a.sha256.size()));
    std::string encoded =
        row.substr(href + prefix.size(), quote - href - prefix.size());
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i) {
      if (encoded[i] == '%') {
        auto hex = [](char c) {
          return c >= '0' && c <= '9'   ? c - '0'
                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                        : -1;
        };
        if (i + 2 >= encoded.size() || hex(encoded[i + 1]) < 0 ||
            hex(encoded[i + 2]) < 0)
          Fail(L"附件名称编码无效");
        decoded +=
            static_cast<char>(hex(encoded[i + 1]) * 16 + hex(encoded[i + 2]));
        i += 2;
      } else
        decoded += encoded[i];
    }
    a.name = Wide(decoded);
    if (a.name.empty() ||
        a.name.find_first_of(L"/\\:<>|?*\r\n") != std::wstring::npos)
      continue;
    bool invalid = false;
    for (wchar_t c : a.name)
      if (c < 32)
        invalid = true;
    if (!invalid)
      r.assets.push_back(std::move(a));
  }
  return r;
}
Release CheckRelease(const std::atomic_bool &cancel) {
  static std::atomic<ULONGLONG> retryApiAfter{0};
  auto read = [&](const std::wstring &url, RequestKind kind) {
    std::string bytes;
    Request(url, cancel, kind, [&](const char *data, DWORD size) {
      if (bytes.size() + size > JsonLimit)
        Fail(L"更新数据超过大小限制");
      bytes.append(data, size);
    });
    return bytes;
  };
  if (GetTickCount64() >= retryApiAfter.load()) {
    try {
      return ParseRelease(read(
          L"https://api.github.com/repos/mAoyf-dev/FilePropertyEditor/releases/"
          L"latest",
          RequestKind::Api));
    } catch (const HttpError &e) {
      if (e.code != 403 && e.code != 429)
        throw;
      retryApiAfter = GetTickCount64() + 60ULL * 60 * 1000;
    }
  }
  return CheckPublicRelease(cancel);
}
Release CheckPublicRelease(const std::atomic_bool &cancel) {
  Canceled(cancel);
  auto ignore = [](const char *, DWORD) {};
  std::wstring releaseUrl;
  Request(FP_REPO L"/releases/latest", cancel, RequestKind::Page, ignore,
          &releaseUrl);
  if (!TrustedReleaseUrl(releaseUrl, false))
    Fail(L"未找到有效正式发布页");
  auto version =
      releaseUrl.substr(std::wstring(FP_REPO L"/releases/tag/").size());
  // Validate before incorporating a redirected tag into the next request path.
  std::array<unsigned, 4> parsed{};
  if (!Version(version, parsed))
    Fail(L"未找到有效正式发布页");
  std::string html;
  Request(FP_REPO L"/releases/expanded_assets/" + version, cancel,
          RequestKind::Page, [&](const char *data, DWORD size) {
            if (html.size() + size > JsonLimit)
              Fail(L"更新数据超过大小限制");
            html.append(data, size);
          });
  auto r = ParseReleaseAssets(releaseUrl, html);
  auto selected = ChooseAsset(r, sizeof(void *) == 8);
  r.assets.clear();
  if (!selected.url.empty()) {
    Request(selected.url, cancel, RequestKind::Download, ignore, nullptr,
            &selected.size);
    r.assets.push_back(std::move(selected));
  }
  return r;
}
void DownloadRelease(const ReleaseAsset &asset, const std::wstring &destination,
                     const std::atomic_bool &cancel, Progress progress) {
  Canceled(cancel);
  if (!TrustedReleaseUrl(asset.url, true) || !asset.size ||
      asset.size > 4ULL * 1024 * 1024 * 1024 ||
      (!asset.sha256.empty() && !Hex(asset.sha256)))
    Fail(L"下载附件信息无效");
  std::wstring path = FullPath(destination);
  TempFile temp;
  auto temporary = UniquePath(path, L".download");
  ULONGLONG done = 0;
  {
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    temp.path = temporary;
    Request(asset.url, cancel, RequestKind::Download,
            [&](const char *data, DWORD size) {
              if (done + size > asset.size)
                Fail(L"下载内容超过发行附件大小");
              DWORD written = 0;
              WinCheck(WriteFile(output.h, data, size, &written, nullptr));
              if (written != size)
                Fail(L"下载文件写入不完整");
              done += size;
              if (progress)
                progress(done, asset.size);
            });
    if (done != asset.size)
      Fail(L"下载中断或附件大小不符");
    WinCheck(FlushFileBuffers(output.h));
  }
  if (!asset.sha256.empty()) {
    auto hash = HashFile(temp.path, false, true, cancel);
    if (_wcsicmp(hash.sha256.c_str(), asset.sha256.c_str()))
      Fail(L"SHA-256 校验失败，已丢弃下载内容");
  }
  Canceled(cancel);
  WinCheck(MoveFileExW(temp.path.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
  temp.path.clear();
}
} // namespace fp
