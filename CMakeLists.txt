cmake_minimum_required(VERSION 3.21)

project(Yumeiren VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# The sources are UTF-8 and most UI strings are Chinese; MSVC needs to be told,
# otherwise it falls back to the system ANSI codepage.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# app.rc is UTF-8 too; windres otherwise decodes it with the ANSI codepage and
# the version-info strings (FileDescription/ProductName) turn into mojibake.
if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

# 把本 target 真正需要的 Qt 模块 DLL 复制到可执行文件旁边。
#
# 为什么不直接跑 windeployqt：那工具会顺带打包 platforms/、imageformats/、
# multimedia/ 等一堆插件目录，每次构建都重扫一遍，开发期太慢。这里只搬 Qt 的
# 模块 DLL(插件目录仍需 windeployqt 或手工准备一次)。
#
# 为什么用 $<TARGET_RUNTIME_DLLS> 而不是遍历链接库清单：早先的写法只搬「链接行
# 上点名的那几个 Qt6:: 目标」，而 Qt6Core / Qt6Gui 是 Qt6::Widgets 的**传递**
# 依赖，不在名单里 —— 于是干净的构建目录里双击 exe 毫无反应、退出码 127，
# 正是下面警告的那种情况。生成器表达式会把传递依赖一并列出，顺带也让 Live2D
# 打开时新增的 Qt6OpenGL / Qt6OpenGLWidgets 自动跟上，不必再手工维护清单。
#
# 为什么必须有这一步：Live2D 打开后依赖里会多出 Qt6OpenGL.dll 与
# Qt6OpenGLWidgets.dll。开发目录里若只手工抄过 Qt6Widgets 那一批 DLL，
# 新依赖缺席的表现是「编译全绿、双击没反应」，退出码 127 —— 没有任何提示，
# 很难往「少了个 DLL」上想。
function(yumeiren_deploy_qt_runtime target)
    # COMMAND_EXPAND_LISTS 是必须的：$<TARGET_RUNTIME_DLLS> 求值出来是分号分隔的
    # 列表，不加这个修饰，它会被当成「一个」文件名传给 copy_if_different，
    # 报 "Error copying file ... from A.dll;B.dll;C.dll"。
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        COMMENT "复制 ${target} 的 Qt 运行时依赖"
        VERBATIM)

    # 平台插件是「能跑」与「双击没反应」的分界线：少了 platforms/qwindows.dll，
    # Qt 连一个窗口都开不出来。其余插件(styles/imageformats/tls)不影响启动，
    # 交给发布脚本或 windeployqt 处理。
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
    # tray/ module
    src/tray/SystemTrayController.cpp
    # kanban/ module
    src/kanban/KanbanAnimationClock.cpp
    src/kanban/KanbanController.cpp
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
)

# 两个产品目标共用的依赖清单；Live2D 开启时在下面追加。
set(YUMEIREN_LIBS Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets dwmapi psapi)

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
    # 工程只声明了 CXX，但 GLEW 是 .c。必须在目录作用域打开 C 语言，
    # 放进函数里调用会让 CMake 的 C 编译规则变量随函数返回一起丢失。
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
    resources/app.rc
)

target_include_directories(Yumeiren PRIVATE src)

target_link_libraries(Yumeiren PRIVATE ${YUMEIREN_LIBS})
if(YUMEIREN_WITH_LIVE2D)
    # 窗口层(KanbanWindow)靠这个宏决定是否创建 GL 宿主视图；
    # 不开宏时即使链了 Cubism 也永远不会走 GL 路径。
    target_compile_definitions(Yumeiren PRIVATE YUMEIREN_WITH_LIVE2D=1)
    yumeiren_deploy_cubism(Yumeiren)
endif()
yumeiren_deploy_qt_runtime(Yumeiren)

set_target_properties(Yumeiren PROPERTIES
    OUTPUT_NAME "Yumeiren"
)

# UI-test build without the requireAdministrator manifest (can be automated).
qt_add_executable(YumeirenTest WIN32
    ${YUMEIREN_SOURCES}
)
target_include_directories(YumeirenTest PRIVATE src)
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
)

# 复制 data/ 目录到编译输出目录（仅当 build/data/models 不存在时触发）
add_custom_command(TARGET Yumeiren POST_BUILD
    COMMAND powershell -Command "if (!(Test-Path '${CMAKE_BINARY_DIR}/data/models')) { Remove-Item -Path '${CMAKE_BINARY_DIR}/data' -Recurse -ErrorAction SilentlyContinue; Copy-Item -Path '${CMAKE_SOURCE_DIR}/data' -Destination '${CMAKE_BINARY_DIR}/data' -Recurse; Write-Host 'Copied data/ to build directory' } else { Write-Host 'data/ already exists in build directory' }"
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

# QVideoSink A/B 实验探针(第二阶段内存归因；非产品组件，可整体删除)。
# 探针源码不在仓库里时跳过该目标，产品目标 Yumeiren / YumeirenTest 照常构建。
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
