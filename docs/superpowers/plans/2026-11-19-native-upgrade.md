# 文件属性查改器 V2.0 Implementation Plan

Goal: 在保留原版原件的前提下交付 C++ Win32 单文件 x86/x64 软件，覆盖网友的查看、修改、大文件哈希和导出建议。

Architecture: 复用 Windows 文件/属性/安全/签名/OPC API，无第三方运行库。UI 线程仅交互，后台串行采集与流式哈希；内容修改在同目录临时副本上完成并回读，通过后用 ReplaceFile 生成备份并替换。

Tech Stack: C++17、Win32、COM Property System、CryptoAPI、WinTrust、MSXML6、Windows OPC，MSVC /MT。

Spec: 项目根目录 REA分析报告.md 及本会话网友建议。用户已明确要求现在实施，直接在当前非 Git 目录开发。

## Global Constraints

- 原版 EXE 和 REA 证据保持不变；x86/x64 同源构建，无安装、无应用配置注册表写入。
- 哈希默认关闭，无 1 GiB 人为限制；读取错误和取消不得显示部分摘要。
- 批量编辑逐文件报告，不因单文件失败假报整批成功。
- 文件系统时间、文档时间、拍摄时间分开；派生字段只读。
- 标题等按实际格式/处理器能力编辑；Office Open XML 核心属性通过 OPC 直接支持；不支持的格式明确说明。
- 提供 TXT/CSV/真正的 XLSX；CSV 防公式注入；默认导出名来自文件名。
- 元数据修改生成备份，校验后替换；用户明确保存才写入。

## Review Focus

- 大于 1 GiB 文件、取消、文件读取失败：摘要完整性与可响应性。
- 损坏的 PE、无版本资源、无处理器：不得崩溃或把未知显示为确定结论。
- 中文/长内容/换行/公式开头文本：导出不得截断、误执行。
- 文件占用/只读/写入失败：不得损坏原件，显示备份路径和错误。
- x86/x64、不同 DPI、键盘导航：单独构建运行；旧版 Windows 明确验证边界。

## Tasks

- [x] 1. 建立 core.h、tests.cpp、CMakeLists.txt、build.ps1；先运行空实现的行为测试并确认失败。
- [x] 2. core.cpp：基本属性、版本、类型提示、所有者/来源、Shell 元数据、OPC Office 元数据、信任验证、双哈希和取消。测试 abc/空文件哈希、丢失文件、PE、时间及标志修改、Office 核心属性回读。
- [x] 3. export.cpp：UTF-8 BOM TXT/CSV、OPC XLSX；测试引号/换行/公式文本、输出完整性及失败路径。
- [x] 4. app.cpp、app.rc、manifest：原生可缩放双列表、可见滚动条、完整值面板、批量导入/编辑、逐文件结果、哈希进度取消、导出与拖放/命令行。
- [x] 5. 双架构构建、真实集成测试、独立代码审查、修复与 README/发布文件。源目录无 Git，不创建提交。
