# Contributing / 参与贡献

## English

Thanks for helping improve nSkinzCS2. This project targets offline Counter-Strike 2 demo playback through HLAE; contributions must preserve that boundary.

Before opening an issue or pull request:

- Read the [English README](README.md) and the [Chinese README](README.zh-CN.md).
- Reproduce against the documented CS2 build and HLAE version when possible.
- Keep changes focused. Do not commit DLLs, PDBs, logs, demos, game files, personal identifiers or payment data.
- Run `run-build-and-tests.cmd` or `run-tests.cmd` when your change touches C++ or configuration logic.
- Include a concise summary, test command, compatibility baseline and any known limitation.

Pull requests should explain the runtime effect, avoid unrelated formatting churn, and update the README when user-facing behavior changes. Cosmetic testing must remain offline and use `-insecure`.

## 简体中文

感谢参与 nSkinzCS2。项目面向通过 HLAE 进行的 Counter-Strike 2 离线 Demo 回放，所有贡献都必须保持这个使用边界。

提交 Issue 或 Pull Request 前请：

- 阅读[中文 README](README.zh-CN.md)和[英文 README](README.md)。
- 尽可能使用文档中的 CS2 与 HLAE 版本复现。
- 保持修改聚焦，不要提交 DLL、PDB、日志、Demo、游戏文件、个人标识或收款资料。
- 修改 C++ 或配置逻辑后运行 `run-build-and-tests.cmd` 或 `run-tests.cmd`。
- 写明变更摘要、测试命令、兼容基线和已知限制。

Pull Request 应说明运行时影响，避免无关格式化，并在用户可见行为变化时同步更新 README。所有皮肤测试必须保持离线并使用 `-insecure`。
