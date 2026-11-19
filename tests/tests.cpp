#include "core.h"
#include "internal.h"
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <winioctl.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << "FAIL line " << __LINE__ << ": " << #x << "\n";             \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int Tests(bool large) {
  std::cout.setf(std::ios::unitbuf);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  CHECK(fp::CsvCell(L"a,\"b\"\nc") == L"\"a,\"\"b\"\"\nc\"");
  CHECK(fp::CsvCell(L"=1+1") == L"\"'=1+1\"");
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring path = std::wstring(temp) + L"fpv-test-" +
                      std::to_wstring(GetCurrentProcessId()) + L".txt";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0,
                         nullptr);
  CHECK(f != INVALID_HANDLE_VALUE);
  DWORD n;
  CHECK(WriteFile(f, "abc", 3, &n, nullptr) && n == 3);
  CloseHandle(f);
  std::atomic_bool cancel{false};
  auto hash = fp::HashFile(path, true, true, cancel);
  CHECK(hash.md5 == L"900150983cd24fb0d6963f7d28e17f72");
  CHECK(hash.sha256 ==
        L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto empty = path + L".empty";
  {
    fp::Handle h(CreateFileW(empty.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, 0, nullptr));
  }
  auto emptyHash = fp::HashFile(empty, true, true, cancel);
  CHECK(emptyHash.md5 == L"d41d8cd98f00b204e9800998ecf8427e");
  CHECK(emptyHash.sha256 ==
        L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  DeleteFileW(empty.c_str());
  {
    fp::Handle locked(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr));
    bool refused = false;
    try {
      fp::HashFile(path, true, true, cancel);
    } catch (const std::exception &) {
      refused = true;
    }
    CHECK(refused);
  }
  cancel = true;
  bool failed = false;
  try {
    fp::HashFile(path, true, true, cancel);
  } catch (const std::exception &) {
    failed = true;
  }
  CHECK(failed);
  cancel = false;
  std::cout << "inspect\n";
  auto info = fp::Inspect(path);
  CHECK(!info.rows.empty());
  fp::Edit(path, {L"隐藏", L"", L"2", fp::Kind::Attribute}, L"是");
  CHECK(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN);
  fp::Edit(path, {L"隐藏", L"", L"2", fp::Kind::Attribute}, L"否");
  CHECK(!(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN));
  fp::Edit(path, {L"修改时间", L"", L"modified", fp::Kind::Time},
           L"2020-02-29 12:34:56");
  WIN32_FILE_ATTRIBUTE_DATA data{};
  CHECK(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data));
  CHECK(fp::FormatTime(data.ftLastWriteTime) == L"2020-02-29 12:34:56");
  failed = false;
  try {
    fp::ParseTime(L"2025-02-29 00:00:00");
  } catch (const std::exception &) {
    failed = true;
  }
  CHECK(failed);
  fp::FileInfo exportInfo{
      path, {{L"标题", L"=1+1"}, {L"备注", L"中文,\"引号\"\n换行"}}};
  std::cout << "csv\n";
  fp::Export(path + L".csv", {exportInfo}, fp::ExportFormat::Csv);
  std::ifstream csv(path + L".csv", std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(csv)), {});
  csv.close();
  CHECK(text.rfind("\xef\xbb\xbf", 0) == 0);
  CHECK(text.find("\"'=1+1\"") != std::string::npos);
  std::cout << "xlsx\n";
  fp::Export(path + L".xlsx", {exportInfo}, fp::ExportFormat::Xlsx);
  auto sheet =
      fp::ReadPackagePart(path + L".xlsx", L"/xl/worksheets/sheet1.xml");
  CHECK(sheet.find("t=\"inlineStr\"") != std::string::npos);
  CHECK(sheet.find("<f>") == std::string::npos);
  CHECK(sheet.find("=1+1") != std::string::npos);
  fp::FileInfo special{path, {{L"特殊", L"_x0041_\r\n\x0001"}}};
  fp::Export(path + L".special.xlsx", {special}, fp::ExportFormat::Xlsx);
  auto encoded = fp::ReadPackagePart(path + L".special.xlsx",
                                     L"/xl/worksheets/sheet1.xml");
  CHECK(encoded.find("_x005F_x0041__x000D_\n_x0001_") != std::string::npos);
  auto recovery = path + L".recovery", originalBackup = path + L".original.bak";
  CHECK(CopyFileW(path.c_str(), originalBackup.c_str(), TRUE));
  CHECK(fp::RecoverReplacement(recovery, originalBackup));
  CHECK(fp::HashFile(recovery, true, false, cancel).md5 == hash.md5);
  CHECK(GetFileAttributesW(originalBackup.c_str()) != INVALID_FILE_ATTRIBUTES);
  DeleteFileW(recovery.c_str());
  DeleteFileW(originalBackup.c_str());
  failed = false;
  try {
    fp::Export(path, {info}, fp::ExportFormat::Text);
  } catch (const std::exception &) {
    failed = true;
  }
  CHECK(failed);
  CHECK(fp::HashFile(path, true, false, cancel).md5 == hash.md5);
  auto dir = path + L".folder";
  CHECK(CreateDirectoryW(dir.c_str(), nullptr));
  CHECK(CreateDirectoryW((dir + L"\\sub").c_str(), nullptr));
  CHECK(CopyFileW(path.c_str(), (dir + L"\\a.txt").c_str(), TRUE));
  CHECK(CopyFileW(path.c_str(), (dir + L"\\sub\\b.txt").c_str(), TRUE));
  std::vector<std::wstring> errors;
  CHECK(fp::ExpandPaths({dir, dir}, false, cancel, errors).size() == 1);
  CHECK(fp::ExpandPaths({dir}, true, cancel, errors).size() == 2);
  CHECK(errors.empty());
  DeleteFileW((dir + L"\\a.txt").c_str());
  DeleteFileW((dir + L"\\sub\\b.txt").c_str());
  RemoveDirectoryW((dir + L"\\sub").c_str());
  RemoveDirectoryW(dir.c_str());
  std::wstring doc = path + L".docx";
  CHECK(CopyFileW(L"tests/fixtures/document.docx",doc.c_str(),TRUE));
  auto body=fp::ReadPackagePart(doc,L"/word/document.xml");
  std::cout << "office edit\n";
  std::wstring backup =
      fp::Edit(doc, {L"标题", L"Before", L"dc:title", fp::Kind::Office},
               L"修改后的中文标题");
  CHECK(GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES);
  CHECK(fp::ReadPackagePart(backup, L"/docProps/core.xml").find("Before") !=
        std::string::npos);
  CHECK(fp::ReadPackagePart(doc, L"/docProps/core.xml")
            .find(fp::Utf8(L"修改后的中文标题")) != std::string::npos);
  DeleteFileW(backup.c_str());
  backup = fp::Edit(doc, {L"创建", L"", L"dcterms:created", fp::Kind::Office},
                    L"2019-01-02 03:04:05");
  CHECK(fp::ReadPackagePart(doc,L"/word/document.xml")==body);
  DeleteFileW(backup.c_str());
  backup =
      fp::Edit(doc, {L"创建", L"", L"dcterms:created", fp::Kind::Office}, L"");
  CHECK(
      fp::ReadPackagePart(doc, L"/docProps/core.xml").find("dcterms:created") ==
      std::string::npos);
  DeleteFileW(backup.c_str());
  wchar_t module[MAX_PATH];
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::wstring app = module;
  app.resize(app.find_last_of(L"\\/") + 1);
  app += L"FilePropertyEditor.exe";
  auto pe = path + L".exe";
  CHECK(CopyFileW(app.c_str(), pe.c_str(), TRUE));
  auto originalHash = fp::HashFile(app, true, false, cancel).md5;
  backup = fp::Edit(pe, {L"公司", L"", L"CompanyName", fp::Kind::Version},
                    L"测试公司");
  CHECK(fp::HashFile(backup, true, false, cancel).md5 == originalHash);
  DeleteFileW(backup.c_str());
  backup = fp::Edit(pe, {L"版本", L"", L"FileVersion", fp::Kind::Version},
                    L"3.4.5.6");
  auto peInfo = fp::Inspect(pe);
  bool versionFound = false;
  for (const auto &r : peInfo.rows)
    if (r.kind == fp::Kind::Version && r.key == L"FileVersion")
      versionFound = r.value == L"3.4.5.6";
  CHECK(versionFound);
  CHECK(fp::HashFile(app, true, false, cancel).md5 == originalHash);
  DeleteFileW(backup.c_str());
  DeleteFileW(pe.c_str());
  std::cout << "photo inspect\n";
  auto photo = path + L".jpg";
  CHECK(CopyFileW(L"tests/fixtures/exif.jpg", photo.c_str(), TRUE));
  auto photoInfo = fp::Inspect(photo);
  bool photoDate = false, photoCamera = false;
  fp::Row photoTitle,photoDateRow;
  for (const auto &r : photoInfo.rows) {
    if (r.key == L"System.Photo.DateTaken") {
      photoDate = r.value.find(L"2018-04-05") != std::wstring::npos;
      photoDateRow=r;
    }
    if (r.key == L"System.Photo.CameraManufacturer")
      photoCamera = r.value == L"Test Camera";
    if (r.key == L"System.Title")
      photoTitle = r;
  }
  CHECK(photoDate);
  CHECK(photoCamera);
  CHECK(photoTitle.editable());
  std::cout << "photo edit\n";
  backup = fp::Edit(photo, photoTitle, L"照片标题测试");
  auto updatedPhoto = fp::Inspect(photo);
  bool titleSaved = false;
  for (const auto &r : updatedPhoto.rows)
    if (r.key == L"System.Title")
      titleSaved = r.value == L"照片标题测试";
  CHECK(titleSaved);
  DeleteFileW(backup.c_str());
  CHECK(photoDateRow.editable());
  backup=fp::Edit(photo,photoDateRow,L"2010-02-03 04:05:06");
  auto dateInfo=fp::Inspect(photo);bool dateSaved=false;for(const auto&r:dateInfo.rows)if(r.key==L"System.Photo.DateTaken")dateSaved=r.value==L"2010-02-03 04:05:06";
  CHECK(dateSaved);DeleteFileW(backup.c_str());
  DeleteFileW(photo.c_str());
  if(large) CHECK(CopyFileW(doc.c_str(),L"build/verification.docx",FALSE));
  DeleteFileW(doc.c_str());
  DeleteFileW((path + L".csv").c_str());
  DeleteFileW((path + L".xlsx").c_str());
  DeleteFileW((path + L".special.xlsx").c_str());
  DeleteFileW(path.c_str());
  failed = false;
  try {
    fp::HashFile(path, true, true, cancel);
  } catch (const std::exception &) {
    failed = true;
  }
  CHECK(failed);
  if (large) {
    fp::Export(L"build/verification.xlsx", {exportInfo},
               fp::ExportFormat::Xlsx);
    auto iso = path + L".iso";
    {
      fp::Handle file(CreateFileW(iso.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, 0, nullptr));
      DWORD returned = 0;
      CHECK(DeviceIoControl(file.h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                            &returned, nullptr));
      LARGE_INTEGER length{};
      length.QuadPart = (1ULL << 30) + 17;
      CHECK(SetFilePointerEx(file.h, length, nullptr, FILE_BEGIN));
      CHECK(SetEndOfFile(file.h));
    }
    auto largeHash = fp::HashFile(iso, true, true, cancel);
    CHECK(largeHash.md5 == L"cd239f6b25a60d0532393bbf1cc0634a");
    CHECK(largeHash.sha256 ==
          L"58604e5e20aaee5de20913ece30d6721fc0f851952cd927234e06889a82803cb");
    cancel = false;
    bool stopped = false;
    try {
      fp::HashFile(iso, true, true, cancel,
                   [&](ULONGLONG, ULONGLONG) { cancel = true; });
    } catch (const std::exception &) {
      stopped = true;
    }
    CHECK(stopped);
    cancel = false;
    DeleteFileW(iso.c_str());
    std::cout << "Large-file and mid-stream cancellation passed\n";
  }
  CoUninitialize();
  std::wcout << L"All tests passed\n";
  return 0;
}
int wmain(int argc, wchar_t **) {
  try {
    return Tests(argc > 1);
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
