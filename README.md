# ZRinput

ZRinput 是一个从零开发的 Windows 中文输入法，不包含 Rime、Weasel 或其他
输入法项目的源代码与 Git 历史。

当前版本为 `0.1.0-alpha.1`。它已经具备可注册的 Windows TSF 前端、拼音
组合与候选选择、候选翻页、基础 Emoji、个人语言记忆、上下文联想、Win11
暗色候选窗、可保存主题编辑器以及安装、更新和卸载脚本。

个人记忆已支持 1 至 4 段有序上下文、句子边界、应用场景、时间衰减、负反馈
以及带校验和的本地持久化。损坏的记忆文件不会替换正在使用的有效模型。

拼音核心已支持连续输入的多路径音节切分、空格或撇号强制分隔、声调数字，
以及 `ü`、`u:`、`v` 三种写法的归一化查询。

词典支持 UTF-8 TSV 批量加载与坏行统计；未完成的拼音前缀也能提前产生候选，
并与完整匹配区分权重。

## 构建核心测试

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 安装 Alpha 包

1. 解压 `ZRinput-0.1.0-alpha.1-windows-x64.zip`。
2. 在解压目录中运行 `scripts\install.ps1`。首次注册 TSF 需要管理员权限。
3. 运行 `zrinput_profile_tool.exe activate`，或在 Windows 输入法菜单中选择
   `ZRinput`。
4. 更新已安装版本时运行 `scripts\update.ps1`。更新使用当前用户目录，不再
   重复请求管理员权限。
5. 卸载时运行 `scripts\uninstall.ps1`。卸载会保留个人记忆文件。

开发构建可直接运行：

```powershell
.\scripts\install.ps1 -BuildDirectory .\build
.\build\Release\zrinput_profile_tool.exe activate
```

## 输入操作

- 输入连续拼音后，按 `Space` 选择当前页第一个候选。
- 按数字 `1` 至 `5` 选择对应候选。
- 按 `Left` / `Right` 或 `PageUp` / `PageDown` 翻页。
- 按 `Backspace` 修改拼音，按 `Escape` 取消，按 `Enter` 提交原始拼音。
- 输入 `emoji` 可使用基础 Emoji 候选。

## 主题

运行 `zrinput_theme_editor.exe` 可视化调整背景、选中块、强调线、文字颜色、
字号和候选窗高度，并保存到本地主题文件。内置首个皮肤为
`themes\microsoft-dark.ini`。候选窗只高亮当前候选词块，宽度会根据当前页
内容自适应。

## 隐私原则

- 个人记忆默认只保存在本机。
- 密码框与隐私模式不学习。
- 用户可以查看、固定、遗忘、导出或清空记忆。
- 网络建议是可选模块，不与本地个人记忆混合上传。

## Alpha 限制

- 词库规模仍较小，尚未达到微软拼音的通用词汇覆盖率。
- Emoji 是基础集合，候选窗暂未使用彩色 Emoji 字形。
- 联网建议尚未启用；当前联想和学习完全在本地完成。
- 主题目前编辑一个活动皮肤，完整的多皮肤管理器将在后续版本加入。
- 极端的零间隔自动按键注入可能使 TSF 同步编辑会话丢键；正常逐键输入已通过
  真实记事本验证，压力输入队列仍需在后续 Alpha 加固。
