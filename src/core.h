#pragma once
#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace fp {
enum class Kind { ReadOnly, Attribute, Time, Property, Office, Version, Owner };
struct Row {
  std::wstring name, value, key;
  Kind kind = Kind::ReadOnly;
  bool editable() const { return kind != Kind::ReadOnly; }
};
struct FileInfo {
  std::wstring path;
  std::vector<Row> rows;
};
struct HashResult {
  std::wstring md5, sha256;
};
using Progress = std::function<void(ULONGLONG, ULONGLONG)>;
std::wstring ErrorText(DWORD error = GetLastError());
std::wstring FullPath(const std::wstring &path);
std::wstring FileName(const std::wstring &path);
std::wstring FormatTime(FILETIME time);
FILETIME ParseTime(const std::wstring &text);
FileInfo Inspect(const std::wstring &path);
HashResult HashFile(const std::wstring &path, bool md5, bool sha256,
                    const std::atomic_bool &cancel, Progress progress = {});
std::vector<std::wstring> ExpandPaths(const std::vector<std::wstring> &paths,
                                      bool recursive,
                                      const std::atomic_bool &cancel,
                                      std::vector<std::wstring> &errors);
// Returns the backup path for content changes; original data is replaced only
// after verification.
std::wstring Edit(const std::wstring &path, const Row &row,
                  const std::wstring &value);
std::wstring CsvCell(const std::wstring &value);
enum class ExportFormat { Text, Csv, Xlsx };
void Export(const std::wstring &path, const std::vector<FileInfo> &files,
            ExportFormat format);
// Native OPC helpers are also used to produce real XLSX files.
struct Part {
  std::wstring name, type;
  std::string content;
};
void WritePackage(const std::wstring &path, const std::vector<Part> &parts,
                  bool workbook = false);
std::string ReadPackagePart(const std::wstring &path, const std::wstring &name);
std::string Utf8(const std::wstring &text);
std::wstring Wide(const std::string &text);
std::wstring XmlEscape(const std::wstring &text);
void WriteAtomic(const std::wstring &path, const std::string &bytes);
} // namespace fp
