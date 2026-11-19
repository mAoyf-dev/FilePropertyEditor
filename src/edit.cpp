#include "internal.h"
#include <aclapi.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlobj.h>
namespace fp {
bool RecoverReplacement(const std::wstring &path, const std::wstring &backup) {
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    return true;
  DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    return false;
  return CopyFileW(backup.c_str(), path.c_str(), TRUE) != FALSE;
}
static void Property(const std::wstring &path, const Row &row,
                     const std::wstring &value) {
  PROPERTYKEY key;
  Hr(PSGetPropertyKeyFromName(row.key.c_str(), &key));
  ComPtr<IPropertyStore> s;
  Hr(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_READWRITE,
                                       IID_PPV_ARGS(&s)));
  ComPtr<IPropertyStoreCapabilities> caps;
  Hr(s.As(&caps));
  if (caps->IsPropertyWritable(key) != S_OK)
    Fail(L"当前文件处理器不允许写入该属性");
  ComPtr<IPropertyDescription> d;
  Hr(PSGetPropertyDescription(key, IID_PPV_ARGS(&d)));
  VARTYPE type;
  Hr(d->GetPropertyType(&type));
  PROPVARIANT v{};
  if (!value.empty()) {
    if (type == VT_FILETIME) {
      auto ft = ParseTime(value);
      Hr(InitPropVariantFromFileTime(&ft, &v));
    } else if (type == (VT_VECTOR | VT_LPWSTR)) {
      std::vector<std::wstring> pieces;
      size_t from = 0;
      for (;;) {
        auto n = value.find(L';', from);
        pieces.push_back(
            value.substr(from, n == std::wstring::npos ? n : n - from));
        if (n == std::wstring::npos)
          break;
        from = n + 1;
      }
      std::vector<PCWSTR> ptrs;
      for (const auto &p : pieces)
        ptrs.push_back(p.c_str());
      Hr(InitPropVariantFromStringVector(ptrs.data(),
                                         static_cast<ULONG>(ptrs.size()), &v));
    } else {
      Hr(InitPropVariantFromString(value.c_str(), &v));
      HRESULT hr = PSCoerceToCanonicalValue(key, &v);
      if (FAILED(hr)) {
        PropVariantClear(&v);
        Hr(hr);
      }
    }
  }
  HRESULT hr = s->SetValue(key, v);
  if (SUCCEEDED(hr))
    hr = s->Commit();
  caps.Reset();
  s.Reset();
  if (FAILED(hr)) {
    PropVariantClear(&v);
    Hr(hr);
  }
  ComPtr<IPropertyStore> verify;
  hr = SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT,
                                         IID_PPV_ARGS(&verify));
  PROPVARIANT actual{};
  if (SUCCEEDED(hr))
    hr = verify->GetValue(key, &actual);
  bool equal = SUCCEEDED(hr) &&
               PropVariantCompareEx(v, actual, PVCU_DEFAULT, PVCF_DEFAULT) == 0;
  PropVariantClear(&v);
  PropVariantClear(&actual);
  if (!equal)
    Fail(L"回读值与写入值不同，未替换原件");
}
std::wstring Edit(const std::wstring &input, const Row &row,
                  const std::wstring &value) {
  if (!row.editable())
    Fail(L"此项是只读信息");
  if (value.size() > 32767)
    Fail(L"属性内容不能超过 32767 字符");
  auto path = FullPath(input);
  DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES)
    Fail(ErrorText());
  if (row.kind == Kind::Attribute) {
    DWORD bit = std::stoul(row.key);
    if (bit != FILE_ATTRIBUTE_READONLY && bit != FILE_ATTRIBUTE_HIDDEN &&
        bit != FILE_ATTRIBUTE_SYSTEM && bit != FILE_ATTRIBUTE_ARCHIVE)
      Fail(L"不支持的属性位");
    bool yes = value == L"是" || value == L"1";
    if (!yes && value != L"否" && value != L"0")
      Fail(L"请输入 是/否 或 1/0");
    DWORD next = yes ? (attrs | bit) : (attrs & ~bit);
    if (next == 0)
      next = FILE_ATTRIBUTE_NORMAL;
    WinCheck(SetFileAttributesW(path.c_str(), next));
    return {};
  }
  if (row.kind == Kind::Time) {
    FILETIME ft = ParseTime(value);
    if (row.key != L"created" && row.key != L"modified" &&
        row.key != L"accessed")
      Fail(L"未知时间字段");
    Handle file(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                            nullptr));
    WinCheck(SetFileTime(file.h, row.key == L"created" ? &ft : nullptr,
                         row.key == L"accessed" ? &ft : nullptr,
                         row.key == L"modified" ? &ft : nullptr));
    return {};
  }
  if (row.kind == Kind::Owner) {
    DWORD a = 0, b = 0;
    SID_NAME_USE use;
    LookupAccountNameW(nullptr, value.c_str(), nullptr, &a, nullptr, &b, &use);
    if (!a)
      Fail(ErrorText());
    std::vector<BYTE> sid(a);
    std::vector<wchar_t> domain(b);
    WinCheck(LookupAccountNameW(nullptr, value.c_str(), sid.data(), &a,
                                domain.data(), &b, &use));
    DWORD error = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION, sid.data(), nullptr, nullptr, nullptr);
    if (error)
      Fail(L"修改所有者失败（需要相应权限，不自动提权）：" + ErrorText(error));
    return {};
  }
  if (attrs & FILE_ATTRIBUTE_READONLY)
    Fail(L"文件只读，请先取消只读再修改内容");
  if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
    Fail(L"不直接改写文件链接内容，请选择实际文件");
  if (attrs & FILE_ATTRIBUTE_ENCRYPTED)
    Fail(L"为避免生成明文临时副本，暂不改写 EFS 加密文件内容");
  Handle original(CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr));
  BY_HANDLE_FILE_INFORMATION before{};
  WinCheck(GetFileInformationByHandle(original.h, &before));
  if (before.nNumberOfLinks > 1)
    Fail(L"文件存在多个硬链接，请先复制为独立文件");
  TempFile stage{UniquePath(path, PathFindExtensionW(path.c_str()))};
  DWORD needed = 0;
  GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0,
                   &needed);
  if (!needed)
    Fail(ErrorText());
  std::vector<BYTE> security(needed);
  WinCheck(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                            security.data(), needed, &needed));
  SECURITY_ATTRIBUTES sa{sizeof(sa), security.data(), FALSE};
  {
    Handle temp(CreateFileW(stage.path.c_str(), GENERIC_WRITE, 0, &sa,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  }
  WinCheck(CopyFileW(path.c_str(), stage.path.c_str(), FALSE));
  WinCheck(SetFileAttributesW(stage.path.c_str(), FILE_ATTRIBUTE_NORMAL));
  if (row.kind == Kind::Office)
    EditOffice(stage.path, row, value);
  else if (row.kind == Kind::Property)
    Property(stage.path, row, value);
  else if (row.kind == Kind::Version)
    EditVersion(stage.path, row, value);
  else
    Fail(L"未知修改类型");
  if (row.kind != Kind::Property) {
    auto info = Inspect(stage.path);
    auto it =
        std::find_if(info.rows.begin(), info.rows.end(), [&](const Row &r) {
          return r.kind == row.kind && r.key == row.key;
        });
    if (it == info.rows.end() || it->value != value)
      Fail(L"回读校验失败，未替换原件");
  }
  PSID sourceOwner = nullptr, stageOwner = nullptr;
  PSECURITY_DESCRIPTOR sourceSd = nullptr, stageSd = nullptr;
  DWORD error =
      GetSecurityInfo(original.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                      &sourceOwner, nullptr, nullptr, nullptr, &sourceSd);
  if (!error)
    error = GetNamedSecurityInfoW(stage.path.c_str(), SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION, &stageOwner,
                                  nullptr, nullptr, nullptr, &stageSd);
  if (!error && !EqualSid(sourceOwner, stageOwner))
    error = SetNamedSecurityInfoW(const_cast<LPWSTR>(stage.path.c_str()),
                                  SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                  sourceOwner, nullptr, nullptr, nullptr);
  LocalFree(sourceSd);
  LocalFree(stageSd);
  if (error)
    Fail(L"无法保留原件所有者，未替换原件：" + ErrorText(error));
  WinCheck(SetFileAttributesW(stage.path.c_str(), attrs));
  Handle current(CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, 0, nullptr));
  BY_HANDLE_FILE_INFORMATION now{};
  WinCheck(GetFileInformationByHandle(current.h, &now));
  if (now.nFileIndexLow != before.nFileIndexLow ||
      now.nFileIndexHigh != before.nFileIndexHigh ||
      now.dwVolumeSerialNumber != before.dwVolumeSerialNumber)
    Fail(L"原文件已被其他程序替换，停止保存");
  auto backup = UniquePath(path, L".bak");
  if (!ReplaceFileW(path.c_str(), stage.path.c_str(), backup.c_str(), 0,
                    nullptr, nullptr)) {
    DWORD replaceError = GetLastError();
    bool restored = RecoverReplacement(path, backup);
    auto retained = stage.path;
    stage.path.clear(); // Partial replacement may leave the original only at
                        // the backup path.
    Fail(L"替换未完成：" + ErrorText(replaceError) +
         (restored ? L"\r\n原路径存在或已从备份恢复。"
                   : L"\r\n原路径未恢复，请保留以下文件并手动恢复。") +
         L"\r\n原路径：" + path + L"\r\n备份（若已生成）：" + backup +
         L"\r\n修改副本：" + retained);
  }
  return backup;
}
} // namespace fp
