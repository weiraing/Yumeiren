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

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

set(YUMEIREN_SOURCES
    src/main.cpp
    src/appinfo.cpp
    src/mainwindow.cpp
    src/imageprocess.cpp
    src/engine.cpp
    src/videowallpaper.cpp
)

qt_add_executable(Yumeiren WIN32
    ${YUMEIREN_SOURCES}
    resources/app.rc
)

target_include_directories(Yumeiren PRIVATE src)

target_link_libraries(Yumeiren PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets dwmapi)

set_target_properties(Yumeiren PROPERTIES
    OUTPUT_NAME "Yumeiren"
)

# UI-test build without the requireAdministrator manifest (can be automated).
qt_add_executable(YumeirenTest WIN32
    ${YUMEIREN_SOURCES}
)
target_include_directories(YumeirenTest PRIVATE src)
target_link_libraries(YumeirenTest PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets dwmapi)

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
