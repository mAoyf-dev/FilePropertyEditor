#include "internal.h"
#include <aclapi.h>
#include <msxml6.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <set>
#include <shlobj.h>
#include <softpub.h>
#include <sstream>
#include <wincrypt.h>
#include <wintrust.h>

namespace fp {
std::string Utf8(const std::wstring &s) {
  if (s.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), nullptr, 0, nullptr,
                              nullptr);
  if (!n)
    throw std::runtime_error("Invalid Unicode text");
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}
std::wstring Wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), nullptr, 0);
  if (!n)
    throw std::runtime_error("Invalid UTF-8 text");
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), out.data(), n);
  return out;
}
std::wstring ErrorText(DWORD e) {
  LPWSTR p = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, e, 0, reinterpret_cast<LPWSTR>(&p), 0, nullptr);
  std::wstring out = p ? p : L"系统操作失败";
  LocalFree(p);
  while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n'))
    out.pop_back();
  wchar_t hex[24];
  swprintf_s(hex, L" (0x%08lX)", e);
  return out + hex;
}
std::wstring FullPath(const std::wstring &path) {
  DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!n)
    Fail(ErrorText());
  std::wstring out(n, L'\0');
  DWORD len = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
  if (!len || len >= n)
    Fail(L"路径过长或无效");
  out.resize(len);
  return out;
}
std::wstring FileName(const std::wstring &p) {
  auto n = p.find_last_of(L"\\/");
  return n == std::wstring::npos ? p : p.substr(n + 1);
}
std::wstring UniquePath(const std::wstring &p, const std::wstring &suffix) {
  GUID id;
  Hr(CoCreateGuid(&id));
  wchar_t s[40];
  StringFromGUID2(id, s, 40);
  return p + L".fpv-" + s + suffix;
}
std::wstring FormatTime(FILETIME ft) {
  SYSTEMTIME utc{}, local{};
  if (!FileTimeToSystemTime(&ft, &utc) ||
      !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
    return {};
  wchar_t s[32];
  swprintf_s(s, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth,
             local.wDay, local.wHour, local.wMinute, local.wSecond);
  return s;
}
FILETIME ParseTime(const std::wstring &text) {
  if (text.size() != 19 || text[4] != L'-' || text[7] != L'-' ||
      text[10] != L' ' || text[13] != L':' || text[16] != L':')
    Fail(L"时间格式应为 YYYY-MM-DD HH:MM:SS（本地时间）");
  for (size_t i = 0; i < text.size(); ++i)
    if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 &&
        (text[i] < L'0' || text[i] > L'9'))
      Fail(L"时间含有无效字符");
  SYSTEMTIME local{}, utc{}, round{}, back{};
  local.wYear = static_cast<WORD>(std::stoi(text.substr(0, 4)));
  local.wMonth = static_cast<WORD>(std::stoi(text.substr(5, 2)));
  local.wDay = static_cast<WORD>(std::stoi(text.substr(8, 2)));
  local.wHour = static_cast<WORD>(std::stoi(text.substr(11, 2)));
  local.wMinute = static_cast<WORD>(std::stoi(text.substr(14, 2)));
  local.wSecond = static_cast<WORD>(std::stoi(text.substr(17, 2)));
  FILETIME check{}, result{};
  if (!SystemTimeToFileTime(&local, &check) ||
      !FileTimeToSystemTime(&check, &round) || round.wDay != local.wDay ||
      round.wMonth != local.wMonth ||
      !TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) ||
      !SystemTimeToFileTime(&utc, &result))
    Fail(L"无效日期或时间");
  if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &back) ||
      back.wHour != local.wHour || back.wDay != local.wDay)
    Fail(L"本地时间处于夏令时跳变区间");
  return result;
}
static std::wstring IsoTime(FILETIME ft) {
  SYSTEMTIME s{};
  WinCheck(FileTimeToSystemTime(&ft, &s));
  wchar_t b[32];
  swprintf_s(b, L"%04u-%02u-%02uT%02u:%02u:%02uZ", s.wYear, s.wMonth, s.wDay,
             s.wHour, s.wMinute, s.wSecond);
  return b;
}
static std::wstring DisplayIso(const std::wstring &v) {
  unsigned y, m, d, h, min, s;
  if (swscanf_s(v.c_str(), L"%u-%u-%uT%u:%u:%u", &y, &m, &d, &h, &min, &s) != 6)
    return v;
  SYSTEMTIME st{};
  st.wYear = static_cast<WORD>(y);
  st.wMonth = static_cast<WORD>(m);
  st.wDay = static_cast<WORD>(d);
  st.wHour = static_cast<WORD>(h);
  st.wMinute = static_cast<WORD>(min);
  st.wSecond = static_cast<WORD>(s);
  FILETIME ft{};
  if (!SystemTimeToFileTime(&st, &ft))
    return v;
  auto tz = v.find_first_of(L"Z+-", 19);
  if (tz == std::wstring::npos)
    return v;
  if (v[tz] != L'Z') {
    unsigned th, tm;
    if (swscanf_s(v.c_str() + tz + 1, L"%u:%u", &th, &tm) != 2 || th > 23 ||
        tm > 59)
      return v;
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    ULONGLONG delta = static_cast<ULONGLONG>(th * 60 + tm) * 600000000;
    if (v[tz] == L'+')
      t.QuadPart -= delta;
    else
      t.QuadPart += delta;
    ft = {t.LowPart, t.HighPart};
  }
  return FormatTime(ft);
}
HashResult HashFile(const std::wstring &path, bool md5, bool sha256,
                    const std::atomic_bool &cancel, Progress progress) {
  if (!md5 && !sha256)
    return {};
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  LARGE_INTEGER size{};
  WinCheck(GetFileSizeEx(file.h, &size));
  HCRYPTPROV provider = 0;
  WinCheck(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                                CRYPT_VERIFYCONTEXT));
  struct Crypto {
    HCRYPTPROV p;
    HCRYPTHASH a = 0, b = 0;
    ~Crypto() {
      if (a)
        CryptDestroyHash(a);
      if (b)
        CryptDestroyHash(b);
      CryptReleaseContext(p, 0);
    }
  } crypto{provider};
  if (md5)
    WinCheck(CryptCreateHash(provider, CALG_MD5, 0, 0, &crypto.a));
  if (sha256)
    WinCheck(CryptCreateHash(provider, CALG_SHA_256, 0, 0, &crypto.b));
  std::vector<BYTE> bytes(1024 * 1024);
  ULONGLONG done = 0;
  DWORD last = 0;
  for (;;) {
    if (cancel)
      Fail(L"已取消，未保留部分摘要");
    DWORD n = 0;
    WinCheck(ReadFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()),
                      &n, nullptr));
    if (!n)
      break;
    if (crypto.a)
      WinCheck(CryptHashData(crypto.a, bytes.data(), n, 0));
    if (crypto.b)
      WinCheck(CryptHashData(crypto.b, bytes.data(), n, 0));
    done += n;
    if (progress && GetTickCount() - last >= 100) {
      progress(done, size.QuadPart);
      last = GetTickCount();
    }
  }
  if (cancel)
    Fail(L"已取消，未保留部分摘要");
  if (done != static_cast<ULONGLONG>(size.QuadPart))
    Fail(L"文件大小在读取过程中发生变化，摘要无效");
  auto digest = [](HCRYPTHASH h) {
    std::wstring out;
    if (!h)
      return out;
    BYTE b[64];
    DWORD n = sizeof(b);
    WinCheck(CryptGetHashParam(h, HP_HASHVAL, b, &n, 0));
    static const wchar_t hex[] = L"0123456789abcdef";
    for (DWORD i = 0; i < n; ++i) {
      out += hex[b[i] >> 4];
      out += hex[b[i] & 15];
    }
    return out;
  };
  if (progress)
    progress(done, size.QuadPart);
  return {digest(crypto.a), digest(crypto.b)};
}
std::vector<std::wstring> ExpandPaths(const std::vector<std::wstring> &paths,
                                      bool recursive,
                                      const std::atomic_bool &cancel,
                                      std::vector<std::wstring> &errors) {
  std::vector<std::wstring> out, pending = paths;
  std::set<std::wstring> seen;
  while (!pending.empty() && !cancel) {
    auto path = FullPath(pending.back());
    pending.pop_back();
    auto key = path;
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    if (!seen.insert(key).second)
      continue;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
      errors.push_back(path + L"：" + ErrorText());
      continue;
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
      out.push_back(path);
      continue;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
      errors.push_back(path + L"：跳过目录链接，避免循环");
      continue;
    }
    WIN32_FIND_DATAW d{};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &d);
    if (find == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_FILE_NOT_FOUND)
        errors.push_back(path + L"：" + ErrorText());
      continue;
    }
    do {
      if (cancel)
        break;
      if (wcscmp(d.cFileName, L".") == 0 || wcscmp(d.cFileName, L"..") == 0)
        continue;
      if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || recursive)
        pending.push_back(path + L"\\" + d.cFileName);
    } while (FindNextFileW(find, &d));
    DWORD e = GetLastError();
    FindClose(find);
    if (!cancel && e != ERROR_NO_MORE_FILES)
      errors.push_back(path + L"：" + ErrorText(e));
  }
  std::sort(out.begin(), out.end());
  return out;
}
ComPtr<IOpcFactory> OpcFactory() {
  ComPtr<IOpcFactory> f;
  Hr(CoCreateInstance(__uuidof(::OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                      IID_PPV_ARGS(&f)));
  return f;
}
ComPtr<IOpcPackage> OpenPackage(const std::wstring &path) {
  auto f = OpcFactory();
  ComPtr<IStream> s;
  Hr(f->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_READ, nullptr,
                           FILE_ATTRIBUTE_NORMAL, &s));
  ComPtr<IOpcPackage> p;
  Hr(f->ReadPackageFromStream(s.Get(), OPC_READ_DEFAULT, &p));
  return p;
}
ComPtr<IOpcPart> PackagePart(IOpcPackage *p, const std::wstring &name) {
  auto f = OpcFactory();
  ComPtr<IOpcPartUri> u;
  Hr(f->CreatePartUri(name.c_str(), &u));
  ComPtr<IOpcPartSet> set;
  Hr(p->GetPartSet(&set));
  ComPtr<IOpcPart> part;
  Hr(set->GetPart(u.Get(), &part));
  return part;
}
std::string ReadStream(IStream *s, ULONGLONG maximum) {
  STATSTG st{};
  Hr(s->Stat(&st, STATFLAG_NONAME));
  if (st.cbSize.QuadPart > maximum)
    Fail(L"元数据部件过大，停止解析");
  LARGE_INTEGER z{};
  Hr(s->Seek(z, STREAM_SEEK_SET, nullptr));
  std::string out(static_cast<size_t>(st.cbSize.QuadPart), '\0');
  ULONG n = 0;
  Hr(s->Read(out.data(), static_cast<ULONG>(out.size()), &n));
  if (n != out.size())
    Fail(L"元数据读取不完整");
  return out;
}
void WriteStream(IStream *s, const std::string &bytes) {
  LARGE_INTEGER z{};
  Hr(s->Seek(z, STREAM_SEEK_SET, nullptr));
  ULARGE_INTEGER size{};
  Hr(s->SetSize(size));
  ULONG n = 0;
  Hr(s->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &n));
  if (n != bytes.size())
    Fail(L"元数据写入不完整");
}
std::string ReadPackagePart(const std::wstring &path,
                            const std::wstring &name) {
  auto p = OpenPackage(path);
  auto part = PackagePart(p.Get(), name);
  ComPtr<IStream> s;
  Hr(part->GetContentStream(&s));
  return ReadStream(s.Get());
}
void SavePackage(IOpcPackage *p, const std::wstring &path,
                 PSECURITY_DESCRIPTOR security) {
  auto f = OpcFactory();
  ComPtr<IStream> s;
  SECURITY_ATTRIBUTES sa{sizeof(sa), security, FALSE};
  Hr(f->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_WRITE,
                           security ? &sa : nullptr, FILE_ATTRIBUTE_NORMAL,
                           &s));
  Hr(f->WritePackageToStream(p, OPC_WRITE_DEFAULT, s.Get()));
  Hr(s->Commit(STGC_DEFAULT));
}
void WritePackage(const std::wstring &path, const std::vector<Part> &parts,
                  bool workbook) {
  auto f = OpcFactory();
  ComPtr<IOpcPackage> p;
  Hr(f->CreatePackage(&p));
  ComPtr<IOpcPartSet> set;
  Hr(p->GetPartSet(&set));
  for (const auto &i : parts) {
    ComPtr<IOpcPartUri> u;
    Hr(f->CreatePartUri(i.name.c_str(), &u));
    ComPtr<IOpcPart> part;
    Hr(set->CreatePart(u.Get(), i.type.c_str(), OPC_COMPRESSION_NORMAL, &part));
    ComPtr<IStream> s;
    Hr(part->GetContentStream(&s));
    WriteStream(s.Get(), i.content);
  }
  if (workbook) {
    ComPtr<IOpcRelationshipSet> rel;
    Hr(p->GetRelationshipSet(&rel));
    ComPtr<IOpcPartUri> u;
    Hr(f->CreatePartUri(L"/xl/workbook.xml", &u));
    ComPtr<IOpcRelationship> r;
    Hr(rel->CreateRelationship(
        L"rId1",
        L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        L"officeDocument",
        u.Get(), OPC_URI_TARGET_MODE_INTERNAL, &r));
    auto wb = PackagePart(p.Get(), L"/xl/workbook.xml");
    rel.Reset();
    Hr(wb->GetRelationshipSet(&rel));
    u.Reset();
    Hr(f->CreatePartUri(L"/xl/worksheets/sheet1.xml", &u));
    r.Reset();
    Hr(rel->CreateRelationship(L"rId1",
                               L"http://schemas.openxmlformats.org/"
                               L"officeDocument/2006/relationships/worksheet",
                               u.Get(), OPC_URI_TARGET_MODE_INTERNAL, &r));
  }
  SavePackage(p.Get(), path);
}
struct Bstr {
  BSTR p;
  explicit Bstr(const wchar_t *s) : p(SysAllocString(s)) {
    if (!p)
      throw std::bad_alloc();
  }
  explicit Bstr(const std::wstring &s)
      : p(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {
    if (!p)
      throw std::bad_alloc();
  }
  ~Bstr() { SysFreeString(p); }
  operator BSTR() const { return p; }
};
static ComPtr<IXMLDOMDocument2> ParseXml(const std::string &xml) {
  ComPtr<IXMLDOMDocument2> d;
  Hr(CoCreateInstance(CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER,
                      IID_PPV_ARGS(&d)));
  Hr(d->put_async(VARIANT_FALSE));
  Hr(d->put_resolveExternals(VARIANT_FALSE));
  Hr(d->put_validateOnParse(VARIANT_FALSE));
  VARIANT yes{};
  yes.vt = VT_BOOL;
  yes.boolVal = VARIANT_TRUE;
  Hr(d->setProperty(Bstr(L"ProhibitDTD"), yes));
  ComPtr<IStream> s;
  s.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(xml.data()),
                             static_cast<UINT>(xml.size())));
  if (!s)
    throw std::bad_alloc();
  VARIANT source{};
  source.vt = VT_UNKNOWN;
  source.punkVal = s.Get();
  VARIANT_BOOL ok = VARIANT_FALSE;
  Hr(d->load(source, &ok));
  if (ok != VARIANT_TRUE)
    Fail(L"Office 元数据 XML 无效");
  VARIANT ns{};
  ns.vt = VT_BSTR;
  ns.bstrVal = SysAllocString(
      L"xmlns:cp='http://schemas.openxmlformats.org/package/2006/metadata/"
      L"core-properties' xmlns:dc='http://purl.org/dc/elements/1.1/' "
      L"xmlns:dcterms='http://purl.org/dc/terms/' "
      L"xmlns:ep='http://schemas.openxmlformats.org/officeDocument/2006/"
      L"extended-properties'");
  HRESULT hr = d->setProperty(Bstr(L"SelectionNamespaces"), ns);
  VariantClear(&ns);
  Hr(hr);
  return d;
}
struct OfficeField {
  const wchar_t *label;
  const wchar_t *tag;
  const wchar_t *ns;
  bool date;
};
static const OfficeField officeFields[] = {
    {L"标题", L"dc:title", L"http://purl.org/dc/elements/1.1/", false},
    {L"主题", L"dc:subject", L"http://purl.org/dc/elements/1.1/", false},
    {L"作者", L"dc:creator", L"http://purl.org/dc/elements/1.1/", false},
    {L"关键词", L"cp:keywords",
     L"http://schemas.openxmlformats.org/package/2006/metadata/core-properties",
     false},
    {L"备注", L"dc:description", L"http://purl.org/dc/elements/1.1/", false},
    {L"最后保存者", L"cp:lastModifiedBy",
     L"http://schemas.openxmlformats.org/package/2006/metadata/core-properties",
     false},
    {L"文档创建时间（首次编辑）", L"dcterms:created",
     L"http://purl.org/dc/terms/", true},
    {L"文档最后保存时间", L"dcterms:modified", L"http://purl.org/dc/terms/",
     true},
    {L"修订号", L"cp:revision",
     L"http://schemas.openxmlformats.org/package/2006/metadata/core-properties",
     false},
    {L"类别", L"cp:category",
     L"http://schemas.openxmlformats.org/package/2006/metadata/core-properties",
     false}};
bool IsOffice(const std::wstring &p) {
  std::wstring e = PathFindExtensionW(p.c_str());
  CharLowerBuffW(e.data(), static_cast<DWORD>(e.size()));
  return std::set<std::wstring>{L".docx", L".docm", L".dotx", L".dotm",
                                L".xlsx", L".xlsm", L".xltx", L".xltm",
                                L".pptx", L".pptm", L".potx", L".potm",
                                L".ppsx", L".ppsm"}
             .count(e) != 0;
}
static std::wstring NodeText(IXMLDOMNode *n) {
  BSTR s = nullptr;
  Hr(n->get_text(&s));
  std::wstring out = s ? s : L"";
  SysFreeString(s);
  return out;
}
void OfficeRows(FileInfo &info) {
  try {
    auto d = ParseXml(ReadPackagePart(info.path, L"/docProps/core.xml"));
    for (const auto &f : officeFields) {
      ComPtr<IXMLDOMNode> node;
      Hr(d->selectSingleNode(Bstr(std::wstring(L"/cp:coreProperties/") + f.tag),
                             &node));
      auto value = node ? NodeText(node.Get()) : L"";
      if (f.date && !value.empty())
        value = DisplayIso(value);
      info.rows.push_back({f.label, value, f.tag, Kind::Office});
    }
    try {
      auto app = ParseXml(ReadPackagePart(info.path, L"/docProps/app.xml"));
      for (const auto &f : std::vector<std::pair<std::wstring, std::wstring>>{
               {L"总编辑时间（分钟）", L"TotalTime"},
               {L"页数", L"Pages"},
               {L"字数", L"Words"},
               {L"应用程序", L"Application"}}) {
        ComPtr<IXMLDOMNode> node;
        Hr(app->selectSingleNode(Bstr(L"/ep:Properties/ep:" + f.second),
                                 &node));
        if (node)
          info.rows.push_back({f.first, NodeText(node.Get())});
      }
    } catch (const std::exception &) {
    }
  } catch (const std::exception &e) {
    info.rows.push_back(
        {L"Office 元数据状态",
         L"无法读取（加密、损坏或缺少核心属性）：" + Wide(e.what())});
  }
}
void EditOffice(const std::wstring &path, const Row &row,
                const std::wstring &value) {
  const OfficeField *field = nullptr;
  for (const auto &f : officeFields)
    if (row.key == f.tag)
      field = &f;
  if (!field)
    Fail(L"不支持的文档字段");
  auto p = OpenPackage(path);
  auto part = PackagePart(p.Get(), L"/docProps/core.xml");
  ComPtr<IStream> s;
  Hr(part->GetContentStream(&s));
  auto d = ParseXml(ReadStream(s.Get()));
  ComPtr<IXMLDOMNode> node;
  Hr(d->selectSingleNode(Bstr(L"/cp:coreProperties/" + row.key), &node));
  if (!node) {
    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = NODE_ELEMENT;
    Hr(d->createNode(type, Bstr(field->tag), Bstr(field->ns), &node));
    ComPtr<IXMLDOMElement> root;
    Hr(d->get_documentElement(&root));
    ComPtr<IXMLDOMNode> added;
    Hr(root->appendChild(node.Get(), &added));
  }
  if (field->date && value.empty()) {
    ComPtr<IXMLDOMNode> parent, removed;
    Hr(node->get_parentNode(&parent));
    Hr(parent->removeChild(node.Get(), &removed));
  } else
    Hr(node->put_text(Bstr(field->date ? IsoTime(ParseTime(value)) : value)));
  if (field->date && !value.empty()) {
    ComPtr<IXMLDOMElement> el;
    Hr(node.As(&el));
    VARIANT v{};
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"http://www.w3.org/2001/XMLSchema-instance");
    HRESULT hr = el->setAttribute(Bstr(L"xmlns:xsi"), v);
    VariantClear(&v);
    Hr(hr);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"dcterms:W3CDTF");
    hr = el->setAttribute(Bstr(L"xsi:type"), v);
    VariantClear(&v);
    Hr(hr);
  }
  BSTR xml = nullptr;
  Hr(d->get_xml(&xml));
  std::wstring text = xml ? xml : L"";
  SysFreeString(xml);
  auto decl = text.find(L"?>");
  if (text.rfind(L"<?xml", 0) == 0 && decl != std::wstring::npos)
    text.erase(0, decl + 2);
  WriteStream(s.Get(),
              Utf8(L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + text));
  s.Reset();
  TempFile out{UniquePath(path, L".zip")};
  DWORD needed = 0;
  GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0,
                   &needed);
  if (!needed)
    Fail(ErrorText());
  std::vector<BYTE> security(needed);
  WinCheck(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                            security.data(), needed, &needed));
  SavePackage(p.Get(), out.path, security.data());
  part.Reset();
  p.Reset();
  WinCheck(MoveFileExW(out.path.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}
} // namespace fp
