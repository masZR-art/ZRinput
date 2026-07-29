# ZRinput

ZRinput 是一个从零开发的 Windows 中文输入法，不包含 Rime、Weasel 或其他
输入法项目的源代码与 Git 历史。

当前版本为 `0.1.0-alpha.2`。它已经具备可注册的 Windows TSF 前端、拼音
组合与候选选择、候选翻页、基础 Emoji、个人语言记忆、上下文联想、Win11
暗色候选窗、可保存主题编辑器以及安装、更新和卸载脚本。

个人记忆已支持 1 至 4 段有序上下文、句子边界、应用场景、时间衰减、负反馈
以及带校验和的本地持久化。损坏的记忆文件不会替换正在使用的有效模型。

拼音核心已支持连续输入的多路径音节切分、空格或撇号强制分隔、声调数字，
以及 `ü`、`u:`、`v` 三种写法的归一化查询。

词典支持 UTF-8 TSV 批量加载与坏行统计；未完成的拼音前缀也能提前产生候选，
并与完整匹配区分权重。

默认词典包含 6.1 万余条候选、5.7 万余个不同词，并为固定的 425 个普通话
及扩展拼音拼写全部提供单字候选。词与频次来自 MIT 许可的 Jieba 0.42.1，
读音来自 MIT 许可的 pypinyin 0.55.0；完整声明见
`THIRD_PARTY_NOTICES.md`。这些 Python 包只用于维护时生成数据，不是输入法的
运行时依赖。

## 构建与无前台验证

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

上述测试不会注册或激活 TSF，也不会切换当前系统输入法。候选窗测试通过离屏
位图渲染和像素审计完成，不显示前台窗口；DLL 冒烟测试使用隔离测试模式，
不会读写个人记忆。还可以使用命令行查询工具直接检查真实词典的候选与耗时：

```powershell
.\build\Release\zrinput_query.exe .\data\default_lexicon.tsv `
  xianzai woshizhongguoren jintianwanshanghaoma
.\build\Release\zrinput_candidate_preview.exe --render `
  .\build\candidate-preview-test.bmp
python .\tests\candidate_bitmap_audit.py .\build\candidate-preview-test.bmp
```

## 重建和审计词典

仓库已提交生成好的运行时词典，一般构建不需要执行本节。更新词源时使用固定
版本依赖；生成器会验证源数据 SHA-256，拒绝内容不一致的包：

```powershell
python -m pip install -r scripts\requirements-lexicon.txt
python scripts\generate_lexicon.py
python tests\lexicon_audit.py
```

## 安装 Alpha 包

1. 解压 `ZRinput-0.1.0-alpha.2-windows-x64.zip`。
2. 在解压目录中用下列命令安装。脚本只为机器注册阶段请求管理员权限，当前
   用户的 COM 配置仍在原登录账户中完成。
3. 运行 `zrinput_profile_tool.exe activate`，或在 Windows 输入法菜单中选择
   `ZRinput`。
4. 安装或更新后关闭并重新打开测试应用；仍显示旧版本时注销并重新登录。
5. 更新使用当前用户目录，不再重复请求管理员权限。卸载会保留个人记忆与
   已保存主题；正在使用的 DLL 会安排在 Windows 重启后删除。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1
.\zrinput_profile_tool.exe activate
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\update.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
```

显式使用 `-ExecutionPolicy Bypass` 是为了避免浏览器下载的 ZIP 带有网络来源
标记时，Windows 阻止尚未签名的 Alpha 安装脚本。

开发构建可直接运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\scripts\install.ps1 -BuildDirectory .\build
.\build\Release\zrinput_profile_tool.exe activate
```

## 输入操作

- 输入连续拼音后，按 `Space` 选择当前页第一个候选。
- 按数字 `1` 至 `7` 选择对应候选。
- 按 `Left` / `Right` 或 `PageUp` / `PageDown` 翻页。
- 按 `Backspace` 修改拼音，按 `Escape` 取消，按 `Enter` 提交原始拼音。
- 逗号、句号会输入中文 `，`、`。`；配合 `Shift` 输入 `《`、`》`。
- 输入 `emoji` 可使用基础 Emoji 候选。

## 主题

运行 `zrinput_theme_editor.exe` 可视化调整背景、选中块、强调线、文字颜色、
字号和候选窗高度，并保存到本地主题文件。内置首个皮肤为
`themes\microsoft-dark.ini`。候选窗只高亮当前候选词块，宽度会根据当前页
内容自适应。

## 隐私原则

- 个人记忆默认只保存在本机。
- 标准 Windows `Edit` 密码编辑控件（`ES_PASSWORD`）不参与个人记忆；自绘或
  非标准密码控件目前不在自动识别保证范围内。
- 网络建议是可选模块，不与本地个人记忆混合上传。

## Alpha 限制

- 当前发布包只包含 x64 TSF DLL；32 位应用程序需要后续双架构包，不能加载本
  Alpha 的 64 位进程内输入法 DLL。
- 词库已覆盖完整拼音音节与数万常用词，但专有名词和长尾新词仍不及成熟商业
  输入法，需要后续通过可更新词包补充。
- Emoji 是基础集合，候选窗暂未使用彩色 Emoji 字形。
- 联网建议尚未启用；当前联想和学习完全在本地完成。
- 主题目前编辑一个活动皮肤，完整的多皮肤管理器将在后续版本加入。
