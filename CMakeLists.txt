cmake_minimum_required(VERSION 3.21)

# 版本号：单一来源 + 构建期解析。规则见 cmake/Version.cmake 顶部注释。
# 必须放在 project() 之前 —— project() 的 VERSION 参数要用解析结果。
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake")
yumeiren_resolve_version()

project(Yumeiren VERSION ${YUMEIREN_VERSION_BASE} LANGUAGES CXX)

# 版本来自 VERSION 文件时会带上「提交数 + 短 sha」后缀，而 configure_file 只在
# configure 阶段求值一次。不盯着 .git/HEAD 的话，本地又提交了几次再构建，版本号
# 还是旧的 —— 而且不会有任何提示。让它一变化就自动重新 configure。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
endif()

# 把版本号灌进 app.rc / app.manifest / YumeirenVersion.h（都落在构建目录）。
yumeiren_generate_version_files()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 默认 Release：CMAKE_BUILD_TYPE 为空时 GCC 一条优化开关都拿不到(等效 -O0)，视频
# 解码回调、看板娘逐帧物理与 GL 渲染全在零优化下跑，CPU/GPU 占用与 exe 体积两头
# 吃亏。命令行显式 -DCMAKE_BUILD_TYPE=Debug 仍然优先。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")
    # Release 档的取舍：
    #   -O2 而非 GCC Release 默认的 -O3 —— 本项目没有能吃满 O3 的纯计算热点，O3 的
    #   激进内联只会徒增体积；
    #   函数/数据分节 + --gc-sections —— 链接器把没被引用到的段整个丢掉；
    #   -s —— 链接期剥离符号表，出问题靠日志排查，不靠 exe 里那几 MB 符号。
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG -ffunction-sections -fdata-sections")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -s")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

# 把目标运行时需要的 Qt 库与插件补齐到它的输出目录。
#
# 挂在 ALL 上的独立 target，而不是 TARGET 的 POST_BUILD —— 理由与 data/ 完全一样：
# POST_BUILD 只在目标真正重新链接时才跑，ninja 判 no work to do 时它根本不执行，
# 用户删掉某个 DLL 后重编译就永远补不回来。对 ffmpegmediaplugin 尤其阴：它是插件、
# 不在 exe 的导入表里，缺了它 exe 照样启动，只是视频壁纸「窗口挂上了却永远黑屏」，
# 日志里 mediaStatus 一次都不出现。细节与判据见 cmake/DeployQtRuntime.cmake 文件头。
function(yumeiren_deploy_qt_runtime target)
    # 平铺到输出目录的库：$<TARGET_RUNTIME_DLLS> 只覆盖 exe 导入表里的那些。
    set(_yumeiren_flat "$<TARGET_RUNTIME_DLLS:${target}>")
    # 需要落进子目录的插件：<源>|<目标相对路径>。
    set(_yumeiren_pairs "")

    if(TARGET Qt6::QWindowsIntegrationPlugin)
        list(APPEND _yumeiren_pairs
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>|platforms/$<TARGET_FILE_NAME:Qt6::QWindowsIntegrationPlugin>")
    endif()

    foreach(mediaPlugin Qt6::QFFmpegMediaPlugin Qt6::QWindowsMediaPlugin)
        if(TARGET ${mediaPlugin})
            list(APPEND _yumeiren_pairs
                "$<TARGET_FILE:${mediaPlugin}>|multimedia/$<TARGET_FILE_NAME:${mediaPlugin}>")
        endif()
    endforeach()

    # FFmpeg 运行库：multimedia/ffmpegmediaplugin.dll 是插件，不在 $<TARGET_RUNTIME_DLLS> 的依赖图里，
    # 而它动态依赖 Qt bin 下的 av*/sw* 这五个库。漏掉时 QLibrary 加载插件失败，Qt 只报
    # "No QtMultimedia backends found"，QMediaPlayer 根本建不起来 —— 表现是壁纸窗口挂上了却永远黑屏，
    # 且从带 Qt bin 的 PATH 启动就一切正常，极易被误判成渲染链路问题。
    set(_yumeiren_qt_bin "")
    if(TARGET Qt6::qmake)
        get_target_property(_yumeiren_qmake Qt6::qmake IMPORTED_LOCATION)
        if(_yumeiren_qmake)
            get_filename_component(_yumeiren_qt_bin "${_yumeiren_qmake}" DIRECTORY)
        endif()
    endif()
    if(NOT _yumeiren_qt_bin)
        get_filename_component(_yumeiren_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
    endif()
    file(GLOB _yumeiren_ffmpeg_dlls
        "${_yumeiren_qt_bin}/avcodec-*.dll"
        "${_yumeiren_qt_bin}/avformat-*.dll"
        "${_yumeiren_qt_bin}/avutil-*.dll"
        "${_yumeiren_qt_bin}/swresample-*.dll"
        "${_yumeiren_qt_bin}/swscale-*.dll")
    if(_yumeiren_ffmpeg_dlls)
        list(APPEND _yumeiren_flat ${_yumeiren_ffmpeg_dlls})
    else()
        message(WARNING "未在 ${_yumeiren_qt_bin} 找到 FFmpeg 运行库，视频壁纸将无法播放")
    endif()

    add_custom_target(yumeiren_deploy_runtime_${target} ALL
        COMMAND ${CMAKE_COMMAND}
                "-DDST_DIR=$<TARGET_FILE_DIR:${target}>"
                "-DFILES=${_yumeiren_flat}"
                "-DPAIRS=${_yumeiren_pairs}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployQtRuntime.cmake
        COMMENT "补齐 ${target} 的 Qt 运行时依赖"
        VERBATIM)
    # 让「只构建这一个目标」也走到补齐：ALL 只在整目录构建时才会被带上。
    add_dependencies(${target} yumeiren_deploy_runtime_${target})
endfunction()

# WebView2 Loader DLL：动态网页壁纸用。运行时本体是系统组件(Edge WebView2
# Runtime)，不随包发布；这里只补我们自己那 0.2MB 的加载器。missing 时警告但不
# 卡构建 —— 网页壁纸功能在 UI 上会以「运行时不可用」降级。
function(yumeiren_deploy_webview2 target)
    set(_wv2_loader "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/loader/WebView2Loader.dll")
    if(EXISTS "${_wv2_loader}")
        # 与 Qt 运行时/data 同一个坑同一个解法：挂 ALL 的独立 target，而不是
        # POST_BUILD —— 后者只在重新链接时才跑，删掉 DLL 后重编译永远补不回来。
        add_custom_target(yumeiren_deploy_webview2_${target} ALL
            COMMAND ${CMAKE_COMMAND}
                    "-DDST_DIR=$<TARGET_FILE_DIR:${target}>"
                    "-DFILES=${_wv2_loader}"
                    "-DPAIRS="
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployQtRuntime.cmake
            COMMENT "补齐 ${target} 的 WebView2 Loader"
            VERBATIM)
        add_dependencies(${target} yumeiren_deploy_webview2_${target})
    else()
        message(WARNING "third_party/webview2/loader/WebView2Loader.dll 缺失，动态网页壁纸将不可用")
    endif()
endfunction()

set(YUMEIREN_SOURCES
    src/main.cpp
    # app/ module
    src/app/AppInfo.cpp
    src/app/ApplicationRuntimeState.cpp
    src/app/ApplicationShutdown.cpp
    # config/ module
    src/config/AppConfig.cpp
    # core/ module(跨模块共享的缓存路径与诊断日志)
    src/core/CachePaths.cpp
    src/core/Diagnostics.cpp
    # explorerbg/ module(文件夹美化：图片背景+效果样式+使用说明)
    src/explorerbg/Engine.cpp
    src/explorerbg/ImageProcess.cpp
    src/explorerbg/ImagePage.cpp
    src/explorerbg/EffectStylePage.cpp
    # ui/ module(主窗口壳 + 共享小部件；三个模块各自的页面在对应模块目录下)
    src/ui/MainWindow.cpp
    src/ui/MediaLibraryCard.cpp
    src/ui/TooltipStyle.cpp
    # wallpaper/ module(动态壁纸：视频+网页，含壁纸页 UI)
    src/wallpaper/WallpaperPage.cpp
    src/wallpaper/VideoWallpaper.cpp
    src/wallpaper/VideoWallpaperOutput.cpp
    src/wallpaper/VideoWallpaperSettings.cpp
    src/wallpaper/WebWallpaper.cpp
    # platform/ module
    src/platform/windows/desktopmount.cpp
    src/platform/windows/globalhotkey.cpp
    src/platform/windows/shellfileops.cpp
    src/platform/windows/wakeuplistener.cpp
    src/platform/windows/webview2boot.cpp
    # tray/ module
    src/tray/SystemTrayController.cpp
    # kanban/ module
    src/kanban/KanbanAnimationClock.cpp
    src/kanban/KanbanSettingsPage.cpp
    src/kanban/KanbanController.cpp
    src/kanban/KanbanControllerInteraction.cpp
    src/kanban/KanbanControllerSettings.cpp
    src/kanban/KanbanModelManager.cpp
    src/kanban/KanbanSoftwareView.cpp
    src/kanban/KanbanStateMachine.cpp
    src/kanban/KanbanWindow.cpp
    # 模型预览图：缓存命名(ModelThumbCache)与隐藏的离屏生成模式(ModelThumbJob)。
    # 两个都在 YUMEIREN_SOURCES 里，所以正式版/测试版/探针都能用 —— 测试版能直接
    # 命令行驱动生成，不必去点界面。
    src/kanban/ModelThumbCache.cpp
    src/kanban/ModelThumbJob.cpp
    src/kanban/PlaceholderRenderer.cpp
    # 解码包装：临时抬高 Qt 的全局分配上限，好让 16384x8192 这类图集读得进来。
    # 放在共用清单里 —— 两个后端(真 Live2D / 占位渲染器)都要用它。
    src/kanban/ImageDecode.cpp
)

set(YUMEIREN_LIBS Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets dwmapi psapi shell32)

# Live2D(Cubism) 后端：SDK 受专有许可约束、不入库，所以仓库里只有解压好的
# third_party/(同样被 .gitignore 排除)。于是默认值做成「探测」而不是写死：
#   · third_party 里有 SDK 与 GLEW → 默认 ON，开箱即得真实 Live2D 看板娘；
#   · 干净的检出(两者都缺) → 默认 OFF，编占位实现，不会把构建卡死。
# 显式 -D YUMEIREN_WITH_LIVE2D=ON/OFF 永远优先。
set(YUMEIREN_LIVE2D_DEFAULT OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cubism/Framework/src/CubismFramework.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew/src/glew.c")
    set(YUMEIREN_LIVE2D_DEFAULT ON)
endif()
option(YUMEIREN_WITH_LIVE2D "编译 Live2D Cubism 看板娘后端" ${YUMEIREN_LIVE2D_DEFAULT})
if(YUMEIREN_WITH_LIVE2D)
    enable_language(C)
    # SDK / GLEW / Core 的构建细节都在这个模块里，主文件只负责接线。
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Live2DCubism.cmake")
    yumeiren_live2d_dependencies(YUMEIREN_LIVE2D_SOURCES
        YUMEIREN_LIVE2D_LIBS YUMEIREN_CUBISM_RUNTIME_DLLS)
    list(APPEND YUMEIREN_SOURCES ${YUMEIREN_LIVE2D_SOURCES})
    list(APPEND YUMEIREN_LIBS ${YUMEIREN_LIVE2D_LIBS})
else()
    list(APPEND YUMEIREN_SOURCES src/kanban/Live2DRendererStub.cpp)
endif()

qt_add_executable(Yumeiren WIN32
    ${YUMEIREN_SOURCES}
    ${YUMEIREN_GENERATED_RC}
)

target_include_directories(Yumeiren PRIVATE src third_party/webview2/include)
yumeiren_apply_version(Yumeiren)

set_property(SOURCE "${YUMEIREN_GENERATED_RC}" APPEND PROPERTY
    OBJECT_DEPENDS "${YUMEIREN_ICON_PATH}")

target_link_libraries(Yumeiren PRIVATE ${YUMEIREN_LIBS})
if(YUMEIREN_WITH_LIVE2D)
    # 窗口层(KanbanWindow)靠这个宏决定是否创建 GL 宿主视图；
    # 不开宏时即使链了 Cubism 也永远不会走 GL 路径。
    target_compile_definitions(Yumeiren PRIVATE YUMEIREN_WITH_LIVE2D=1)
    yumeiren_deploy_cubism(Yumeiren)
endif()
yumeiren_deploy_qt_runtime(Yumeiren)
yumeiren_deploy_webview2(Yumeiren)

set_target_properties(Yumeiren PROPERTIES
    OUTPUT_NAME "虞美人"
)

# MSVC 会**自己生成并嵌入**一个 manifest（链接行里能看到 `/MANIFEST:EMBED,ID=1`），而
# resources/app.rc.in 里已经有一个 `1 24 "app.manifest"` —— 类型同为 RT_MANIFEST、ID 同为 1，
# CVTRES 直接报 `CVT1100: duplicate resource. type:MANIFEST, name:1`，链接随之
# `LNK1123: failure during conversion to COFF` 失败。
#
# 为什么本机一直没暴露：本机两个 build 目录都是 MinGW（CLion 自带 g++ + Ninja），
# windres/ld 不做这件事；CI 是第一次用 MSVC 编，一编就撞上。
#
# 关掉链接器自己那份，让 resources/app.manifest.in 成为唯一来源 —— 它本来就承载着
# assemblyIdentity 版本、requestedExecutionLevel、DPI 感知这些项目自己的意图。
# 实测（VS 18 + Ninja 的最小复现，rc 里放一条 `1 24 "x.manifest"`）：
#   不加 → CVT1100 + LNK1123；加了 → 链接通过，且用 mt.exe 从 exe 里抽出 RT_MANIFEST，
#   内容正是项目自己的 manifest（不是链接器生成的通用那份）。
if(MSVC)
    target_link_options(Yumeiren PRIVATE "/MANIFEST:NO")
endif()

qt_add_executable(YumeirenTest WIN32
    ${YUMEIREN_SOURCES}
)
target_include_directories(YumeirenTest PRIVATE src third_party/webview2/include)
yumeiren_apply_version(YumeirenTest)
target_link_libraries(YumeirenTest PRIVATE ${YUMEIREN_LIBS})
if(YUMEIREN_WITH_LIVE2D)
    target_compile_definitions(YumeirenTest PRIVATE YUMEIREN_WITH_LIVE2D=1)
    yumeiren_deploy_cubism(YumeirenTest)
endif()
yumeiren_deploy_qt_runtime(YumeirenTest)
yumeiren_deploy_webview2(YumeirenTest)

set(APP_RESOURCES
    resources/style.qss
    resources/light.qss
    resources/dlls/ExplorerBgTool.dll
    resources/dlls/ExplorerBlurMica.dll
    resources/icons/yumeiren-16.png
    resources/icons/yumeiren-20.png
    resources/icons/yumeiren-24.png
    resources/icons/yumeiren-32.png
    resources/icons/yumeiren-48.png
    resources/icons/yumeiren-64.png
    resources/icons/yumeiren-128.png
    resources/icons/yumeiren-256.png
)

# 复制 data/ 目录到编译输出目录。
# 挂在 ALL 上而不是某个 target 的 POST_BUILD：POST_BUILD 只在目标真正重新链接时才跑，
# ninja 判定 no work to do 时它根本不执行 —— 用户删掉 build/data 后重编译就永远补不回来。
# 判据是「逐文件补齐」而非「整个 data 不存在」：别的构建步骤只要先建出 build/data/models，
# 整目录判据就会被顶掉，结果只剩 models 一份。
add_custom_target(yumeiren_runtime_data ALL
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data
            -DDST_DIR=${CMAKE_BINARY_DIR}/data
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeData.cmake
    COMMENT "补齐 data/ 到编译输出目录（已有文件不动）"
    VERBATIM
)

qt_add_resources(Yumeiren app_res
    PREFIX "/"
    BASE resources
    FILES ${APP_RESOURCES}
)

qt_add_resources(YumeirenTest app_res
    PREFIX "/"
    BASE resources
    FILES ${APP_RESOURCES}
)


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/sinkprobe/main.cpp")
    qt_add_executable(SinkProbe WIN32
        tools/sinkprobe/main.cpp
    )
    target_link_libraries(SinkProbe PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets)
endif()

# 看板娘渲染管线自检探针(非产品组件，可整体删除)。
# 存在的理由：Live2D 出问题的表现是「桌面上什么都没有」，没有异常也没有日志；
# 这个探针把同一条渲染链路搬到离屏上下文里跑，把结论变成可判读的文本和一张 PNG。
# 刻意不加 WIN32：它是控制台程序，输出要能直接被脚本读走。
# 只在 Live2D 后端进构建时才成立 —— 没有 SDK 时它自己也无从下手。
if(YUMEIREN_WITH_LIVE2D AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/kanbanprobe/main.cpp")
    set(YUMEIREN_PROBE_SOURCES ${YUMEIREN_SOURCES})
    list(REMOVE_ITEM YUMEIREN_PROBE_SOURCES src/main.cpp)
    add_executable(KanbanProbe ${YUMEIREN_PROBE_SOURCES} tools/kanbanprobe/main.cpp)
    target_include_directories(KanbanProbe PRIVATE src third_party/webview2/include)
    yumeiren_apply_version(KanbanProbe)
    target_link_libraries(KanbanProbe PRIVATE ${YUMEIREN_LIBS})
    target_compile_definitions(KanbanProbe PRIVATE YUMEIREN_WITH_LIVE2D=1)
    yumeiren_deploy_cubism(KanbanProbe)
    yumeiren_deploy_qt_runtime(KanbanProbe)
yumeiren_deploy_webview2(KanbanProbe)
    # 不单独复制 data/models：它与上面的 yumeiren_runtime_data 抢同一个目标目录，
    # 先建出 build/data 就会把整份 data 的补齐判据顶掉。
    add_dependencies(KanbanProbe yumeiren_runtime_data)
endif()
