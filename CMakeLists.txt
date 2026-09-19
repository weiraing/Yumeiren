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

if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

function(yumeiren_deploy_qt_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        COMMENT "复制 ${target} 的 Qt 运行时依赖"
        VERBATIM)

    if(TARGET Qt6::QWindowsIntegrationPlugin)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target}>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
                "$<TARGET_FILE_DIR:${target}>/platforms"
            COMMENT "复制 Qt 平台插件"
            VERBATIM)
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
    # core/ module
    src/core/CachePaths.cpp
    src/core/Diagnostics.cpp
    src/core/ImageProcess.cpp
    # engine/ module
    src/engine/Engine.cpp
    # ui/ module
    src/ui/MainWindow.cpp
    src/ui/MainWindowImagePage.cpp
    src/ui/MainWindowEffectPage.cpp
    src/ui/MainWindowVideoPage.cpp
    src/ui/MainWindowKanbanPage.cpp
    src/ui/TooltipStyle.cpp
    # wallpaper/ module
    src/wallpaper/VideoWallpaper.cpp
    src/wallpaper/VideoWallpaperOutput.cpp
    src/wallpaper/VideoWallpaperSettings.cpp
    # platform/ module
    src/platform/windows/desktopmount.cpp
    src/platform/windows/shellfileops.cpp
    # tray/ module
    src/tray/SystemTrayController.cpp
    # kanban/ module
    src/kanban/KanbanAnimationClock.cpp
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

target_include_directories(Yumeiren PRIVATE src)
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

set_target_properties(Yumeiren PROPERTIES
    OUTPUT_NAME "虞美人"
)

qt_add_executable(YumeirenTest WIN32
    ${YUMEIREN_SOURCES}
)
target_include_directories(YumeirenTest PRIVATE src)
yumeiren_apply_version(YumeirenTest)
target_link_libraries(YumeirenTest PRIVATE ${YUMEIREN_LIBS})
if(YUMEIREN_WITH_LIVE2D)
    target_compile_definitions(YumeirenTest PRIVATE YUMEIREN_WITH_LIVE2D=1)
    yumeiren_deploy_cubism(YumeirenTest)
endif()
yumeiren_deploy_qt_runtime(YumeirenTest)

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

# 复制 data/ 目录到编译输出目录（仅当 build/data 整个不存在时触发）
add_custom_command(TARGET Yumeiren POST_BUILD
    COMMAND powershell -Command "if (!(Test-Path '${CMAKE_BINARY_DIR}/data')) { Copy-Item -Path '${CMAKE_SOURCE_DIR}/data' -Destination '${CMAKE_BINARY_DIR}/data' -Recurse; Write-Host 'Copied data/ to build directory' } else { Write-Host 'data/ already exists in build directory' }"
    COMMENT "Copying data/ directory to build output if needed"
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
    target_include_directories(KanbanProbe PRIVATE src)
    yumeiren_apply_version(KanbanProbe)
    target_link_libraries(KanbanProbe PRIVATE ${YUMEIREN_LIBS})
    target_compile_definitions(KanbanProbe PRIVATE YUMEIREN_WITH_LIVE2D=1)
    yumeiren_deploy_cubism(KanbanProbe)
    yumeiren_deploy_qt_runtime(KanbanProbe)
    add_custom_command(TARGET KanbanProbe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/models" "$<TARGET_FILE_DIR:KanbanProbe>/data/models"
        COMMENT "复制模型目录到探针输出目录"
        VERBATIM)
endif()
