# vendored: winautoupdate

来源：https://github.com/Hunlongyu/updater @ 0f93399

Windows 纯 C 自动更新库。仅链接 winhttp + bcrypt。
更新方式：从上游仓库同步 include/ 与 src/ 覆盖本目录同名文件。
（注意：上游工作副本是 LF，本仓库是 CRLF。整目录覆盖会把全部文件刷成行尾差异，
淹没真实改动——只同步内容确有变化的文件，或同步后用 `git diff --ignore-cr-at-eol` 复核。）

限流兜底（0f93399 起）：REST API 被限流时自动改走 github.com 网页端点。它要求
Release 附带 `checksums.txt`（本仓库 release.yml 已生成），且 `asset_pattern` 必须是
确定文件名——hua 的 `hua-<arch>.exe` 已满足。详见上游 README「限流兜底」。
