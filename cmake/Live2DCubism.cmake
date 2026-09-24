# Live2D Cubism Native SDK(OpenGL 后端) 的第三方构建配置。
#
# 为什么单独一个文件：SDK 与 GLEW 都受各自许可约束、且不进仓库，
# 这里的逻辑只在 -D YUMEIREN_WITH_LIVE2D=ON 时才需要读，主 CMakeLists 保持干净。
#
# 产出两个静态库 + Core 运行库的接线，并把运行期 DLL 复制到可执行文件旁边：
#   yumeiren_glew             —— GLEW 静态库(只取 Cubism 需要的类型与扩展标志)
#   yumeiren_cubism_framework —— Cubism Framework 静态库(仅 OpenGL 渲染路径)
#   Live2DCubismCore          —— INTERFACE 目标，把 Core 的 DLL 与头文件带下去
#
# 关键取舍(都已对着 SDK 源码与本地工具链核对)：
#   1. 不用 SDK 自带的 CMake：它的 Framework/CMakeLists.txt 依赖一堆
#      FRAMEWORK_* 缓存变量与平台目录推导，MinGW 下改动量比我们自己 glob 还大；
#   2. 只要 OpenGL 渲染路径：Rendering/{D3D9,D3D11,Metal,Vulkan} 整体排除；
#   3. GL 上下文由 Qt(QOpenGLWidget)提供，所以不需要 GLFW；
#   4. Core 官方只给 MSVC 的 .lib/.dll，两种编译器各有各的接法：MSVC 直接链 .lib；
#      MinGW 下不去链 MSVC 导入库(CRT 不匹配)，也刻意不用 gendef+dlltool 现场生成
#      .a —— 那套工具在部分 MinGW 发行版里并不随编译器安装(本机 CLion 自带的那份
#      就没有)。改成把 .dll 直接放到链接行上：GNU ld 会自己读 PE 导出表，效果与
#      导入库完全一致。分岔点见下面「Core：链接方式按编译器分岔」。

# 默认指向仓库内已解压的 third_party/(见 docs/dev/REFACTORING_GUIDE.md 的目录规范)。
# 两者都可用 -D 覆盖，指向仓库外的 SDK/GLEW 副本也能工作。
set(YUMEIREN_CUBISM_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cubism"
    CACHE PATH "Cubism Native SDK 根目录(需含 Framework/ 与 Core/)")
set(YUMEIREN_GLEW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew"
    CACHE PATH "GLEW 源码根目录(需含 include/GL/glew.h 与 src/glew.c)")

function(yumeiren_live2d_dependencies out_sources out_libraries out_dlls)
    set(${out_sources}
        src/kanban/KanbanOpenGLView.cpp
        src/kanban/Live2DRendererCubism.cpp
        src/kanban/CubismRuntime.cpp
        src/kanban/CubismModel.cpp
        src/kanban/CubismModelGl.cpp
        src/kanban/CubismModelInteraction.cpp
        PARENT_SCOPE)

    if(NOT EXISTS "${YUMEIREN_CUBISM_SDK}/Framework/src/CubismFramework.cpp")
        message(FATAL_ERROR
            "YUMEIREN_WITH_LIVE2D=ON，但在 ${YUMEIREN_CUBISM_SDK} 没找到 Cubism Framework。"
            "请把 SDK 解压到 third_party/cubism，或用 -D YUMEIREN_CUBISM_SDK=<SDK 路径> 指定。")
    endif()
    if(NOT EXISTS "${YUMEIREN_GLEW_ROOT}/src/glew.c")
        message(FATAL_ERROR
            "在 ${YUMEIREN_GLEW_ROOT} 没找到 GLEW 源码(Cubism 的 Windows GL 渲染器硬依赖它)。"
            "请把 GLEW 源码放到 third_party/glew，或用 -D YUMEIREN_GLEW_ROOT=<GLEW 路径> 指定。")
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

    # —— Core：链接方式按编译器分岔 ——
    # 官方在同一个目录里同时给了运行库与 MSVC 导入库，两种编译器各取所需：
    #   MSVC —— 用 .lib，这是 link.exe 的标准输入；
    #   GNU  —— 直接给 .dll，让 ld 读 PE 导出表(见文件头第 4 条；MinGW 下链 MSVC
    #           的 .lib 会因 CRT 不匹配出问题)。
    # 两边都必须是绝对路径，否则会被当成 -l 参数去找同名库。
    # ⚠️ MSVC 分支是给 CI 用的(CI 跑 win64_msvc2022_64)，本机只有 MinGW，所以那条路
    # 只在 CI 上验证过 —— 本地改动这里时别只跑 MinGW 就以为两条路都好。
    set(_core_dir "${YUMEIREN_CUBISM_SDK}/Core/dll/windows/x86_64")
    set(_core_dll "${_core_dir}/Live2DCubismCore.dll")
    set(_core_lib "${_core_dir}/Live2DCubismCore.lib")
    if(NOT EXISTS "${_core_dll}")
        message(FATAL_ERROR "没找到 64 位 Core 运行库：${_core_dll}")
    endif()

    if(MSVC AND EXISTS "${_core_lib}")
        set(_core_link "${_core_lib}")
    else()
        set(_core_link "${_core_dll}")
    endif()

    if(NOT TARGET Live2DCubismCore)
        add_library(Live2DCubismCore INTERFACE)
        target_include_directories(Live2DCubismCore INTERFACE
            "${YUMEIREN_CUBISM_SDK}/Core/include")
        target_link_libraries(Live2DCubismCore INTERFACE "${_core_link}")
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
    set(YUMEIREN_CUBISM_SHADER_DIR
        "${YUMEIREN_CUBISM_SDK}/Framework/src/Rendering/OpenGL/Shaders/Standard"
        PARENT_SCOPE)
endfunction()

# 运行期需要两样东西躺在可执行文件旁边，少一样都是「编译通过但一片空白」：
#
#   1. Live2DCubismCore.dll —— 不复制，程序会在装载模型时直接崩在导出符号解析上，
#      且报错信息毫无参考价值；
#   2. FrameworkShaders/   —— Cubism 的 GL 着色器不在代码里，而在运行期由
#      CubismShader_OpenGLES2::GenerateShaders() 逐个读盘(见
#      Framework/src/Rendering/OpenGL/CubismShader_OpenGLES2.cpp 里的
#      "FrameworkShaders/VertShaderSrc.vert" 等常量)。缺目录时它只往日志里写
#      "Failed to load vertex shader"，然后拿着 ShaderProgram=0 一路画下去 ——
#      界面上什么都看不到，也不报错。
#      官方示例 proj.win.cmake/CMakeLists.txt 里有同样的 copy_directory 步骤。
#
# 挂在 ALL 上的**单个**共享 target，而不是每个目标的 POST_BUILD，两个理由：
#   1. POST_BUILD 只在目标真正重新链接时才跑 —— 用户删掉 FrameworkShaders/ 之后
#      重编译永远补不回来，而症状正是上面说的「一片空白且不报错」。与 data/、
#      Qt 运行库是同一类坑，用同一种解法。
#   2. 三个 exe 的输出目录相同，各自 POST_BUILD 会**并发**往同一个
#      FrameworkShaders/ 里 copy_directory，实测会随机报
#      "Error copying directory from ... to ..." 把整次构建打挂。只做一次就没有这个问题。
#
# 前提：调用本函数的各目标输出目录一致(本仓库三个 exe 都在 build/ 根)。不一致会
# 直接 FATAL_ERROR 报出来，而不是悄悄把库补到别人的目录里。
function(yumeiren_deploy_cubism target)
    if(NOT YUMEIREN_CUBISM_RUNTIME_DLLS)
        return()
    endif()

    get_target_property(_yumeiren_cubism_out ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _yumeiren_cubism_out)
        set(_yumeiren_cubism_out "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    get_property(_yumeiren_cubism_dir GLOBAL PROPERTY YUMEIREN_CUBISM_DEPLOY_DIR)
    if(NOT _yumeiren_cubism_dir)
        set_property(GLOBAL PROPERTY YUMEIREN_CUBISM_DEPLOY_DIR "${_yumeiren_cubism_out}")
    elseif(NOT _yumeiren_cubism_dir STREQUAL _yumeiren_cubism_out)
        message(FATAL_ERROR
            "Cubism 运行期文件的复制只做一份，但 ${target} 的输出目录"
            "(${_yumeiren_cubism_out})与先前目标的(${_yumeiren_cubism_dir})不同。"
            "请给输出目录不同的目标另建一个复制 target。")
    endif()

    if(NOT TARGET yumeiren_runtime_cubism)
        # 命令直接写进 add_custom_target：POST_BUILD 只对「有构建步骤」的目标成立，
        # 挂在自定义 target 上永远不会执行。
        set(_yumeiren_cubism_cmds
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_yumeiren_cubism_out}")

        foreach(_dll ${YUMEIREN_CUBISM_RUNTIME_DLLS})
            list(APPEND _yumeiren_cubism_cmds
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "${_yumeiren_cubism_out}")
        endforeach()

        if(YUMEIREN_CUBISM_SHADER_DIR AND EXISTS "${YUMEIREN_CUBISM_SHADER_DIR}")
            list(APPEND _yumeiren_cubism_cmds
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${YUMEIREN_CUBISM_SHADER_DIR}"
                    "${_yumeiren_cubism_out}/FrameworkShaders")
        else()
            message(WARNING
                "没找到 Cubism 的 Standard 着色器目录，Live2D 会加载失败并画不出东西："
                "${YUMEIREN_CUBISM_SHADER_DIR}")
        endif()

        add_custom_target(yumeiren_runtime_cubism ALL ${_yumeiren_cubism_cmds}
            COMMENT "复制 Cubism 运行期文件(Core DLL + FrameworkShaders/)"
            VERBATIM)
    endif()

    # 让「只构建这一个目标」也走到复制：ALL 只在整目录构建时才会被带上。
    add_dependencies(${target} yumeiren_runtime_cubism)
endfunction()
