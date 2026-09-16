# Live2D Cubism Native SDK(OpenGL 后端) 的第三方构建配置。
#
# 为什么单独一个文件：SDK 与 GLEW 都受各自许可约束、且不进仓库，
# 这里的逻辑只在 -D YUMEIREN_WITH_LIVE2D=ON 时才需要读，主 CMakeLists 保持干净。
#
# 产出两个静态库 + 一个 Core 导入库，并把运行期 DLL 复制到可执行文件旁边：
#   yumeiren_glew            —— GLEW 静态库(只取 Cubism 需要的类型与扩展标志)
#   yumeiren_cubism_framework —— Cubism Framework 静态库(仅 OpenGL 渲染路径)
#   Live2DCubismCore          —— SHARED IMPORTED(MinGW 导入库 + 运行期 DLL)
#
# 关键取舍(都已对着 SDK 源码核对)：
#   1. 不用 SDK 自带的 CMake：它的 Framework/CMakeLists.txt 依赖一堆
#      FRAMEWORK_* 缓存变量与平台目录推导，MinGW 下改动量比我们自己 glob 还大；
#   2. 只要 OpenGL 渲染路径：Rendering/{D3D9,D3D11,Metal,Vulkan} 整体排除；
#   3. GL 上下文由 Qt(QOpenGLWidget)提供，所以不需要 GLFW；
#   4. Core 官方只给 MSVC 的 .lib。MinGW 用 gendef+dlltool 现场生成导入库，
#      避免去链 MSVC 静态库(CRT 不匹配)。

set(YUMEIREN_CUBISM_SDK "C:/Users/rain/Documents/ExplorerBg/sdk/CubismSdkForNative-5-r.5"
    CACHE PATH "Cubism Native SDK 根目录(需含 Framework/ 与 Core/)")
set(YUMEIREN_GLEW_ROOT "C:/Users/rain/Documents/ExplorerBg/ref/QtLive2dDesktop-master/live2d/glew"
    CACHE PATH "GLEW 源码根目录(需含 include/GL/glew.h 与 src/glew.c)")

function(yumeiren_live2d_dependencies out_sources out_libraries out_dlls)
    set(${out_sources} src/kanban/KanbanOpenGLView.cpp src/kanban/Live2DRendererCubism.cpp
        PARENT_SCOPE)

    if(NOT EXISTS "${YUMEIREN_CUBISM_SDK}/Framework/src/CubismFramework.cpp")
        message(FATAL_ERROR
            "YUMEIREN_WITH_LIVE2D=ON，但在 ${YUMEIREN_CUBISM_SDK} 没找到 Cubism Framework。"
            "请用 -D YUMEIREN_WITH_LIVE2D=<SDK 路径> 指向解压后的 SDK。")
    endif()
    if(NOT EXISTS "${YUMEIREN_GLEW_ROOT}/src/glew.c")
        message(FATAL_ERROR
            "在 ${YUMEIREN_GLEW_ROOT} 没找到 GLEW 源码(Cubism 的 Windows GL 渲染器硬依赖它)。")
    endif()

    find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets)

    # C 语言由调用方在目录作用域 enable_language(C) 打开。
    # 不能在这里调用：函数作用域里 enable_language 设的 CMAKE_C_COMPILE_OBJECT
    # 等规则变量会随函数返回一起消失，生成阶段直接报缺变量。

    # —— GLEW：单文件静态库 ——
    if(NOT TARGET yumeiren_glew)
        add_library(yumeiren_glew STATIC "${YUMEIREN_GLEW_ROOT}/src/glew.c")
        set_target_properties(yumeiren_glew PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        target_compile_definitions(yumeiren_glew PUBLIC GLEW_STATIC PRIVATE GLEW_NO_GLU)
        # SYSTEM：第三方头文件的警告不该混进我们的构建日志。
        target_include_directories(yumeiren_glew SYSTEM PUBLIC "${YUMEIREN_GLEW_ROOT}/include")
        if(WIN32)
            target_link_libraries(yumeiren_glew PUBLIC opengl32)
        endif()
    endif()

    # —— Core：MinGW 导入库(gendef + dlltool) ——
    set(_core_dll "${YUMEIREN_CUBISM_SDK}/Core/dll/windows/x86_64/Live2DCubismCore.dll")
    if(NOT EXISTS "${_core_dll}")
        message(FATAL_ERROR "没找到 64 位 Core 运行库：${_core_dll}")
    endif()
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(YUMEIREN_GENEDEF gendef HINTS "${_mingw_bin}")
    find_program(YUMEIREN_DLLTOOL dlltool HINTS "${_mingw_bin}")
    if(NOT YUMEIREN_GENEDEF OR NOT YUMEIREN_DLLTOOL)
        message(FATAL_ERROR
            "需要 gendef 与 dlltool 才能给 MinGW 生成 Live2DCubismCore 的导入库，"
            "请在 MinGW 的 bin 目录里找到它们(通常随编译器一起装好)。")
    endif()

    set(_core_lib_dir "${CMAKE_BINARY_DIR}/cubism-core")
    file(MAKE_DIRECTORY "${_core_lib_dir}")
    set(_core_lib "${_core_lib_dir}/libLive2DCubismCore.a")
    if(NOT EXISTS "${_core_lib}")
        execute_process(COMMAND "${YUMEIREN_GENEDEF}" "${_core_dll}"
            WORKING_DIRECTORY "${_core_lib_dir}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "gendef 解析 ${_core_dll} 失败")
        endif()
        execute_process(COMMAND "${YUMEIREN_DLLTOOL}"
            -d Live2DCubismCore.def -l libLive2DCubismCore.a -D Live2DCubismCore.dll
            WORKING_DIRECTORY "${_core_lib_dir}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "dlltool 生成 Core 导入库失败")
        endif()
    endif()

    if(NOT TARGET Live2DCubismCore)
        add_library(Live2DCubismCore SHARED IMPORTED GLOBAL)
        set_target_properties(Live2DCubismCore PROPERTIES
            IMPORTED_IMPLIB "${_core_lib}"
            IMPORTED_LOCATION "${_core_dll}")
    endif()

    # —— Framework：只用 OpenGL 渲染路径 ——
    if(NOT TARGET yumeiren_cubism_framework)
        file(GLOB_RECURSE _cubism_sources CONFIGURE_DEPENDS
            "${YUMEIREN_CUBISM_SDK}/Framework/src/*.cpp")
        set(_cubism_kept "")
        foreach(_f ${_cubism_sources})
            if(_f MATCHES "/Rendering/(D3D9|D3D11|Metal|Vulkan)/")
                continue()
            endif()
            list(APPEND _cubism_kept "${_f}")
        endforeach()

        add_library(yumeiren_cubism_framework STATIC ${_cubism_kept})
        set_target_properties(yumeiren_cubism_framework PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_STANDARD 17)
        target_compile_definitions(yumeiren_cubism_framework PUBLIC CSM_TARGET_WIN_GL)
        target_include_directories(yumeiren_cubism_framework
            PUBLIC "${YUMEIREN_CUBISM_SDK}/Framework/src"
            PUBLIC "${YUMEIREN_CUBISM_SDK}/Core/include")
        target_link_libraries(yumeiren_cubism_framework
            PUBLIC yumeiren_glew Live2DCubismCore)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # Core 的 wglGetProcAddress 返回值在 MinGW 下是函数指针，SDK 直接赋给
            # void*；这是 SDK 自身的写法问题，不能改 SDK，只在第三方目标上放行。
            target_compile_options(yumeiren_cubism_framework PRIVATE -fpermissive -w)
        elseif(MSVC)
            target_compile_options(yumeiren_cubism_framework PRIVATE /utf-8 /w)
        endif()
    endif()

    set(${out_libraries} Qt6::OpenGL Qt6::OpenGLWidgets yumeiren_cubism_framework
        PARENT_SCOPE)
    set(${out_dlls} "${_core_dll}" PARENT_SCOPE)
endfunction()

# 把 Core 运行库复制到可执行文件旁边：Live2DCubismCore.dll 不复制进去，
# 程序会在装载模型时直接崩在导出符号解析上，且报错信息毫无参考价值。
function(yumeiren_deploy_cubism target)
    if(NOT YUMEIREN_CUBISM_RUNTIME_DLLS)
        return()
    endif()
    foreach(_dll ${YUMEIREN_CUBISM_RUNTIME_DLLS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" $<TARGET_FILE_DIR:${target}>
            COMMENT "复制 Live2DCubismCore.dll"
            VERBATIM)
    endforeach()
endfunction()
