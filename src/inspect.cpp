#include "internal.h"
#include <aclapi.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <wintrust.h>
namespace fp {
static std::wstring Owner(const std::wstring &path) {
  PSID sid = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  DWORD e = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION, &sid, nullptr,
                                  nullptr, nullptr, &sd);
  if (e)
    return L"读取失败：" + ErrorText(e);
  wchar_t name[512], domain[512];
  DWORD a = 512, b = 512;
  SID_NAME_USE use;
  std::wstring out;
  if (LookupAccountSidW(nullptr, sid, name, &a, domain, &b, &use))
    out = (b ? std::wstring(domain) + L"\\" : L"") + name;
  else
    out = L"无法解析账户";
  LocalFree(sd);
  return out;
}
static std::wstring Signature(const std::wstring &path) {
  WINTRUST_FILE_INFO f{};
  f.cbStruct = sizeof(f);
  f.pcwszFilePath = path.c_str();
  WINTRUST_DATA d{};
  d.cbStruct = sizeof(d);
  d.dwUIChoice = WTD_UI_NONE;
  d.fdwRevocationChecks = WTD_REVOKE_NONE;
  d.dwUnionChoice = WTD_CHOICE_FILE;
  d.pFile = &f;
  d.dwStateAction = WTD_STATEACTION_VERIFY;
  d.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG status =
      WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &d);
  d.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &d);
  if (status == ERROR_SUCCESS)
    return L"验证通过（离线，未检查吊销）";
  if (status == TRUST_E_NOSIGNATURE)
    return L"未发现嵌入签名（未检查目录签名）";
  return L"验证未通过：" + ErrorText(static_cast<DWORD>(status));
}
static std::wstring VariantText(const PROPVARIANT &v) {
  if (v.vt == VT_EMPTY || v.vt == VT_NULL)
    return {};
  if (v.vt == VT_FILETIME)
    return FormatTime(v.filetime);
  PWSTR s = nullptr;
  if (FAILED(PropVariantToStringAlloc(v, &s)))
    return L"（此属性值无法转换为文本）";
  std::wstring out = s ? s : L"";
  CoTaskMemFree(s);
  return out;
}
static bool EditableType(VARTYPE t) {
  return t == VT_LPWSTR || t == VT_BSTR || t == (VT_VECTOR | VT_LPWSTR) ||
         t == VT_UI4 || t == VT_UI8 || t == VT_I4 || t == VT_I8 || t == VT_R8 ||
         t == VT_BOOL || t == VT_FILETIME;
}
static void PropertyRows(FileInfo &info) {
  ComPtr<IPropertyStore> store;
  HRESULT hr = SHGetPropertyStoreFromParsingName(
      info.path.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(&store));
  bool writable = SUCCEEDED(hr);
  if (FAILED(hr))
    hr = SHGetPropertyStoreFromParsingName(info.path.c_str(), nullptr,
                                           GPS_DEFAULT, IID_PPV_ARGS(&store));
  if (FAILED(hr)) {
    info.rows.push_back(
        {L"扩展属性状态", L"无可用的 Windows 属性处理器：" + ErrorText(hr)});
    return;
  }
  ComPtr<IPropertyStore> write;
  if (writable)
    write = store;
  ComPtr<IPropertyStoreCapabilities> caps;
  if (write)
    write.As(&caps);
  DWORD n = 0;
  Hr(store->GetCount(&n));
  std::vector<PROPERTYKEY> keys;
  for (DWORD i = 0; i < std::min(n, static_cast<DWORD>(512)); ++i) {
    PROPERTYKEY k;
    if (SUCCEEDED(store->GetAt(i, &k)))
      keys.push_back(k);
  }
  for (auto name :
       {L"System.Title", L"System.Subject", L"System.Author",
        L"System.Keywords", L"System.Comment", L"System.Photo.DateTaken",
        L"System.Music.Artist", L"System.Music.AlbumTitle",
        L"System.Media.Year", L"System.Document.DateCreated",
        L"System.Document.DateSaved"}) {
    PROPERTYKEY k;
    if (SUCCEEDED(PSGetPropertyKeyFromName(name, &k)) &&
        std::none_of(keys.begin(), keys.end(), [&](const PROPERTYKEY &x) {
          return IsEqualPropertyKey(x, k);
        }))
      keys.push_back(k);
  }
  for (const auto &k : keys) {
    ComPtr<IPropertyDescription> d;
    if (FAILED(PSGetPropertyDescription(k, IID_PPV_ARGS(&d))))
      continue;
    PWSTR canonical = nullptr, label = nullptr;
    d->GetCanonicalName(&canonical);
    d->GetDisplayName(&label);
    std::wstring key = canonical ? canonical : L"", name = label ? label : key;
    CoTaskMemFree(canonical);
    CoTaskMemFree(label);
    if (key.empty())
      continue;
    if (IsOffice(info.path) &&
        (key == L"System.Title" || key == L"System.Subject" ||
         key == L"System.Author" || key == L"System.Keywords" ||
         key == L"System.Comment" || key == L"System.Document.DateCreated" ||
         key == L"System.Document.DateSaved" ||
         key == L"System.Document.LastAuthor" || key == L"System.Category"))
      continue;
    PROPVARIANT val{};
    if (FAILED(store->GetValue(k, &val)))
      continue;
    VARTYPE type = VT_EMPTY;
    d->GetPropertyType(&type);
    PROPDESC_TYPE_FLAGS flags{};
    d->GetTypeFlags(PDTF_ISINNATE, &flags);
    bool editable = write && EditableType(type) && !(flags & PDTF_ISINNATE) &&
                    caps && caps->IsPropertyWritable(k) == S_OK;
    auto value = VariantText(val);
    PropVariantClear(&val);
    if (value.empty() && !editable)
      continue;
    info.rows.push_back(
        {name, value, key, editable ? Kind::Property : Kind::ReadOnly});
  }
  if (n > 512)
    info.rows.push_back(
        {L"扩展属性提示", L"超过 512 项，仅显示前 512 项及常用可编辑项"});
}
static std::wstring TypeHelp(const std::wstring &ext) {
  std::wstring e = ext;
  CharLowerBuffW(e.data(), static_cast<DWORD>(e.size()));
  if (e == L".mp3" || e == L".wav" || e == L".flac" || e == L".aac" ||
      e == L".m4a" || e == L".ogg")
    return L"音频文件，可使用支持此格式的音乐/视频播放器打开";
  if (e == L".jpg" || e == L".jpeg" || e == L".png" || e == L".gif" ||
      e == L".bmp" || e == L".tif" || e == L".tiff" || e == L".webp")
    return L"图片文件，可使用照片查看器或图像编辑软件打开";
  if (e == L".iso")
    return L"光盘映像，可使用系统装载功能或光盘映像工具打开";
  if (e == L".doc" || e == L".docx" || e == L".rtf" || e == L".odt")
    return L"文档，可使用兼容格式的文字处理软件打开";
  if (e == L".xlsx" || e == L".xls")
    return L"电子表格，可使用 Excel 或兼容办公软件打开";
  if (e == L".mp4" || e == L".mkv" || e == L".avi" || e == L".mov")
    return L"视频文件，可使用支持相应编解码器的视频播放器打开";
  return L"类型说明依据扩展名/系统关联；扩展名可能被更改，请结合文件头识别判断";
}
static std::wstring MagicType(const std::wstring &path) {
  Handle file(
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, 0, nullptr));
  BYTE b[16]{};
  DWORD n;
  WinCheck(ReadFile(file.h, b, sizeof(b), &n, nullptr));
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
    return L"PNG 图片";
  if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)
    return L"JPEG 图片";
  if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
    return L"GIF 图片";
  if (n >= 5 && memcmp(b, "%PDF-", 5) == 0)
    return L"PDF 文档";
  if (n >= 4 && memcmp(b, "PK\x03\x04", 4) == 0)
    return L"ZIP 容器（也可能是 Office 文档等）";
  if (n >= 8 && memcmp(b, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8) == 0)
    return L"OLE 复合文档（如旧版 Office）";
  if (n >= 3 && memcmp(b, "ID3", 3) == 0)
    return L"ID3 音频标签（常见于 MP3）";
  if (n >= 4 && memcmp(b, "fLaC", 4) == 0)
    return L"FLAC 音频";
  if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0)
    return L"WAV 音频";
  if (n >= 2 && b[0] == 'M' && b[1] == 'Z')
    return L"DOS/Windows 可执行映像标记（MZ）";
  return L"未识别（仅探测常见格式，不代表文件无效）";
}
FileInfo Inspect(const std::wstring &input) {
  FileInfo info;
  info.path = FullPath(input);
  WIN32_FILE_ATTRIBUTE_DATA a{};
  WinCheck(GetFileAttributesExW(info.path.c_str(), GetFileExInfoStandard, &a));
  if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    Fail(L"请选择文件，目录请使用添加文件夹");
  auto add = [&](const std::wstring &name, const std::wstring &value) {
    info.rows.push_back({name, value});
  };
  add(L"文件名", FileName(info.path));
  add(L"文件路径", info.path);
  std::wstring ext = PathFindExtensionW(info.path.c_str());
  add(L"扩展名", ext.empty() ? L"（无扩展名）" : ext);
  SHFILEINFOW sh{};
  if (SHGetFileInfoW(info.path.c_str(), 0, &sh, sizeof(sh), SHGFI_TYPENAME))
    add(L"系统文件类型", sh.szTypeName);
  add(L"类型与打开建议", TypeHelp(ext));
  wchar_t app[2048];
  DWORD count = _countof(app);
  if (!ext.empty() &&
      SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, ext.c_str(),
                                  nullptr, app, &count)))
    add(L"默认打开程序", app);
  else
    add(L"默认打开程序", L"系统未注册默认应用");
  try {
    add(L"文件头识别", MagicType(info.path));
  } catch (const std::exception &e) {
    add(L"文件头识别", Wide(e.what()));
  }
  ULARGE_INTEGER size;
  size.LowPart = a.nFileSizeLow;
  size.HighPart = a.nFileSizeHigh;
  add(L"字节大小", std::to_wstring(size.QuadPart));
  wchar_t h[64];
  if (size.QuadPart >= 1024 * 1024 * 1024)
    swprintf_s(h, L"%.2f GiB",
               static_cast<double>(size.QuadPart) / (1024 * 1024 * 1024));
  else if (size.QuadPart >= 1024 * 1024)
    swprintf_s(h, L"%.2f MiB",
               static_cast<double>(size.QuadPart) / (1024 * 1024));
  else
    swprintf_s(h, L"%.2f KiB", static_cast<double>(size.QuadPart) / 1024);
  add(L"大小", h);
  info.rows.push_back({L"创建时间（文件系统）", FormatTime(a.ftCreationTime),
                       L"created", Kind::Time});
  info.rows.push_back({L"修改时间（文件系统）", FormatTime(a.ftLastWriteTime),
                       L"modified", Kind::Time});
  info.rows.push_back({L"访问时间（文件系统）", FormatTime(a.ftLastAccessTime),
                       L"accessed", Kind::Time});
  for (const auto &f : std::vector<std::pair<const wchar_t *, DWORD>>{
           {L"只读", FILE_ATTRIBUTE_READONLY},
           {L"隐藏", FILE_ATTRIBUTE_HIDDEN},
           {L"系统", FILE_ATTRIBUTE_SYSTEM},
           {L"存档", FILE_ATTRIBUTE_ARCHIVE}})
    info.rows.push_back({f.first,
                         (a.dwFileAttributes & f.second) ? L"是" : L"否",
                         std::to_wstring(f.second), Kind::Attribute});
  add(L"属性位（十进制）", std::to_wstring(a.dwFileAttributes));
  info.rows.push_back({L"文件所有者", Owner(info.path), L"owner", Kind::Owner});
  wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD c = _countof(computer);
  if (GetComputerNameW(computer, &c))
    add(L"当前计算机", computer);
  // shortcut: download provenance only comes from Zone.Identifier; ordinary
  // files do not retain complete source history.
  HANDLE zone =
      CreateFileW((info.path + L":Zone.Identifier").c_str(), GENERIC_READ,
                  FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (zone != INVALID_HANDLE_VALUE) {
    char b[16384];
    DWORD n = 0;
    if (ReadFile(zone, b, sizeof(b), &n, nullptr)) {
      try {
        add(L"下载来源（Zone.Identifier）", Wide(std::string(b, n)));
      } catch (const std::exception &) {
        add(L"下载来源", L"来源流存在，但不是有效 UTF-8 文本");
      }
    }
    CloseHandle(zone);
  }
  VersionRows(info);
  add(L"数字签名", Signature(info.path));
  add(L"MD5", L"未计算（默认关闭，可按需计算）");
  add(L"SHA-256", L"未计算（默认关闭，可按需计算）");
  if (IsOffice(info.path))
    OfficeRows(info);
  PropertyRows(info);
  return info;
}
} // namespace fp
