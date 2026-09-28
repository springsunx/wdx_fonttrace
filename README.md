# wdx_fonttrace

面向大量中文字体管理的高性能 Total Commander / Double Commander WDX
内容插件。它的主要用途是把 Illustrator “缺失字体”窗口显示的
**PostScript Name** 反查到本地 TTF、OTF 或字体集合文件。

这是对 [springsunx/wdx_fontinfo](https://github.com/springsunx/wdx_fontinfo)
热路径的 C++20 重构版；原项目由 Daniel Plakhotich 创建。当前版本有意缩小
格式和字段范围，以换取更快的目录列显示与插件搜索速度。

## 字段

| WDX 字段 | OpenType 名称来源 | 行为 |
|---|---|---|
| Family | Name ID 16，回退到 ID 1 | 优先排印家族名 |
| Style | Name ID 17，回退到 ID 2 | 优先排印子族名 |
| Full Name | Name ID 4 | 完整字体名 |
| PostScript Name | Name ID 6 | 原样返回，不用其他名称伪造 |

WDX 只在 Total Commander 请求某个字段时解析该字段。配置一个
`PostScript Name` 列不会顺带解析另外三个字段。同一线程连续读取同一文件时，
插件会复用文件映射和已经取得的字段值。

## 中文字体兼容

- 按当前 Windows 用户语言选择本地化名称。
- 支持 Windows Unicode、GB2312、Big5、Shift-JIS、Wansung 和 Johab 名称记录。
- 保留参考实现针对错误 UCS-2、空字节填充以及旧中文字体名称的修复逻辑。
- Name ID 16/17 可正确覆盖旧式 ID 1/2。
- PostScript Name 始终保持字体内的原始值；空值和重复值不会被掩盖。

## 支持范围

支持 `.ttf`、`.otf`、`.otb`、`.ttc` 和 `.otc`。字体集合当前读取第一张
font face。

为了保持实现小而快，本版暂不支持 WOFF/WOFF2、EOT、FON、Type 1、BDF、
PCF 等格式。

## 安装和使用

发布包中同时包含：

- `fonttrace.wdx`：32 位 Total Commander
- `fonttrace.wdx64`：64 位 Total Commander

在 Total Commander 中打开发布 ZIP 并确认安装。随后可以：

1. 建立自定义列视图，加入 `PostScript Name`。
2. 在“查找文件 → 插件”中选择 `fonttrace` 的 `PostScript Name` 字段。
3. 粘贴 Illustrator 报告的名称并进行精确匹配。

例如 `DenshanCSEG-Light-GB` 可以定位到 `文鼎CS细等线.ttf`。

## 性能实测

在一套包含 2,289 个中文字体的目录中，缓存预热后的单字段完整扫描约
96–123 ms，即约 1.9–2.4 万文件/秒。对包含 3,812 个字体的字体库执行一次
冷缓存 PostScript Name 精确反查约 6.6 秒。首次读取速度主要取决于磁盘、
杀毒软件和 Windows 文件缓存；这些数字仅用于说明测试规模，不作为硬件无关
基准。

## 构建

要求 Windows、Visual Studio 2022 Build Tools 和 CMake 3.20 或更高版本。
运行库采用静态链接，最终插件仅依赖 Windows 系统 DLL。

```powershell
cmake -S . -B out/build-x64 -A x64
cmake --build out/build-x64 --config Release
ctest --test-dir out/build-x64 -C Release --output-on-failure

cmake -S . -B out/build-x86 -A Win32
cmake --build out/build-x86 --config Release
ctest --test-dir out/build-x86 -C Release --output-on-failure
```

测试组件：

- `sfnt_reader_tests`：名称选择、边界检查和中文修复规则。
- `wdx_exports_tests`：实际 DLL 的 WDX 导出、字段顺序和字段类型。
- `wdx_probe`：加载真实插件，递归测试字体目录或精确反查 PostScript Name。

```text
wdx_probe fonttrace.wdx64 C:\Windows\Fonts
wdx_probe fonttrace.wdx64 E:\Fonts DenshanCSEG-Light-GB
```

GitHub Actions 会同时构建和测试 Win32/x64，并生成可安装 ZIP。

## 项目结构

- `native/plugin.cpp`：WDX ABI、内存映射和线程内缓存
- `native/sfnt_reader.cpp`：带完整边界检查的 SFNT `name` 表读取
- `native/sfnt_reader_tests.cpp`：解析器单元测试
- `native/wdx_exports_tests.cpp`：插件 ABI 测试
- `native/wdx_probe.cpp`：真实字体测试与反查工具

## 许可证与来源

本仓库是经过明确修改的派生版本，保留原项目的 `LICENSE.txt` 与版权声明。
请勿将本重构版误认为原作者发布的官方版本。
