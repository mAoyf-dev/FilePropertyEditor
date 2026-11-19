#include "internal.h"
#include "resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <memory>
#include <set>
#include <shellapi.h>
#include <shobjidl.h>
#include <thread>

namespace {
using namespace fp;
enum {
  Add = 200,
  Folder,
  Remove,
  Clear,
  Refresh,
  Copy,
  Save,
  Hash,
  Cancel,
  EditOne,
  Filter,
  Md5,
  Sha,
  Recursive,
  OnlyEdit,
  Files,
  Props,
  Detail,
  Status,
  Bar,
  Help,
  Log
};
enum { DataMessage = WM_APP + 1, ProgressMessage, DoneMessage, LogMessage };
const wchar_t *ClassName = L"FilePropertyEditorV2";
HINSTANCE instance;
HWND window, fileList, propList, detail, status, bar, search;
HFONT font;
int dpi = 96, active = -1;
bool busy = false, closing = false, rebuilding = false;
std::vector<FileInfo> files;
std::vector<size_t> visibleRows;
std::thread worker;
std::atomic_bool canceled{false};
std::wstring jobLog;
std::vector<HWND> commands;
struct Data {
  FileInfo info;
  int replace = -1;
};
struct ProgressData {
  std::wstring text;
  int percent;
};
int Scale(int n) { return MulDiv(n, dpi, 96); }
HWND Control(int id) { return GetDlgItem(window, id); }
std::wstring Text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(n + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}
void StatusText(const std::wstring &s) { SetWindowTextW(status, s.c_str()); }
bool Checked(int id) {
  return SendMessageW(Control(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}
template <class T> void Post(UINT message, T *value) {
  if (!PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(value)))
    delete value;
}
void Progress(const std::wstring &text, int percent = 0) {
  Post(ProgressMessage, new ProgressData{text, percent});
}
void LogLine(const std::wstring &s) { Post(LogMessage, new std::wstring(s)); }
void ShowError(const std::exception &e) {
  MessageBoxW(window, Wide(e.what()).c_str(), L"操作未完成",
              MB_OK | MB_ICONERROR);
}
std::vector<int> Selected() {
  std::vector<int> out;
  int i = -1;
  while ((i = ListView_GetNextItem(fileList, i, LVNI_SELECTED)) != -1)
    out.push_back(i);
  return out;
}
std::vector<int> Targets() {
  auto out = Selected();
  if (out.empty())
    for (int i = 0; i < static_cast<int>(files.size()); ++i)
      out.push_back(i);
  return out;
}
void CopyText(const std::wstring &text) {
  if (!OpenClipboard(window))
    Fail(ErrorText());
  if (!EmptyClipboard()) {
    CloseClipboard();
    Fail(ErrorText());
  }
  SIZE_T size = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!h) {
    CloseClipboard();
    throw std::bad_alloc();
  }
  void *ptr = GlobalLock(h);
  if (!ptr) {
    GlobalFree(h);
    CloseClipboard();
    Fail(L"剪贴板内存不可用");
  }
  memcpy(ptr, text.c_str(), size);
  GlobalUnlock(h);
  if (!SetClipboardData(CF_UNICODETEXT, h)) {
    GlobalFree(h);
    CloseClipboard();
    Fail(ErrorText());
  }
  CloseClipboard();
  StatusText(L"已复制到剪贴板");
}
void CopyProperties() {
  std::wstring out;
  for (int i : Targets()) {
    out += L"【" + FileName(files[i].path) + L"】\r\n";
    for (const auto &r : files[i].rows)
      if (!r.value.empty())
        out += r.name + L"：" + r.value + L"\r\n";
    out += L"\r\n";
  }
  if (!out.empty())
    CopyText(out);
}
const Row *CurrentRow() {
  int n = ListView_GetNextItem(propList, -1, LVNI_SELECTED);
  if (active < 0 || active >= static_cast<int>(files.size()) || n < 0 ||
      n >= static_cast<int>(visibleRows.size()))
    return nullptr;
  return &files[active].rows[visibleRows[n]];
}
void DetailRow() {
  const auto *r = CurrentRow();
  SetWindowTextW(detail, r ? r->value.c_str()
                           : L"选择属性可在此查看完整内容；双击“可修改”项编辑。"
                             L"右键或 Ctrl+C 复制属性值。");
  EnableWindow(Control(EditOne), !busy && r && r->editable());
}
std::wstring Lower(std::wstring v) {
  if (!v.empty())
    CharLowerBuffW(v.data(), static_cast<DWORD>(v.size()));
  return v;
}
void ShowProperties() {
  rebuilding = true;
  SendMessageW(propList, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(propList);
  visibleRows.clear();
  if (active >= 0 && active < static_cast<int>(files.size())) {
    auto filter = Lower(Text(search));
    for (size_t i = 0; i < files[active].rows.size(); ++i) {
      const auto &r = files[active].rows[i];
      if (Checked(OnlyEdit) && !r.editable())
        continue;
      if (!filter.empty() &&
          Lower(r.name + L" " + r.value + L" " + r.key).find(filter) ==
              std::wstring::npos)
        continue;
      visibleRows.push_back(i);
      LVITEMW item{};
      item.mask = LVIF_TEXT;
      item.iItem = ListView_GetItemCount(propList);
      item.pszText = const_cast<LPWSTR>(r.name.c_str());
      int row = ListView_InsertItem(propList, &item);
      std::wstring value = r.value.empty() ? L"（空）" : r.value;
      ListView_SetItemText(propList, row, 1, const_cast<LPWSTR>(value.c_str()));
      ListView_SetItemText(
          propList, row, 2,
          const_cast<LPWSTR>(r.editable() ? L"可修改" : L"只读"));
    }
  }
  SendMessageW(propList, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(propList, nullptr, TRUE);
  rebuilding = false;
  DetailRow();
}
void AddFileItem(const FileInfo &f) {
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = ListView_GetItemCount(fileList);
  auto name = FileName(f.path);
  item.pszText = name.data();
  int n = ListView_InsertItem(fileList, &item);
  ListView_SetItemText(fileList, n, 1, const_cast<LPWSTR>(L"已载入"));
}
void Busy(bool value) {
  busy = value;
  for (HWND h : commands)
    EnableWindow(h, !value);
  EnableWindow(Control(Cancel), value);
  EnableWindow(Control(Log), !value && !jobLog.empty());
  if (value) {
    jobLog.clear();
    SendMessageW(bar, PBM_SETPOS, 0, 0);
  }
  DetailRow();
}
void Start(std::function<void()> work) {
  if (busy)
    return;
  if (worker.joinable())
    worker.join();
  canceled = false;
  Busy(true);
  try {
    worker = std::thread([work = std::move(work)]() {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      try {
        Hr(hr);
        work();
      } catch (const std::exception &e) {
        LogLine(L"失败：" + Wide(e.what()));
      } catch (...) {
        LogLine(L"失败：未知错误");
      }
      if (SUCCEEDED(hr))
        CoUninitialize();
      PostMessageW(window, DoneMessage, 0, 0);
    });
  } catch (...) {
    Busy(false);
    throw;
  }
}
void UpdateHashes(FileInfo &info, bool md5, bool sha) {
  if (!md5 && !sha)
    return;
  auto hash = HashFile(
      info.path, md5, sha, canceled, [&](ULONGLONG done, ULONGLONG total) {
        int percent =
            total ? static_cast<int>(static_cast<double>(done) * 1000 / total)
                  : 1000;
        Progress(L"计算哈希：" + FileName(info.path) + L"  " +
                     std::to_wstring(done) + L" / " + std::to_wstring(total) +
                     L" 字节",
                 percent);
      });
  for (auto &r : info.rows) {
    if (r.name == L"MD5" && md5)
      r.value = hash.md5;
    if (r.name == L"SHA-256" && sha)
      r.value = hash.sha256;
  }
}
void Load(std::vector<std::wstring> paths) {
  if (busy || paths.empty())
    return;
  bool recurse = Checked(Recursive), md5 = Checked(Md5), sha = Checked(Sha);
  std::set<std::wstring> known;
  for (const auto &f : files)
    known.insert(Lower(f.path));
  Start([paths = std::move(paths), recurse, md5, sha,
         known = std::move(known)]() {
    Progress(L"正在收集文件…");
    std::vector<std::wstring> errors;
    auto paths2 = ExpandPaths(paths, recurse, canceled, errors);
    for (const auto &e : errors)
      LogLine(e);
    size_t count = 0;
    for (const auto &p : paths2) {
      if (canceled)
        break;
      if (known.count(Lower(p)))
        continue;
      Progress(L"读取 " + std::to_wstring(++count) + L" / " +
               std::to_wstring(paths2.size()) + L"：" + FileName(p));
      try {
        auto info = Inspect(p);
        try {
          UpdateHashes(info, md5, sha);
        } catch (const std::exception &e) {
          LogLine(p + L"：" + Wide(e.what()));
        }
        Post(DataMessage, new Data{std::move(info), -1});
      } catch (const std::exception &e) {
        LogLine(p + L"：" + Wide(e.what()));
      }
    }
  });
}
void AddFiles() {
  std::vector<wchar_t> b(65536);
  OPENFILENAMEW o{};
  o.lStructSize = sizeof(o);
  o.hwndOwner = window;
  o.lpstrFile = b.data();
  o.nMaxFile = static_cast<DWORD>(b.size());
  o.lpstrFilter = L"所有文件\0*.*\0\0";
  o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
            OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&o))
    return;
  std::vector<std::wstring> paths;
  std::wstring first = b.data();
  auto next = b.data() + first.size() + 1;
  if (!*next)
    paths.push_back(first);
  else
    for (; *next; next += wcslen(next) + 1)
      paths.push_back(first + L"\\" + next);
  Load(std::move(paths));
}
void AddFolder() {
  ComPtr<IFileOpenDialog> d;
  Hr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                      IID_PPV_ARGS(&d)));
  DWORD flags;
  Hr(d->GetOptions(&flags));
  Hr(d->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                   FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR));
  if (d->Show(window) == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    return;
  ComPtr<IShellItemArray> items;
  Hr(d->GetResults(&items));
  DWORD n = 0;
  Hr(items->GetCount(&n));
  std::vector<std::wstring> paths;
  for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> item;
    Hr(items->GetItemAt(i, &item));
    PWSTR p = nullptr;
    Hr(item->GetDisplayName(SIGDN_FILESYSPATH, &p));
    paths.push_back(p);
    CoTaskMemFree(p);
  }
  Load(std::move(paths));
}
void Reload(bool hashes) {
  if (files.empty() || busy)
    return;
  bool md5 = hashes && Checked(Md5), sha = hashes && Checked(Sha);
  if (hashes && !md5 && !sha) {
    MessageBoxW(window, L"请先勾选 MD5 或 SHA-256。哈希默认关闭。", L"按需计算",
                MB_OK);
    return;
  }
  std::vector<std::pair<int, std::wstring>> paths;
  for (int i : Targets())
    paths.push_back({i, files[i].path});
  Start([paths = std::move(paths), md5, sha]() {
    for (const auto &p : paths) {
      if (canceled)
        break;
      Progress(L"刷新：" + FileName(p.second));
      try {
        auto info = Inspect(p.second);
        try {
          UpdateHashes(info, md5, sha);
        } catch (const std::exception &e) {
          LogLine(p.second + L"：" + Wide(e.what()));
        }
        Post(DataMessage, new Data{std::move(info), p.first});
      } catch (const std::exception &e) {
        LogLine(p.second + L"：" + Wide(e.what()));
      }
    }
  });
}
struct EditDialog {
  Row row;
  std::wstring value, path;
  size_t selected;
  bool batch = false;
};
INT_PTR CALLBACK EditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  auto *ctx = reinterpret_cast<EditDialog *>(GetWindowLongPtrW(h, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    ctx = reinterpret_cast<EditDialog *>(l);
    SetWindowLongPtrW(h, DWLP_USER, l);
    SetDlgItemTextW(h, IDC_FIELD,
                    (ctx->row.name + L"  [" + ctx->row.key + L"]").c_str());
    SetDlgItemTextW(h, IDC_TARGET, ctx->path.c_str());
    SetDlgItemTextW(h, IDC_VALUE, ctx->value.c_str());
    SendDlgItemMessageW(h, IDC_VALUE, EM_SETLIMITTEXT, 32767, 0);
    EnableWindow(GetDlgItem(h, IDC_BATCH), ctx->selected > 1);
    CheckDlgButton(h, IDC_BATCH,
                   ctx->selected > 1 ? BST_CHECKED : BST_UNCHECKED);
    bool content = ctx->row.kind == Kind::Office ||
                   ctx->row.kind == Kind::Property ||
                   ctx->row.kind == Kind::Version;
    ShowWindow(GetDlgItem(h, IDC_ACK), content ? SW_SHOW : SW_HIDE);
    std::wstring hint =
        content ? L"内容修改先在副本校验，成功后替换并在原目录生成 .bak "
                  L"备份。关闭正在使用此文件的软件。多值字段用英文分号分隔。"
                : L"此操作直接修改文件系统属性，不改文件内容。";
    if (ctx->row.kind == Kind::Time || ctx->row.key == L"dcterms:created" ||
        ctx->row.key == L"dcterms:modified" ||
        ctx->row.key.find(L"Date") != std::wstring::npos)
      hint += L" 时间输入：YYYY-MM-DD HH:MM:SS（本地时间）。";
    if (ctx->row.kind == Kind::Attribute)
      hint += L" 输入 是/否 或 1/0。";
    if (ctx->row.kind == Kind::Owner)
      hint = L"输入账户名，例如 "
             L"计算机名\\用户名。需要相应权限；不自动提权。所有者变更不生成内容"
             L"备份，可能影响文件访问。";
    SetDlgItemTextW(h, IDC_HINT, hint.c_str());
    return TRUE;
  }
  if (msg == WM_COMMAND) {
    if (LOWORD(w) == IDCANCEL) {
      EndDialog(h, IDCANCEL);
      return TRUE;
    }
    if (LOWORD(w) == IDOK) {
      bool content = ctx->row.kind == Kind::Office ||
                     ctx->row.kind == Kind::Property ||
                     ctx->row.kind == Kind::Version;
      if (content && IsDlgButtonChecked(h, IDC_ACK) != BST_CHECKED) {
        MessageBoxW(h, L"请先确认已了解修改对哈希和数字签名的影响。",
                    L"保存修改", MB_OK);
        return TRUE;
      }
      ctx->value = Text(GetDlgItem(h, IDC_VALUE));
      ctx->batch = IsDlgButtonChecked(h, IDC_BATCH) == BST_CHECKED;
      try {
        if (ctx->row.kind == Kind::Time)
          ParseTime(ctx->value);
      } catch (const std::exception &e) {
        MessageBoxW(h, Wide(e.what()).c_str(), L"时间无效",
                    MB_OK | MB_ICONERROR);
        return TRUE;
      }
      EndDialog(h, IDOK);
      return TRUE;
    }
  }
  return FALSE;
}
void EditRow() {
  auto *row = CurrentRow();
  if (busy || !row || !row->editable())
    return;
  auto selection = Selected();
  EditDialog ctx{*row, row->value, files[active].path, selection.size()};
  if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_EDIT), window, EditProc,
                      reinterpret_cast<LPARAM>(&ctx)) != IDOK)
    return;
  std::vector<std::pair<int, std::wstring>> targets;
  if (ctx.batch) {
    for (int i : selection)
      targets.push_back({i, files[i].path});
  } else
    targets.push_back({active, files[active].path});
  Start([targets = std::move(targets), row = ctx.row, value = ctx.value]() {
    for (const auto &target : targets) {
      if (canceled)
        break;
      Progress(L"保存：" + FileName(target.second));
      try {
        auto fresh = Inspect(target.second);
        auto it = std::find_if(fresh.rows.begin(), fresh.rows.end(),
                               [&](const Row &r) {
                                 return r.kind == row.kind && r.key == row.key;
                               });
        if (it == fresh.rows.end() || !it->editable())
          Fail(L"此文件不支持修改该字段");
        auto backup = Edit(target.second, *it, value);
        LogLine(L"成功：" + target.second +
                (backup.empty() ? L"" : L"\r\n  备份：" + backup));
        try {
          Post(DataMessage, new Data{Inspect(target.second), target.first});
        } catch (const std::exception &e) {
          LogLine(L"已保存，但刷新失败：" + Wide(e.what()));
        }
      } catch (const std::exception &e) {
        LogLine(L"失败：" + target.second + L"\r\n  " + Wide(e.what()));
      }
    }
  });
}
void RemoveFiles() {
  if (busy)
    return;
  auto selected = Selected();
  for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
    files.erase(files.begin() + *it);
    ListView_DeleteItem(fileList, *it);
  }
  active = -1;
  ShowProperties();
  StatusText(L"已移除选中项（未删除磁盘文件）");
}
void SaveExport() {
  if (busy || files.empty())
    return;
  std::vector<wchar_t> b(32768);
  auto name = files.size() == 1 ? FileName(files[0].path)
                                : FileName(files[0].path) + L"_等" +
                                      std::to_wstring(files.size()) + L"个文件";
  name += L"_属性.txt";
  wcscpy_s(b.data(), b.size(), name.c_str());
  OPENFILENAMEW o{};
  o.lStructSize = sizeof(o);
  o.hwndOwner = window;
  o.lpstrFile = b.data();
  o.nMaxFile = static_cast<DWORD>(b.size());
  o.lpstrFilter = L"文本清单 (*.txt)\0*.txt\0CSV 表格 (*.csv)\0*.csv\0Excel "
                  L"工作簿 (*.xlsx)\0*.xlsx\0\0";
  o.nFilterIndex = 1;
  o.lpstrDefExt = L"txt";
  o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&o))
    return;
  ExportFormat format = o.nFilterIndex == 2   ? ExportFormat::Csv
                        : o.nFilterIndex == 3 ? ExportFormat::Xlsx
                                              : ExportFormat::Text;
  std::wstring path = b.data(), ext = o.nFilterIndex == 2   ? L".csv"
                                      : o.nFilterIndex == 3 ? L".xlsx"
                                                            : L".txt";
  std::wstring old = PathFindExtensionW(path.c_str());
  if (_wcsicmp(old.c_str(), ext.c_str()) != 0) {
    if (_wcsicmp(old.c_str(), L".txt") == 0 ||
        _wcsicmp(old.c_str(), L".csv") == 0 ||
        _wcsicmp(old.c_str(), L".xlsx") == 0)
      path.resize(path.size() - old.size());
    path += ext;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        MessageBoxW(window, (L"替换现有导出文件？\r\n" + path).c_str(), L"导出",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
      return;
  }
  auto snapshot = files;
  Start([path, format, snapshot = std::move(snapshot)]() {
    Progress(L"正在导出…");
    Export(path, snapshot, format);
    LogLine(L"导出成功：" + path);
  });
}
LRESULT CALLBACK ListKeys(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                          DWORD_PTR) {
  if (m == WM_KEYDOWN) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    try {
      if (ctrl && w == 'C') {
        if (h == fileList)
          CopyProperties();
        else if (auto *r = CurrentRow())
          CopyText(r->value);
        return 0;
      }
      if (ctrl && w == 'A' && h == fileList) {
        ListView_SetItemState(fileList, -1, LVIS_SELECTED, LVIS_SELECTED);
        return 0;
      }
      if (w == VK_DELETE && h == fileList) {
        RemoveFiles();
        return 0;
      }
      if (w == VK_RETURN && h == propList) {
        EditRow();
        return 0;
      }
    } catch (const std::exception &e) {
      ShowError(e);
    }
  }
  return DefSubclassProc(h, m, w, l);
}
HWND Make(const wchar_t *cls, const wchar_t *text, DWORD style, int id,
          DWORD ex = 0) {
  HWND h = CreateWindowExW(
      ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
  if (!h)
    Fail(ErrorText());
  SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  return h;
}
void Move(HWND h, int x, int y, int w, int height) {
  MoveWindow(h, Scale(x), Scale(y), Scale(std::max(w, 1)),
             Scale(std::max(height, 1)), TRUE);
}
void Layout() {
  RECT r;
  GetClientRect(window, &r);
  int w = MulDiv(r.right, 96, dpi), h = MulDiv(r.bottom, 96, dpi);
  int left = std::min(350, std::max(255, w / 3));
  int x = 14;
  for (const auto &p : std::vector<std::pair<int, int>>{{Add, 94},
                                                        {Folder, 94},
                                                        {Remove, 86},
                                                        {Clear, 86},
                                                        {Refresh, 72},
                                                        {Copy, 94},
                                                        {Save, 94},
                                                        {Help, 64},
                                                        {Log, 86}}) {
    Move(Control(p.first), x, 14, p.second, 30);
    x += p.second + 7;
  }
  Move(Control(Md5), 14, 55, 65, 25);
  Move(Control(Sha), 84, 55, 98, 25);
  Move(Control(Hash), 190, 52, 100, 29);
  Move(Control(Cancel), 298, 52, 72, 29);
  Move(Control(Recursive), 385, 55, 165, 25);
  Move(Control(OnlyEdit), w - 168, 94, 154, 25);
  Move(search, left + 28, 91, w - left - 210, 28);
  Move(fileList, 14, 91, left, h - 156);
  int top = 128, detailH = std::max(112, std::min(170, h / 4));
  Move(propList, left + 28, top, w - left - 42, h - top - detailH - 86);
  Move(Control(EditOne), left + 28, h - detailH - 72, 106, 29);
  Move(Control(10000), left + 145, h - detailH - 69, w - left - 158, 24);
  Move(detail, left + 28, h - detailH - 36, w - left - 42, detailH);
  Move(status, 14, h - 30, w - 250, 22);
  Move(bar, w - 225, h - 27, 211, 17);
  ListView_SetColumnWidth(fileList, 0, Scale(left - 96));
  ListView_SetColumnWidth(fileList, 1, Scale(90));
  ListView_SetColumnWidth(propList, 0, Scale(215));
  ListView_SetColumnWidth(propList, 1, Scale(std::max(140, w - left - 350)));
  ListView_SetColumnWidth(propList, 2, Scale(82));
}
void CreateControls() {
  HDC dc = GetDC(window);
  dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(window, dc);
  font =
      CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
  for (const auto &p :
       std::vector<std::pair<int, const wchar_t *>>{{Add, L"添加文件…"},
                                                    {Folder, L"添加文件夹…"},
                                                    {Remove, L"移除选中"},
                                                    {Clear, L"清空列表"},
                                                    {Refresh, L"刷新"},
                                                    {Copy, L"复制属性"},
                                                    {Save, L"导出…"},
                                                    {Hash, L"计算哈希"},
                                                    {Cancel, L"取消任务"},
                                                    {EditOne, L"修改此项…"},
                                                    {Help, L"说明"},
                                                    {Log, L"操作结果…"}}) {
    auto h = Make(L"BUTTON", p.second, WS_TABSTOP | BS_PUSHBUTTON, p.first);
    if (p.first != Cancel && p.first != Help && p.first != Copy &&
        p.first != Log)
      commands.push_back(h);
  }
  for (const auto &p : std::vector<std::pair<int, const wchar_t *>>{
           {Md5, L"MD5"},
           {Sha, L"SHA-256"},
           {Recursive, L"包含子文件夹"},
           {OnlyEdit, L"仅显示可修改项"}}) {
    auto h = Make(L"BUTTON", p.second, WS_TABSTOP | BS_AUTOCHECKBOX, p.first);
    if (p.first != OnlyEdit)
      commands.push_back(h);
  }
  fileList = Make(WC_LISTVIEWW, L"文件列表",
                  WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, Files,
                  WS_EX_CLIENTEDGE);
  propList = Make(WC_LISTVIEWW, L"属性列表",
                  WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                  Props, WS_EX_CLIENTEDGE);
  for (HWND h : {fileList, propList}) {
    ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT |
                                             LVS_EX_DOUBLEBUFFER |
                                             LVS_EX_GRIDLINES | LVS_EX_INFOTIP);
    SetWindowSubclass(h, ListKeys, 1, 0);
  }
  for (const auto &p : std::vector<std::pair<HWND, std::vector<std::wstring>>>{
           {fileList, {L"文件名", L"状态"}},
           {propList, {L"属性", L"内容", L"编辑能力"}}}) {
    for (int i = 0; i < static_cast<int>(p.second.size()); ++i) {
      LVCOLUMNW col{};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = const_cast<LPWSTR>(p.second[i].c_str());
      col.cx = Scale(150);
      ListView_InsertColumn(p.first, i, &col);
    }
  }
  search =
      Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Filter, WS_EX_CLIENTEDGE);
  SendMessageW(search, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"搜索属性名称、内容或字段标识"));
  detail = Make(L"EDIT", L"",
                WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                    WS_VSCROLL,
                Detail, WS_EX_CLIENTEDGE);
  Make(L"STATIC", L"完整内容（可选中复制）", 0, 10000);
  status = Make(L"STATIC", L"拖入文件或文件夹开始；勾选哈希后添加会自动计算。",
                SS_LEFTNOWORDWRAP, Status);
  bar = Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, Bar);
  SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
  EnableWindow(Control(Cancel), FALSE);
  EnableWindow(Control(EditOne), FALSE);
  EnableWindow(Control(Log), FALSE);
  DragAcceptFiles(window, TRUE);
  Layout();
  DetailRow();
}
LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  try {
    switch (msg) {
    case WM_CREATE:
      window = h;
      CreateControls();
      return 0;
    case WM_SIZE:
      Layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto *p = reinterpret_cast<MINMAXINFO *>(l);
      p->ptMinTrackSize = {Scale(960), Scale(640)};
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(w);
      if (id == Filter && HIWORD(w) == EN_CHANGE) {
        ShowProperties();
        return 0;
      }
      if (id == OnlyEdit) {
        ShowProperties();
        return 0;
      }
      if (id == Help) {
        MessageBoxW(
            h,
            L"添加、拖放或命令行载入文件。Ctrl+A 全选，Ctrl+C 复制属性，Delete "
            L"只移除列表项。\r\n\r\n哈希默认关闭；勾选后添加会自动计算，也可选"
            L"择文件后点击计算哈希。大文件分块读取，可取消，无 1 GiB "
            L"限制。\r\n\r\n属性栏可搜索、滚动；下方显示完整值。双击可修改项，"
            L"支持批量应用。内容写入生成 .bak "
            L"备份；文件时间、标志、所有者直接修改。文档时间与文件时间、照片拍"
            L"摄时间是不同字段。\r\n\r\nOffice Open XML "
            L"支持标题、作者、创建时间等；图片 "
            L"EXIF、音频、旧版文档取决于系统处理器。无可写处理器的项为只读。类"
            L"型识别仅覆盖常见文件头，签名验证离线且不检查目录签名。\r\n\r\n导"
            L"出全部文件为 TXT、CSV 或真正 XLSX。CSV 会保护公式文本；用 XLSX "
            L"可避免 Excel 自动改写版本号和摘要。",
            L"文件属性查改器 V2.0", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (id == Copy) {
        CopyProperties();
        return 0;
      }
      if (id == Cancel) {
        canceled = true;
        StatusText(L"正在取消，请等待当前文件操作完成…");
        return 0;
      }
      if (id == Log) {
        CopyText(jobLog);
        MessageBoxW(h,
                    jobLog.size() > 15000 ? L"完整操作结果已复制到剪贴板。"
                                          : jobLog.c_str(),
                    L"逐文件操作结果（已复制）", MB_OK);
        return 0;
      }
      if (busy)
        return 0;
      switch (id) {
      case Add:
        AddFiles();
        break;
      case Folder:
        AddFolder();
        break;
      case Remove:
        RemoveFiles();
        break;
      case Clear:
        files.clear();
        ListView_DeleteAllItems(fileList);
        active = -1;
        ShowProperties();
        StatusText(L"列表已清空");
        break;
      case Refresh:
        Reload(false);
        break;
      case Save:
        SaveExport();
        break;
      case Hash:
        Reload(true);
        break;
      case EditOne:
        EditRow();
        break;
      }
      return 0;
    }
    case WM_NOTIFY: {
      auto *n = reinterpret_cast<NMHDR *>(l);
      if (n->hwndFrom == fileList && n->code == LVN_ITEMCHANGED &&
          !rebuilding) {
        auto *p = reinterpret_cast<NMLISTVIEW *>(l);
        if ((p->uNewState & LVIS_SELECTED) && (p->uChanged & LVIF_STATE)) {
          active = p->iItem;
          ShowProperties();
        }
      }
      if (n->hwndFrom == propList) {
        if (n->code == LVN_ITEMCHANGED && !rebuilding)
          DetailRow();
        if (n->code == NM_DBLCLK)
          EditRow();
        if (n->code == NM_RCLICK) {
          auto *p = reinterpret_cast<NMITEMACTIVATE *>(l);
          if (p->iItem >= 0) {
            ListView_SetItemState(propList, p->iItem,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            if (auto *r = CurrentRow())
              CopyText(r->value);
          }
        }
      }
      if (n->code == NM_CUSTOMDRAW &&
          (n->hwndFrom == fileList || n->hwndFrom == propList)) {
        auto *d = reinterpret_cast<NMLVCUSTOMDRAW *>(l);
        if (d->nmcd.dwDrawStage == CDDS_PREPAINT)
          return CDRF_NOTIFYITEMDRAW;
        if (d->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          d->clrTextBk = (d->nmcd.dwItemSpec % 2) ? RGB(244, 248, 249)
                                                  : RGB(255, 255, 255);
          return CDRF_DODEFAULT;
        }
      }
      return 0;
    }
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(w);
      std::vector<std::wstring> paths;
      UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        paths.push_back(p);
      }
      DragFinish(drop);
      if (busy)
        StatusText(L"任务执行中，请完成后再添加文件");
      else
        Load(std::move(paths));
      return 0;
    }
    case DataMessage: {
      std::unique_ptr<Data> d(reinterpret_cast<Data *>(l));
      if (d->replace >= 0 && d->replace < static_cast<int>(files.size())) {
        files[d->replace] = std::move(d->info);
        ListView_SetItemText(fileList, d->replace, 1,
                             const_cast<LPWSTR>(L"已刷新"));
        if (active == d->replace)
          ShowProperties();
      } else if (d->replace < 0) {
        files.push_back(std::move(d->info));
        AddFileItem(files.back());
        if (active < 0) {
          active = static_cast<int>(files.size()) - 1;
          ListView_SetItemState(fileList, active, LVIS_SELECTED | LVIS_FOCUSED,
                                LVIS_SELECTED | LVIS_FOCUSED);
          ShowProperties();
        }
      }
      return 0;
    }
    case ProgressMessage: {
      std::unique_ptr<ProgressData> p(reinterpret_cast<ProgressData *>(l));
      if (!canceled)
        StatusText(p->text);
      SendMessageW(bar, PBM_SETPOS, p->percent, 0);
      return 0;
    }
    case LogMessage: {
      std::unique_ptr<std::wstring> p(reinterpret_cast<std::wstring *>(l));
      jobLog += *p + L"\r\n";
      return 0;
    }
    case DoneMessage:
      if (worker.joinable())
        worker.join();
      Busy(false);
      SendMessageW(bar, PBM_SETPOS, canceled ? 0 : 1000, 0);
      StatusText((canceled ? L"已取消。" : L"任务完成。") +
                 std::wstring(L"共 ") + std::to_wstring(files.size()) +
                 L" 个文件" + (jobLog.empty() ? L"" : L"；请查看“操作结果”"));
      if (closing)
        DestroyWindow(h);
      return 0;
    case WM_CLOSE:
      if (busy) {
        closing = true;
        canceled = true;
        StatusText(L"正在结束任务，完成当前操作后关闭…");
      } else
        DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (worker.joinable())
        worker.join();
      DeleteObject(font);
      PostQuitMessage(0);
      return 0;
    }
  } catch (const std::exception &e) {
    ShowError(e);
  }
  return DefWindowProcW(h, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  instance = inst;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr))
    return 1;
  SetProcessDPIAware();
  HDC screen = GetDC(nullptr);
  dpi = GetDeviceCaps(screen, LOGPIXELSY);
  ReleaseDC(nullptr, screen);
  INITCOMMONCONTROLSEX controls{sizeof(controls),
                                ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&controls);
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = WindowProc;
  cls.hInstance = inst;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  cls.lpszClassName = ClassName;
  RegisterClassExW(&cls);
#ifdef _WIN64
  const wchar_t *title = L"文件属性查改器 V2.0 · 64 位";
#else
  const wchar_t *title = L"文件属性查改器 V2.0 · 32 位";
#endif
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int width = std::min<int>(Scale(1180), work.right - work.left),
      height = std::min<int>(Scale(800), work.bottom - work.top);
  HWND h = CreateWindowExW(0, ClassName, title,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           work.left + (work.right - work.left - width) / 2,
                           work.top + (work.bottom - work.top - height) / 2,
                           width, height, nullptr, nullptr, inst, nullptr);
  if (!h) {
    CoUninitialize();
    return 1;
  }
  ShowWindow(h, show);
  UpdateWindow(h);
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> paths;
  if (argv) {
    for (int i = 1; i < argc; ++i)
      paths.push_back(argv[i]);
    LocalFree(argv);
  }
  try {
    if (!paths.empty())
      Load(std::move(paths));
  } catch (const std::exception &e) {
    ShowError(e);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.wParam == 'F' &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
      SetFocus(search);
      continue;
    }
    if (!IsDialogMessageW(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}
