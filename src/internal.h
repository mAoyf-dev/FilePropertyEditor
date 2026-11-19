#pragma once
#include "core.h"
#include <algorithm>
#include <msopc.h>
#include <shlwapi.h>
#include <wrl/client.h>
namespace fp {
using Microsoft::WRL::ComPtr;
[[noreturn]] inline void Fail(const std::wstring &text) {
  throw std::runtime_error(Utf8(text));
}
inline void WinCheck(BOOL ok) {
  if (!ok)
    Fail(ErrorText());
}
inline void Hr(HRESULT hr) {
  if (FAILED(hr))
    Fail(ErrorText(static_cast<DWORD>(hr)));
}
struct Handle {
  HANDLE h;
  explicit Handle(HANDLE v) : h(v) {
    if (h == INVALID_HANDLE_VALUE)
      Fail(ErrorText());
  }
  ~Handle() { CloseHandle(h); }
  Handle(const Handle &) = delete;
};
std::wstring UniquePath(const std::wstring &path, const std::wstring &suffix);
struct TempFile {
  std::wstring path;
  ~TempFile() {
    if (!path.empty()) {
      SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(path.c_str());
    }
  }
};
ComPtr<IOpcFactory> OpcFactory();
ComPtr<IOpcPackage> OpenPackage(const std::wstring &path);
ComPtr<IOpcPart> PackagePart(IOpcPackage *package, const std::wstring &name);
void SavePackage(IOpcPackage *package, const std::wstring &path,
                 PSECURITY_DESCRIPTOR security = nullptr);
std::string ReadStream(IStream *stream, ULONGLONG maximum = 16 * 1024 * 1024);
void WriteStream(IStream *stream, const std::string &bytes);
bool IsOffice(const std::wstring &path);
void OfficeRows(FileInfo &info);
void EditOffice(const std::wstring &path, const Row &row,
                const std::wstring &value);
void EditVersion(const std::wstring &path, const Row &row,
                 const std::wstring &value);
bool VersionRows(FileInfo &info);
bool RecoverReplacement(const std::wstring &path, const std::wstring &backup);
} // namespace fp
