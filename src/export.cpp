#include "internal.h"
#include <map>
namespace fp {
std::wstring CsvCell(const std::wstring &value) {
  std::wstring safe = value;
  size_t i = safe.find_first_not_of(L" \r\n\t");
  if (i != std::wstring::npos && (safe[i] == L'=' || safe[i] == L'+' ||
                                  safe[i] == L'-' || safe[i] == L'@'))
    safe = L"'" + safe;
  else if (!safe.empty() &&
           (safe[0] == L'\t' || safe[0] == L'\r' || safe[0] == L'\n'))
    safe = L"'" + safe;
  std::wstring out = L"\"";
  for (wchar_t c : safe) {
    if (c == L'\"')
      out += L'\"';
    out += c;
  }
  return out + L"\"";
}
std::wstring XmlEscape(const std::wstring &text) {
  std::wstring out;
  for (wchar_t c : text) {
    switch (c) {
    case L'&':
      out += L"&amp;";
      break;
    case L'<':
      out += L"&lt;";
      break;
    case L'>':
      out += L"&gt;";
      break;
    case L'\"':
      out += L"&quot;";
      break;
    default:
      if (c >= 32 || c == L'\n' || c == L'\r' || c == L'\t')
        out += c;
    }
  }
  return out;
}
void WriteAtomic(const std::wstring &path, const std::string &bytes) {
  TempFile t{UniquePath(path, L".tmp")};
  {
    Handle f(CreateFileW(t.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr));
    size_t offset = 0;
    while (offset < bytes.size()) {
      DWORD n = 0, count = static_cast<DWORD>(
                       std::min<size_t>(bytes.size() - offset, 1024 * 1024));
      WinCheck(WriteFile(f.h, bytes.data() + offset, count, &n, nullptr));
      if (!n)
        Fail(L"写入不完整，未替换目标文件");
      offset += n;
    }
    WinCheck(FlushFileBuffers(f.h));
  }
  WinCheck(MoveFileExW(t.path.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}
static std::wstring RowId(const Row &r) { return r.name + L"\x1f" + r.key; }
static std::wstring Xstring(const std::wstring &text) {
  std::wstring encoded;
  auto hex = [](wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
           (c >= L'A' && c <= L'F');
  };
  for (size_t i = 0; i < text.size(); ++i) {
    wchar_t c = text[i];
    if (c == L'_' && i + 6 < text.size() && text[i + 1] == L'x' &&
        text[i + 6] == L'_' && hex(text[i + 2]) && hex(text[i + 3]) &&
        hex(text[i + 4]) && hex(text[i + 5]))
      encoded += L"_x005F_";
    else if ((c < 32 && c != L'\n' && c != L'\t') || c == 0xfffe ||
             c == 0xffff) {
      wchar_t b[9];
      swprintf_s(b, L"_x%04X_", static_cast<unsigned>(c));
      encoded += b;
    } else
      encoded += c;
  }
  return XmlEscape(encoded);
}
static std::wstring Column(size_t n) {
  std::wstring out;
  do {
    out.insert(out.begin(), static_cast<wchar_t>(L'A' + n % 26));
    if (n < 26)
      break;
    n = n / 26 - 1;
  } while (true);
  return out;
}
void Export(const std::wstring &path, const std::vector<FileInfo> &files,
            ExportFormat format) {
  if (files.empty())
    Fail(L"没有可导出的文件");
  auto target = FullPath(path);
  for (const auto &f : files)
    if (_wcsicmp(FullPath(f.path).c_str(), target.c_str()) == 0)
      Fail(L"导出目标不能覆盖列表中的原文件");
  if (format == ExportFormat::Text) {
    std::wstring text;
    for (const auto &f : files) {
      text += L"==================================================\r\n【" +
              FileName(f.path) +
              L"】\r\n==================================================\r\n";
      for (const auto &r : f.rows)
        if (!r.value.empty())
          text += r.name + L"：" + r.value + L"\r\n";
      text += L"\r\n";
    }
    WriteAtomic(path, "\xef\xbb\xbf" + Utf8(text));
    return;
  }
  std::vector<Row> columns;
  std::map<std::wstring, size_t> index;
  for (const auto &f : files)
    for (const auto &r : f.rows) {
      auto key = RowId(r);
      if (!index.count(key)) {
        index[key] = columns.size();
        columns.push_back(r);
      }
    }
  if (columns.size() > 16384 || files.size() >= 1048576)
    Fail(L"超出 Excel 工作表行列限制，请分批导出");
  std::vector<std::vector<std::wstring>> table;
  std::vector<std::wstring> header;
  for (const auto &r : columns)
    header.push_back(r.name + (r.key.empty() ? L"" : L" [" + r.key + L"]"));
  table.push_back(std::move(header));
  for (const auto &f : files) {
    std::vector<std::wstring> line(columns.size());
    for (const auto &r : f.rows)
      line[index.at(RowId(r))] = r.value;
    table.push_back(std::move(line));
  }
  if (format == ExportFormat::Csv) {
    std::wstring text;
    for (const auto &line : table) {
      for (size_t i = 0; i < line.size(); ++i) {
        if (i)
          text += L',';
        text += CsvCell(line[i]);
      }
      text += L"\r\n";
    }
    WriteAtomic(path, "\xef\xbb\xbf" + Utf8(text));
    return;
  }
  std::wstring sheet =
      L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><worksheet "
      L"xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/"
      L"main\"><sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" "
      L"topLeftCell=\"A2\" activePane=\"bottomLeft\" "
      L"state=\"frozen\"/></sheetView></sheetViews><sheetData>";
  for (size_t r = 0; r < table.size(); ++r) {
    sheet += L"<row r=\"" + std::to_wstring(r + 1) + L"\">";
    for (size_t c = 0; c < table[r].size(); ++c) {
      const auto &value = table[r][c];
      if (value.size() > 32767)
        Fail(L"某属性超过 Excel 单元格 32767 字符限制，请使用 TXT/CSV");
      sheet += L"<c r=\"" + Column(c) + std::to_wstring(r + 1) +
               L"\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" +
               Xstring(value) + L"</t></is></c>";
    }
    sheet += L"</row>";
  }
  sheet += L"</sheetData></worksheet>";
  TempFile t{UniquePath(path, L".xlsx")};
  WritePackage(
      t.path,
      {{L"/xl/workbook.xml",
        L"application/"
        L"vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><workbook "
        "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
        "relationships\"><sheets><sheet name=\"文件属性\" sheetId=\"1\" "
        "r:id=\"rId1\"/></sheets></workbook>"},
       {L"/xl/worksheets/sheet1.xml",
        L"application/"
        L"vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml",
        Utf8(sheet)}},
      true);
  WinCheck(MoveFileExW(t.path.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}
} // namespace fp
