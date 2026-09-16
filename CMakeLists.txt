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

set(YUMEIREN_SOURCES
    src/main.cpp
    src/appinfo.cpp
    src/core/CachePaths.cpp
    src/config/AppConfig.cpp
    src/mainwindow.cpp
    src/tooltipstyle.cpp
    src/imageprocess.cpp
    src/engine.cpp
    src/videowallpaper.cpp
    src/videodiag.cpp
    src/platform/windows/desktopmount.cpp
    src/app/ApplicationRuntimeState.cpp
    src/app/ApplicationShutdown.cpp
    src/tray/SystemTrayController.cpp
    src/kanban/KanbanAnimationClock.cpp
    src/kanban/KanbanController.cpp
    src/kanban/KanbanModelManager.cpp
    src/kanban/KanbanSoftwareView.cpp
    src/kanban/KanbanStateMachine.cpp
    src/kanban/KanbanWindow.cpp
    src/kanban/PlaceholderRenderer.cpp
)

# 两个产品目标共用的依赖清单；Live2D 开启时在下面追加。
set(YUMEIREN_LIBS Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets dwmapi psapi)

# Live2D(Cubism) 后端默认不进构建：SDK 受专有许可约束，不入库。
# -D YUMEIREN_WITH_LIVE2D=ON 时编译真实渲染器与 GL 宿主窗口，
# 否则编译占位实现(接口一致，运行时上报「不可用」并自动降级)。
option(YUMEIREN_WITH_LIVE2D "编译 Live2D Cubism 看板娘后端" OFF)
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

set(APP_RESOURCES
    resources/style.qss
    resources/light.qss
    resources/dlls/ExplorerBgTool.dll
    resources/dlls/ExplorerBlurMica.dll
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
