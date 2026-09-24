# ---------------------------------------------------------------------------
# @file   TrimRuntimeFiles.cmake
# @author rain
# @date   2026-09-25
# @brief  运行时输出目录里「经实测确认无用」的文件清单，以及剔除逻辑。
# @note   本地构建与 CI 打包共用这一份清单，入参见下方说明。
# @copyright 本项目遵循仓库 LICENSE。
# ---------------------------------------------------------------------------
#
# 三种用法：
#   1) 本地构建 —— CMakeLists.txt 里 yumeiren_trim_runtime() 建的独立 target，
#      挂在部署链**末尾**（所有部署步骤都跑完之后）；
#   2) CI 打包 —— cmake -DTARGET_DIR=<目录> -DRELEASE_PACKAGE=ON -P cmake/TrimRuntimeFiles.cmake；
#   3) CI 读清单 —— cmake -DPRINT_LIST=<文件> -P cmake/TrimRuntimeFiles.cmake（只读，不改文件）。
#
# ⚠️ 为什么剔除与断言**不能**塞进 DeployQtRuntime.cmake（曾经就是那么写的）：
#   那个脚本是三个部署步骤共用的拷贝器，每次调用都会跑一遍。断言挂在它末尾，
#   就变成在**只拷了一部分**的中间状态上做完整性检查 —— 2026-09-25 CI 实测：
#   Qt 运行库那步跑完时 WebView2Loader.dll 还没被拷进来（它排在下一步），
#   断言当场 FATAL_ERROR，整次构建红。本机没暴露是因为 build/ 里躺着上一次
#   构建留下的旧文件，把断言蒙过去了；从零构建一样会红。
#   结论：完整性检查只能有一个位置 —— **所有部署步骤之后**。
#
# 为什么清单只维护这一份：本地走 CMake 拷贝、CI 走 windeployqt，两条路径各自会把
#   **不同的**多余文件带进来（本地是 $<TARGET_RUNTIME_DLLS> 里的 Qt6Concurrent，
#   CI 是 windeployqt 带的图片插件与 CRT 家族），但「哪些是无用的」是同一个事实。
#   写两份必然漂移，而且漂移了没人会发现 —— 少删一个只是包大一点，多删一个才会炸，
#   于是错的那份能长期存活。
#
# 判定依据（2026-09-25 实测，三层证据，缺一层都会误判）：
#   1) 静态导入表（**含延迟导入表** —— MSVC 的 CRT 会用 delay-load 拉同伴 DLL）：
#      没有任何 PE 导入清单里的模块；
#   2) 运行时模块枚举（EnumProcessModulesEx）：程序真正跑起来（视频壁纸起播 +
#      看板娘 Live2D 渲染首帧）之后，这些 DLL 一个都没被加载；
#   3) 源码核对：全仓库零引用。
#   端到端验证：删完启动，主窗口 / 视频播放 / Live2D 上下文就绪 / 纹理上传 /
#   首帧 paintGL 与未删减的包逐行一致，日志 0 条错误。
#
# ⚠️ 刻意**不含** opengl32sw.dll（19.68MB）：它是「机器没有可用 OpenGL 驱动」时的
#   软件渲染回退 —— 远程桌面(RDP)会话、无 3D 加速的虚拟机、显卡驱动异常时会用到。
#   删掉能再省 19.68MB，但那些环境下程序会启动失败或画面全黑。这是产品决策而非
#   技术事实，所以留在清单外（2026-09-25 拍板保留）。

# 顶层文件按**文件名**匹配，子目录插件按**相对路径**（用 / 分隔）匹配。
set(YUMEIREN_TRIM_FILES
    # Qt 的 ANGLE 后端专用（编译 HLSL 着色器）。包里没有 libEGL.dll /
    # libGLESv2.dll，即根本没有 ANGLE，它就是纯废件。
    "D3Dcompiler_47.dll"

    # 全仓库零 QtSvg 引用，素材里也没有任何 svg。它只是被下面那两个同样要删的
    # 插件连带引用。
    "Qt6Svg.dll"

    # 源码里零 QtConcurrent / QFuture 引用。本地构建也会被
    # $<TARGET_RUNTIME_DLLS> 带进来（实测 MinGW 的 Qt 依赖树里有它）。
    "Qt6Concurrent.dll"

    # 下面五个都是 MSVC CRT 的「同族兄弟」：windeployqt 与「从 Redist 目录
    # *.dll 一把梭」的兜底拷贝都会把用不到的带上。逐个核对过（含延迟导入表），
    # 没有任何模块导入它们。
    "concrt140.dll"                 # ConcRT 并发运行时，Qt 不用 PPL
    "vccorlib140.dll"               # C++/CX (WinRT)，Qt 不走 C++/CX
    "msvcp140_atomic_wait.dll"      # std::atomic 的 wait/notify
    "msvcp140_codecvt_ids.dll"      # <codecvt> 的 id 表
    "vcruntime140_threads.dll"      # std::thread 拆出来的那部分

    # Qt 6 在 Windows 上默认走 FFmpeg 后端；实测运行时加载的是
    # multimedia/ffmpegmediaplugin.dll，windowsmediaplugin 一次都没被加载。
    "multimedia/windowsmediaplugin.dll"

    # main.cpp 里显式 app.setStyle(QStyleFactory::create("Fusion"))，且主题切换
    # 只换 QSS、不换 style 插件。这个插件会被 Qt 扫到并加载，但从不被使用。
    "styles/qmodernwindowsstyle.dll"

    # 桌面鼠标程序，没有触摸屏输入路径。
    "generic/qtuiotouchplugin.dll"

    # 网络状态检测后端；缺了 Qt 降级为「状态未知」，不影响任何功能。
    "networkinformation/qnetworklistmanager.dll"

    # 全仓库没有任何 HTTPS 请求（QNetworkAccessManager 只有 include、没有使用点），
    # 网页壁纸走 WebView2 自己的网络栈，不经过 QtNetwork 的 TLS 后端。
    "tls/qschannelbackend.dll"
    "tls/qcertonlybackend.dll"

    # 图片格式插件。图库过滤器只收 png/jpg/jpeg/bmp（webp 是否收录由
    # QImageReader::supportedImageFormats() 动态决定，见 ImagePage.cpp），
    # 素材里也只有 png；程序图标走 :/icons/yumeiren-*.png（内嵌 PNG），
    # 不经过 ico 插件。
    "imageformats/qgif.dll"
    "imageformats/qico.dll"
    "imageformats/qsvg.dll"
    "iconengines/qsvgicon.dll"
)

# 剔完之后必须还在的 —— 少一个就是「双击起不来」或「核心功能没了」。
# 这里是**反向断言**：删过头要当场红，而不是等用户反馈。
#
# ⚠️ 这一组只放**总是会被部署**的东西（Qt 侧 + 我们自己从 Qt bin 拷的那五个 FFmpeg
#   运行库）。来自 third_party/ 的（Live2D / WebView2）**不放这里** ——
#   third_party/ 不入库（Cubism 是专有许可），干净检出里它们**合法缺席**，
#   无条件要求会让「没有 SDK 的正常构建」在断言上红。
set(YUMEIREN_KEEP_FILES
    "Qt6Core.dll"
    "Qt6Gui.dll"
    "Qt6Widgets.dll"
    "Qt6Multimedia.dll"
    "Qt6MultimediaWidgets.dll"
    "Qt6OpenGL.dll"
    "Qt6OpenGLWidgets.dll"
    "Qt6Network.dll"
    "platforms/qwindows.dll"
    "multimedia/ffmpegmediaplugin.dll"
    "imageformats/qjpeg.dll"
    "avcodec-61.dll"
    "avformat-61.dll"
    "avutil-59.dll"
    "swresample-5.dll"
    "swscale-8.dll"
)

# 来自 third_party/ 的运行期文件：在不在，取决于那份 SDK 在不在。
#
# ⚠️ 判据必须与部署条件**逐条对齐**（同 MEMORY 里那条铁律）：多一条 → 干净检出上
#   误红；少一条 → 漏掉「部署静默跳过」。后者正是 Live2D 曾经的表现：构建全绿、
#   看板娘空白、日志里一行都没有。
#   所以这里不自己下结论 —— 由调用方按当前配置显式声明（-DYUMEIREN_EXPECT_LIVE2D=ON /
#   -DYUMEIREN_EXPECT_WEBVIEW2=ON），声明了才进必需清单。
set(YUMEIREN_KEEP_FILES_OPTIONAL
    "Live2DCubismCore.dll"
    "WebView2Loader.dll"
)

# 只有**发布包**才该有的，不能并进上面那组 —— 本地是 MinGW 构建：它不用 MSVC 的
# CRT（那是 MSVC 编出来的 exe 才导入的），Qt 也不带 icuuc.dll。把它们放进通用组，
# 本地每次构建都会在断言上红，而那并不是「删过头了」，是「本来就没有」。
set(YUMEIREN_KEEP_FILES_RELEASE
    "vcruntime140.dll"
    "vcruntime140_1.dll"
    "msvcp140.dll"
    "msvcp140_1.dll"
    "msvcp140_2.dll"
    "icuuc.dll"
)

# 被删空之后应该一并清掉的插件目录：留着空目录会让用户以为「这里本该有东西」。
set(YUMEIREN_TRIM_EMPTY_DIRS
    "tls" "styles" "generic" "networkinformation" "iconengines"
)

# ---------------------------------------------------------------------------
# 清单自相矛盾的静态检查 —— 放在**文件作用域**，于是每种用法都会跑到
#   （含 PRINT_LIST 那条只读清单、根本不碰文件的路径）。
#
# 这一条才是「删过头」的真正防线：剔清单里混进一个必需文件，是**纯集合性质**的错误，
#   与现场有多少文件、部署步骤跑到第几步、调用顺序如何**全都无关**，必然错。
# 而下面那个「目录里在不在」的存在性断言依赖现场状态 —— 它能抓住「部署没跑到」，
#   但也会被现场状态骗（旧文件、中间状态）。两者互补，别拿一个替另一个。
# ---------------------------------------------------------------------------
set(_yumeiren_conflict "")
foreach(_rel IN LISTS YUMEIREN_TRIM_FILES)
    foreach(_group YUMEIREN_KEEP_FILES YUMEIREN_KEEP_FILES_OPTIONAL YUMEIREN_KEEP_FILES_RELEASE)
        list(FIND ${_group} "${_rel}" _idx)
        if(NOT _idx EQUAL -1)
            list(APPEND _yumeiren_conflict "${_rel}  (同时在 ${_group} 里)")
        endif()
    endforeach()
endforeach()
if(_yumeiren_conflict)
    message(FATAL_ERROR "剔除清单与保留清单自相矛盾，同一项不可能既删又留：${_yumeiren_conflict}")
endif()
unset(_yumeiren_conflict)
unset(_idx)
unset(_rel)
unset(_group)

# 剔除 dst_dir 下清单里的文件，并断言保留清单齐全。
#
# 删除用 `cmake -E rm` 而不是 file(REMOVE)：file(REMOVE) 失败会直接 FATAL_ERROR
#   中断整次构建。而「文件删不掉」最常见的场景恰恰是良性的 —— 用户正开着自己刚
#   构建出来的程序，DLL 被映射着。那种情况给一行 warning 就够了，不该把构建搞红。
function(yumeiren_trim_runtime_files dst_dir)
    if(NOT IS_DIRECTORY "${dst_dir}")
        message(STATUS "运行时剔除: 目录不存在，跳过 ${dst_dir}")
        return()
    endif()

    set(_victims "")
    foreach(_rel IN LISTS YUMEIREN_TRIM_FILES)
        if(EXISTS "${dst_dir}/${_rel}")
            list(APPEND _victims "${dst_dir}/${_rel}")
        endif()
    endforeach()

    list(LENGTH _victims _n)
    if(_n GREATER 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -E rm -f ${_victims}
                        RESULT_VARIABLE _rc)
        if(_rc EQUAL 0)
            message(STATUS "运行时剔除: 已删除 ${_n} 个无用文件 -> ${dst_dir}")
            foreach(_v IN LISTS _victims)
                file(RELATIVE_PATH _vrel "${dst_dir}" "${_v}")
                message(STATUS "  - ${_vrel}")
            endforeach()
        else()
            message(WARNING
                "运行时剔除: 有文件删不掉(rc=${_rc})，可能程序正开着、DLL 被占用。"
                "下次构建会重试。")
        endif()
    endif()

    # 插件目录被删空后一并清掉
    foreach(_d IN LISTS YUMEIREN_TRIM_EMPTY_DIRS)
        if(IS_DIRECTORY "${dst_dir}/${_d}")
            file(GLOB _left "${dst_dir}/${_d}/*")
            list(LENGTH _left _left_n)
            if(_left_n EQUAL 0)
                file(REMOVE_RECURSE "${dst_dir}/${_d}")
                message(STATUS "运行时剔除: 已删除空目录 ${_d}/")
            endif()
        endif()
    endforeach()

    # 存在性断言：只查「本次构建/本次打包本该有」的那些。
    # 必需清单 = 无条件组 + 调用方显式声明了的那几个可选件（见文件头那段）。
    set(_required ${YUMEIREN_KEEP_FILES})
    if(YUMEIREN_EXPECT_LIVE2D)
        list(APPEND _required "Live2DCubismCore.dll")
    endif()
    if(YUMEIREN_EXPECT_WEBVIEW2)
        list(APPEND _required "WebView2Loader.dll")
    endif()

    set(_missing "")
    foreach(_keep IN LISTS _required)
        if(NOT EXISTS "${dst_dir}/${_keep}")
            list(APPEND _missing "${_keep}")
        endif()
    endforeach()
    if(_missing)
        # 这条消息刻意不说「剔除把文件删了」—— 那只是两种可能之一，而且往往不是。
        # 真实经历：CI 上这么红过一次，缺的是 WebView2Loader.dll，真因是**断言跑早了**
        # （那时它还没被拷进来），跟剔除毫无关系。把「跑早了 / 没跑到 / 被删了」并列写出来，
        # 免得下一个人又顺着「谁删的」去查。
        message(FATAL_ERROR
            "运行时输出目录不完整，缺：${_missing}\n"
            "  三种可能，按顺序排查：\n"
            "    1) 断言跑早了 —— 该文件的部署步骤还没执行（见 CMakeLists.txt 的部署链顺序）；\n"
            "    2) 部署条件没成立 —— 检查该文件的部署开关与本次配置是否一致；\n"
            "    3) 剔除清单误删 —— 静态检查本该拦住，检查 cmake/TrimRuntimeFiles.cmake 的三份清单。")
    endif()
endfunction()

# 作为 -P 入口被直接运行时（CI 打包场景）才执行。
# ⚠️ 不能只判断 CMAKE_SCRIPT_MODE_FILE 非空：本地构建是
#   `cmake -P DeployQtRuntime.cmake`，那个脚本 include 本文件时它同样非空。
#   只有「本文件就是那个入口脚本」才是 CI 用法。
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    # 把清单**写到文件**，不打印。
    # ⚠️ CMake 的 message() 输出到 stderr，而 CI 里 `& cmake ... | ForEach-Object`
    # 只接 stdout；且外部命令的 stderr 在 $ErrorActionPreference='Stop' 下可能被
    # 当成错误记录直接中断步骤。写文件绕开这整套语义。
    if(DEFINED PRINT_LIST)
        list(LENGTH YUMEIREN_TRIM_FILES _n)
        file(WRITE "${PRINT_LIST}" "")
        foreach(_rel IN LISTS YUMEIREN_TRIM_FILES)
            file(APPEND "${PRINT_LIST}" "${_rel}\n")
        endforeach()
        message(STATUS "已写出剔除清单 ${_n} 项 -> ${PRINT_LIST}")
        return()
    endif()

    if(NOT DEFINED TARGET_DIR)
        message(FATAL_ERROR "需要 -DTARGET_DIR=<要清理的目录>")
    endif()

    # 发布包模式：这个目录是**要发给用户的**，third_party 派生的运行期文件与 MSVC
    # 运行时都必须齐全（CI 打包那步加 -DRELEASE_PACKAGE=ON）。
    # 本地构建输出走同一条 -P 入口但**不加这个开关** —— 那边 third_party 可能合法
    # 缺席，要求 MSVC 运行时更是无从谈起（本地是 MinGW，压根没有那些 DLL）。
    if(RELEASE_PACKAGE)
        set(YUMEIREN_EXPECT_LIVE2D ON)
        set(YUMEIREN_EXPECT_WEBVIEW2 ON)
    endif()

    yumeiren_trim_runtime_files("${TARGET_DIR}")

    if(RELEASE_PACKAGE)
        set(_release_missing "")
        foreach(_keep IN LISTS YUMEIREN_KEEP_FILES_RELEASE)
            if(NOT EXISTS "${TARGET_DIR}/${_keep}")
                list(APPEND _release_missing "${_keep}")
            endif()
        endforeach()
        if(_release_missing)
            message(FATAL_ERROR "发布包缺少必需的运行期文件：${_release_missing}")
        endif()
        message(STATUS "运行时剔除完成（发布包模式：Live2D + WebView2 + MSVC 运行时 + ICU + FFmpeg 运行库齐全）")
    else()
        message(STATUS "运行时剔除完成（构建输出模式：不要求 third_party 派生件与 MSVC 运行时）")
    endif()
endif()
