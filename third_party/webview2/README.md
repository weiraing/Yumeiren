# WebView2 SDK（入库说明）

本目录与 Live2D 的 third_party 不同：**这里的文件随仓库分发**（git add -f 越过了
third_party/ 的整体忽略——那条规则是为专有许可的 Cubism SDK 设的，WebView2 SDK
是 MIT 许可、允许再分发）。

| 文件 | 来源 | 用途 |
|---|---|---|
| include/WebView2.h | nuget `Microsoft.Web.WebView2` 1.0.4191.47 的 build/native/include | COM 接口声明（MinGW 可直接编译，见 tools/webprobe 探针） |
| loader/WebView2Loader.dll | 同上包的 build/native/x64 | 进程加载器（约 0.2MB），构建期部署到 exe 目录 |

浏览器运行时本体是系统组件（Edge WebView2 Runtime，Win11 内置），不随包发布；
缺失时 WebWallpaper 以「运行时不可用」降级，UI 会给出安装指引。

升级方式：从 nuget 下载新包，替换这两个文件即可（接口向后兼容）。
