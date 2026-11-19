#include "internal.h"
namespace fp {
static const wchar_t *keys[] = {
    L"FileDescription",  L"FileVersion",    L"ProductName",
    L"ProductVersion",   L"LegalCopyright", L"LegalTrademarks",
    L"OriginalFilename", L"CompanyName",    L"InternalName",
    L"Comments",         L"PrivateBuild",   L"SpecialBuild"};
static const wchar_t *labels[] = {L"文件说明",   L"文件版本", L"产品名称",
                                  L"产品版本",   L"版权",     L"合法商标",
                                  L"原始文件名", L"公司名称", L"内部名称",
                                  L"备注",       L"私有生成", L"特殊生成"};
bool VersionRows(FileInfo &info) {
  DWORD unused = 0, n = GetFileVersionInfoSizeW(info.path.c_str(), &unused);
  if (!n || n > 16 * 1024 * 1024)
    return false;
  std::vector<BYTE> b(n);
  if (!GetFileVersionInfoW(info.path.c_str(), 0, n, b.data()))
    return false;
  struct Translation {
    WORD lang, cp;
  };
  Translation *tr = nullptr;
  UINT len = 0;
  VerQueryValueW(b.data(), L"\\VarFileInfo\\Translation",
                 reinterpret_cast<void **>(&tr), &len);
  WORD lang = len >= 4 ? tr[0].lang : 0x409, cp = len >= 4 ? tr[0].cp : 1200;
  VS_FIXEDFILEINFO *f = nullptr;
  UINT fs = 0;
  VerQueryValueW(b.data(), L"\\", reinterpret_cast<void **>(&f), &fs);
  for (size_t i = 0; i < _countof(keys); ++i) {
    wchar_t q[128];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\%s", lang, cp, keys[i]);
    LPWSTR v = nullptr;
    UINT s = 0;
    std::wstring value;
    if (VerQueryValueW(b.data(), q, reinterpret_cast<void **>(&v), &s) && v &&
        s)
      value.assign(v, s - (v[s - 1] == 0));
    if ((i == 1 || i == 3) && f && fs >= sizeof(*f) &&
        f->dwSignature == VS_FFI_SIGNATURE) {
      DWORD ms = i == 1 ? f->dwFileVersionMS : f->dwProductVersionMS,
            ls = i == 1 ? f->dwFileVersionLS : f->dwProductVersionLS;
      wchar_t buf[64];
      swprintf_s(buf, L"%u.%u.%u.%u", HIWORD(ms), LOWORD(ms), HIWORD(ls),
                 LOWORD(ls));
      value = buf;
    }
    info.rows.push_back({labels[i], value, keys[i], Kind::Version});
  }
  wchar_t language[128]{};
  VerLanguageNameW(lang, language, 128);
  info.rows.push_back({L"语言", language});
  return true;
}
struct Node {
  WORD type;
  std::wstring key;
  std::vector<BYTE> value;
  std::vector<Node> children;
};
static size_t Align(size_t n) { return (n + 3) & ~size_t(3); }
static WORD Word(const std::vector<BYTE> &b, size_t n) {
  if (n + 2 > b.size())
    Fail(L"版本资源损坏");
  WORD v;
  memcpy(&v, b.data() + n, 2);
  return v;
}
static Node Read(const std::vector<BYTE> &b, size_t start, size_t limit,
                 unsigned depth = 0) {
  if (depth > 16 || start + 6 > limit)
    Fail(L"版本资源结构无效");
  WORD len = Word(b, start), vl = Word(b, start + 2), t = Word(b, start + 4);
  if (len < 6 || start + len > limit)
    Fail(L"版本资源长度无效");
  size_t end = start + len, p = start + 6;
  std::wstring key;
  while (p + 2 <= end && Word(b, p) != 0) {
    key += static_cast<wchar_t>(Word(b, p));
    p += 2;
  }
  if (p + 2 > end)
    Fail(L"版本资源键名无效");
  p = Align(p + 2);
  size_t bytes = t ? size_t(vl) * 2 : vl;
  if (p + bytes > end)
    Fail(L"版本资源数据无效");
  Node out{t, key, {}, {}};
  out.value.assign(b.begin() + p, b.begin() + p + bytes);
  p = Align(p + bytes);
  while (p + 6 <= end) {
    if (!Word(b, p))
      break;
    out.children.push_back(Read(b, p, end, depth + 1));
    p = Align(p + Word(b, p));
  }
  return out;
}
static void Put(std::vector<BYTE> &b, size_t p, WORD v) {
  memcpy(b.data() + p, &v, 2);
}
static std::vector<BYTE> Save(const Node &n) {
  std::vector<BYTE> b(6);
  for (wchar_t c : n.key) {
    b.push_back(static_cast<BYTE>(c));
    b.push_back(static_cast<BYTE>(c >> 8));
  }
  b.push_back(0);
  b.push_back(0);
  b.resize(Align(b.size()));
  b.insert(b.end(), n.value.begin(), n.value.end());
  for (const auto &c : n.children) {
    b.resize(Align(b.size()));
    auto v = Save(c);
    b.insert(b.end(), v.begin(), v.end());
  }
  if (b.size() > 65535 || n.value.size() > 65535)
    Fail(L"版本资源过大");
  Put(b, 0, static_cast<WORD>(b.size()));
  Put(b, 2, static_cast<WORD>(n.type ? n.value.size() / 2 : n.value.size()));
  Put(b, 4, n.type);
  return b;
}
static void Update(Node &n, const std::wstring &key, const std::wstring &text) {
  if (n.key == L"StringFileInfo") {
    for (auto &table : n.children) {
      auto it = std::find_if(table.children.begin(), table.children.end(),
                             [&](const Node &x) { return x.key == key; });
      if (it == table.children.end()) {
        table.children.push_back({1, key, {}, {}});
        it = table.children.end() - 1;
      }
      it->value.resize((text.size() + 1) * 2);
      memcpy(it->value.data(), text.c_str(), it->value.size());
    }
  } else
    for (auto &c : n.children)
      Update(c, key, text);
}
struct Resource {
  std::wstring name;
  WORD id = 0, lang = 0;
  bool integer = false;
  std::vector<BYTE> bytes;
};
static BOOL CALLBACK Lang(HMODULE m, LPCWSTR t, LPCWSTR n, WORD l, LONG_PTR p) {
  auto &v = *reinterpret_cast<std::vector<Resource> *>(p);
  auto r = FindResourceExW(m, t, n, l);
  if (!r)
    return FALSE;
  DWORD size = SizeofResource(m, r);
  auto b = static_cast<BYTE *>(LockResource(LoadResource(m, r)));
  if (!b || !size || size > 16 * 1024 * 1024)
    return FALSE;
  Resource out;
  if (IS_INTRESOURCE(n)) {
    out.integer = true;
    out.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(n));
  } else
    out.name = n;
  out.lang = l;
  out.bytes.assign(b, b + size);
  v.push_back(std::move(out));
  return TRUE;
}
static BOOL CALLBACK Name(HMODULE m, LPCWSTR t, LPWSTR n, LONG_PTR p) {
  return EnumResourceLanguagesW(m, t, n, Lang, p);
}
void EditVersion(const std::wstring &path, const Row &row,
                 const std::wstring &value) {
  if (std::none_of(std::begin(keys), std::end(keys),
                   [&](const wchar_t *k) { return row.key == k; }))
    Fail(L"不支持的版本字段");
  DWORD ms = 0, ls = 0;
  bool number = row.key == L"FileVersion" || row.key == L"ProductVersion";
  if (number) {
    unsigned a, b, c, d;
    int used = 0;
    if (swscanf_s(value.c_str(), L"%u.%u.%u.%u%n", &a, &b, &c, &d, &used) !=
            4 ||
        static_cast<size_t>(used) != value.size() || a > 65535 || b > 65535 ||
        c > 65535 || d > 65535)
      Fail(L"版本号应为四个 0–65535 整数，例如 2.0.0.0");
    ms = (a << 16) | b;
    ls = (c << 16) | d;
  }
  HMODULE module =
      LoadLibraryExW(path.c_str(), nullptr,
                     LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module)
    Fail(ErrorText());
  std::vector<Resource> resources;
  BOOL ok = EnumResourceNamesW(module, RT_VERSION, Name,
                               reinterpret_cast<LONG_PTR>(&resources));
  FreeLibrary(module);
  if (!ok || resources.empty())
    Fail(L"无法完整读取版本资源");
  for (auto &r : resources) {
    auto root = Read(r.bytes, 0, r.bytes.size());
    Update(root, row.key, value);
    if (number) {
      if (root.value.size() < sizeof(VS_FIXEDFILEINFO))
        Fail(L"缺少固定版本信息");
      VS_FIXEDFILEINFO f;
      memcpy(&f, root.value.data(), sizeof(f));
      if (f.dwSignature != VS_FFI_SIGNATURE)
        Fail(L"固定版本信息签名无效");
      if (row.key == L"FileVersion") {
        f.dwFileVersionMS = ms;
        f.dwFileVersionLS = ls;
      } else {
        f.dwProductVersionMS = ms;
        f.dwProductVersionLS = ls;
      }
      memcpy(root.value.data(), &f, sizeof(f));
    }
    r.bytes = Save(root);
  }
  HANDLE update = BeginUpdateResourceW(path.c_str(), FALSE);
  if (!update)
    Fail(ErrorText());
  for (auto &r : resources)
    if (!UpdateResourceW(update, RT_VERSION,
                         r.integer ? MAKEINTRESOURCEW(r.id) : r.name.c_str(),
                         r.lang, r.bytes.data(),
                         static_cast<DWORD>(r.bytes.size()))) {
      DWORD e = GetLastError();
      EndUpdateResourceW(update, TRUE);
      Fail(ErrorText(e));
    }
  WinCheck(EndUpdateResourceW(update, FALSE));
}
} // namespace fp
