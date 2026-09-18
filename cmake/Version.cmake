# 版本号的唯一解析入口。
#
# 为什么要有这个文件：在此之前版本号**散在 5 个地方** —— CMakeLists 的 project()、
# app.rc 里的 FILEVERSION / PRODUCTVERSION / FileVersion / ProductVersion、
# app.manifest 的 assemblyIdentity、以及 main.cpp 的 setApplicationVersion()。
# 发一次版要手工改五处，漏一处就出现「文件属性写着 1.0.0、日志里写着 1.0.1」这种
# 自相矛盾的状态。现在统一成「构建期解析一次，各处跟着走」。
#
# 解析优先级（从高到低）：
#   1. -DYUMEIREN_VERSION=1.2.3 或环境变量 YUMEIREN_VERSION
#      —— CI 用它把 git 标签号传进来；也是「我想临时编个别的号」的出口。
#   2. git 标签恰好指向 HEAD（v1.2.3 → 1.2.3）
#      —— 在标签上直接构建时自动生效，不用任何额外参数。
#   3. 仓库根目录的 VERSION 文件
#      —— 开发期的默认值，也是版本号真正的「源头」，发版时手工改这一行。
#
# 本地开发版后缀：当版本来自 VERSION 文件（即「不是标签、也不是显式指定」）时，
# 完整版本串会追加 `+<提交数>.g<短sha>[.dirty]`，例如：
#
#   1.0.0              ← 在 v1.0.0 标签上构建（发布版）
#   1.0.0+67.g3f9a1c2  ← 标签之后又提交了 7 次（本地开发版）
#   1.0.0+67.g3f9a1c2.dirty  ← 且工作区还有未提交改动
#
# 这样日志和文件属性里一眼就能分出「这是发布版」还是「这是本地编的」。
# 注意数字型的 FILEVERSION 塞不进字符串 —— Windows 的版本资源是 4 个 16 位整数，
# 所以那里只用基础版本（1,0,0,0），完整串放在 ProductVersion。

find_package(Git QUIET)

set(YUMEIREN_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    CACHE FILEPATH "版本号来源文件（内容为 X.Y.Z 一行）")

# 跑一条 git 命令并取回输出；git 不可用 / 不是仓库 / 命令失败都返回空串。
function(_yumeiren_git out_var)
    if(NOT GIT_EXECUTABLE)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
        set(${out_var} "${_out}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(yumeiren_resolve_version)
    set(_base "")
    set(_source "")

    if(YUMEIREN_VERSION)
        # 显式指定（-D 或缓存）。空串视为「没指定」，这样 CI 传空变量也不会误伤。
        set(_base "${YUMEIREN_VERSION}")
        set(_source "explicit")
    elseif(DEFINED ENV{YUMEIREN_VERSION} AND NOT "$ENV{YUMEIREN_VERSION}" STREQUAL "")
        set(_base "$ENV{YUMEIREN_VERSION}")
        set(_source "env")
    else()
        _yumeiren_git(_tag describe --tags --exact-match)
        if(_tag MATCHES "^v?[0-9]+\\.[0-9]+\\.[0-9]+")
            set(_base "${_tag}")
            set(_source "tag")
        endif()
    endif()

    if(NOT _base)
        if(EXISTS "${YUMEIREN_VERSION_FILE}")
            file(STRINGS "${YUMEIREN_VERSION_FILE}" _base LIMIT_COUNT 1)
            string(STRIP "${_base}" _base)
            # 显式去掉 CR：本仓库 core.autocrlf=true，全新克隆到 Windows 上时
            # VERSION 会是 CRLF。实测 string(STRIP) 目前确实会吃掉 \r，但那属于
            # 未文档化的行为 —— 而这里一旦漏掉，\r 会被下面正则的 (.*) 捕成
            # 「预发布后缀」，版本号就变成 "1.0.0\r"，且从输出上看不出问题。
            string(REPLACE "\r" "" _base "${_base}")
            string(REPLACE "\n" "" _base "${_base}")
            set(_source "file")
        else()
            message(FATAL_ERROR
                "找不到版本号：${YUMEIREN_VERSION_FILE} 不存在。"
                "请新建该文件并写入一行 X.Y.Z，或用 -DYUMEIREN_VERSION=X.Y.Z 指定。")
        endif()
    endif()

    # 允许 v 前缀（标签习惯写法），并拆成 MAJOR.MINOR.PATCH + 可选预发布后缀
    # （如 1.1.0-rc1 的 "-rc1"）。预发布后缀只进字符串版本，不进数字版本。
    string(REGEX REPLACE "^v" "" _base "${_base}")
    if(NOT _base MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)$")
        message(FATAL_ERROR
            "版本号 '${_base}' 不是 X.Y.Z 形式（来源：${_source}）。"
            "正确写法示例：1.0.0 / 2.3.1 / 1.1.0-rc1")
    endif()
    set(_major "${CMAKE_MATCH_1}")
    set(_minor "${CMAKE_MATCH_2}")
    set(_patch "${CMAKE_MATCH_3}")
    set(_pre   "${CMAKE_MATCH_4}")
    set(_base  "${_major}.${_minor}.${_patch}")

    # 本地开发版后缀：只有「版本来自 VERSION 文件」时才加。
    # 在标签上构建、或 CI 显式传了版本号，都应该得到干净的发布号。
    set(_suffix "")
    # 提交数默认 0（不是空串）：它要当数字宏写进 YumeirenVersion.h，
    # 留空会生成 `#define YUMEIREN_VERSION_COMMIT` 这种半截语句。
    set(_commit "0")
    set(_sha "")
    set(_dirty 0)
    if(_source STREQUAL "file")
        _yumeiren_git(_commit rev-list --count HEAD)
        _yumeiren_git(_sha    rev-parse --short HEAD)
        _yumeiren_git(_dirt   status --porcelain --untracked-files=no)
        if(_commit AND _sha)
            set(_suffix "+${_commit}.g${_sha}")
            if(_dirt)
                set(_dirty 1)
                set(_suffix "${_suffix}.dirty")
            endif()
        endif()
    endif()

    set(YUMEIREN_VERSION_BASE           "${_base}"                    PARENT_SCOPE)
    set(YUMEIREN_VERSION_PRE            "${_pre}"                     PARENT_SCOPE)
    set(YUMEIREN_VERSION_FULL           "${_base}${_pre}${_suffix}"   PARENT_SCOPE)
    set(YUMEIREN_VERSION_MAJOR          "${_major}"                   PARENT_SCOPE)
    set(YUMEIREN_VERSION_MINOR          "${_minor}"                   PARENT_SCOPE)
    set(YUMEIREN_VERSION_PATCH          "${_patch}"                   PARENT_SCOPE)
    # Windows 版本资源要的 4 段形式：逗号分隔给宏用，点分隔给字符串字段用。
    set(YUMEIREN_VERSION_NUMERIC        "${_major},${_minor},${_patch},0" PARENT_SCOPE)
    set(YUMEIREN_VERSION_NUMERIC_STRING "${_major}.${_minor}.${_patch}.0" PARENT_SCOPE)
    set(YUMEIREN_VERSION_SOURCE         "${_source}"                  PARENT_SCOPE)
    set(YUMEIREN_VERSION_COMMIT         "${_commit}"                  PARENT_SCOPE)
    set(YUMEIREN_VERSION_SHA            "${_sha}"                     PARENT_SCOPE)
    set(YUMEIREN_VERSION_DIRTY          "${_dirty}"                   PARENT_SCOPE)

    message(STATUS "Yumeiren 版本: ${_base}${_pre}${_suffix}  (来源: ${_source})")
endfunction()

# 把版本号灌进三个模板。产出全部落在构建目录，源码树保持干净。
# 调用一次即可，必须在 yumeiren_resolve_version() 之后、任何 apply 之前。
function(yumeiren_generate_version_files)
    if(NOT YUMEIREN_VERSION_FULL)
        message(FATAL_ERROR "先调用 yumeiren_resolve_version()")
    endif()

    set(_dir "${CMAKE_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_dir}")

    # app.rc 里的 `1 24 "..."` 是 RT_MANIFEST 资源，要一个 windres 能打开的真实路径。
    # 生成物在构建目录里，所以这里给绝对路径，不依赖相对路径解析规则。
    set(YUMEIREN_MANIFEST_PATH "${_dir}/app.manifest")

    # 图标同理：windres 编译 app.rc 时自己去读 .ico，路径也必须是绝对的真实文件。
    # 复制而不是直接指向源码树，是为了和 manifest 保持一致；另外 configure_file
    # 会把源文件登记进 CMAKE_CONFIGURE_DEPENDS，改了 .ico 会自动重新 configure。
    # （只有 configure 还不够 —— app.rc 重编与否要看 CMakeLists 里那条 OBJECT_DEPENDS。）
    set(YUMEIREN_ICON_PATH "${_dir}/yumeiren.ico")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/yumeiren.ico"
                   "${YUMEIREN_ICON_PATH}" COPYONLY)

    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in"
                   "${_dir}/app.rc" @ONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest.in"
                   "${_dir}/app.manifest" @ONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/YumeirenVersion.h.in"
                   "${_dir}/YumeirenVersion.h" @ONLY)

    set(YUMEIREN_GENERATED_DIR "${_dir}"         PARENT_SCOPE)
    set(YUMEIREN_GENERATED_RC  "${_dir}/app.rc"  PARENT_SCOPE)
    set(YUMEIREN_ICON_PATH     "${YUMEIREN_ICON_PATH}" PARENT_SCOPE)
endfunction()

# 给目标接上版本信息：让它能 #include <YumeirenVersion.h>。
# 每个编译了 src/app/AppInfo.cpp 的目标都要调一次（Yumeiren / YumeirenTest / KanbanProbe）。
function(yumeiren_apply_version target)
    if(NOT YUMEIREN_GENERATED_DIR)
        message(FATAL_ERROR "先调用 yumeiren_generate_version_files()")
    endif()
    target_include_directories(${target} PRIVATE "${YUMEIREN_GENERATED_DIR}")
endfunction()
