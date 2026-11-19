# 作者信息与程序内更新 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** mAoyf-dev 作者链接和可取消的程序内新版查询下载。
**Architecture:** 新增 update 模块，用 WinHTTP 后台查询 GitHub Release API，严格解析版本和附件，下载至同目录临时文件并校验后原子保存；复用现有 worker、取消和进度 UI。
**Tech Stack:** C++17 / Win32 / WinHTTP，无新运行库。
**Spec:** docs/superpowers/specs/2026-11-19-author-update-design.md，用户已选择方案 2。

## Global Constraints
- x86/x64，静态 CRT，Win7 API 下限；HTTPS 证书保持系统验证。
- 作者 mAoyf-dev；保留原版历史作者；版本 2.1.0。
- 仅手动联网，不自动执行或替换自身；失败/取消保留已有目标文件。

## Review Focus
- 超时、取消、HTTP 404/403 分别反馈。
- JSON 深度、大小、转义及数字边界。
- 不匹配架构和无附件不得误下载源代码。
- 重定向不可降级 HTTP 或跳到任意站点。
- 校验失败/目标被占用必须清理临时文件，保留目标。

## Tasks
- [x] 1. tests/update_tests.cpp 首先声明预期，运行确认缺少实现；添加 src/update.h、update.cpp，实现严格 JSON、版本比较、可信 URL 和架构匹配；测试通过。
- [x] 2. WinHTTP 请求与下载、取消进度、大小/摘要校验、原子保存；测试取消及目标保留，实网验证 API 与附件；更新两种构建配置。
- [x] 3. app.cpp 新增关于和检查更新，复用 worker 消息生命周期；统一 version.h 和资源版本；README 及验证记录更新。
- [x] 4. 双架构构建、完整测试、独立审查；刷新 dist 与 ZIP，不提交/推送/发布。

## Decisions
Ruling: 当前工程原位实施，不创建额外 worktree，所有本次修改保持可审阅；用户方案选择视为对该设计的确认。
Ruling: 摘要存在则强制 SHA-256 校验；历史 Release 无摘要时明确提示仅 HTTPS 和长度验证，不自动运行。

## Verification ledger
- 新测试首次运行因缺少 update 模块失败；实现后通过。
- 审查发现 x86_64 架构误选；新增断言复现失败，再修复，双架构通过。
- 最终 build.ps1 -Architecture all -Test 退出 0，无警告。
- x64 界面关于链接、正式版查询结果已检查；未发布新版本用于 UI 下载提示。
- 独立复审无新增重要问题；Win7 TLS 依赖系统支持。
