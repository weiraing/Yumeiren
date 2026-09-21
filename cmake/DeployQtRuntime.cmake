# ---------------------------------------------------------------------------
# @file   DeployQtRuntime.cmake
# @author rain
# @date   2026-09-21
# @brief  把可执行文件运行时需要的 Qt 库与插件补齐到输出目录，缺什么补什么。
# @note   以 cmake -P 脚本模式运行，入参见下方说明。
# @copyright 本项目遵循仓库 LICENSE。
# ---------------------------------------------------------------------------
#
# 入参：
#   DST_DIR  可执行文件所在目录(所有目标的落点)
#   FILES    平铺复制到 DST_DIR 的源文件，分号分隔(取各自的文件名作目标名)
#   PAIRS    需要落到子目录的项，每项形如 <源文件绝对路径>|<目标相对路径>
#
# 为什么不用 add_custom_command(TARGET ... POST_BUILD)：
#   POST_BUILD 只在目标**真正重新链接**时才跑。ninja 判定 no work to do 时它根本
#   不执行 —— 用户删掉某个 DLL 后重编译，那个文件永远补不回来。视频壁纸会以
#   「窗口挂上了却永远黑屏、日志里 mediaStatus 一次都不出现」的形式坏掉，而且不报
#   任何错误(ffmpegmediaplugin 是插件，不在 exe 的导入表里，缺了它 exe 照样启动)。
#   data/ 是同一类坑，用同一种解法：挂到 ALL 上的独立 target，每次构建都补。
#
# 逐文件判断，刻意**不用**「目标目录已存在就整体跳过」：别的构建步骤只要先建出
#   目录，整目录判据就会被顶掉 —— 那正是 data/ 那边踩过的坑。
#
# 判据是「缺失 / 大小不同 / 源时间戳更新」，不做内容比对：一次复制之后目标的时间戳
#   就不会早于源，后续构建只做几次 stat 就跳过，不会把几十 MB 的 avcodec 读一遍。
#   时间戳用秒数比较而不是 IS_NEWER_THAN —— 后者在两者相同时返回真，会让每次都退化成
#   内容比对。
#
# 落点相同的多个目标(本仓库三个 exe 都在 build/)会并发跑本脚本、可能同时写同一个文件。
#   这是良性的：源文件是同一份，写入的字节序列也逐字节相同，交错的写不会改变结果 ——
#   所以不必为此加锁。

if(NOT DEFINED DST_DIR)
    message(FATAL_ERROR "需要 -DDST_DIR=<可执行文件目录>")
endif()

if(NOT IS_DIRECTORY "${DST_DIR}")
    file(MAKE_DIRECTORY "${DST_DIR}")
endif()

set(YUMEIREN_COPIED_N 0)
set(YUMEIREN_KEPT_N 0)
set(YUMEIREN_MISSING "")

# 需要时复制一个文件，并保证父目录存在。
function(_yumeiren_sync_one src dst)
    if(NOT EXISTS "${src}")
        set(_missing "${YUMEIREN_MISSING}")
        list(APPEND _missing "${src}")
        set(YUMEIREN_MISSING "${_missing}" PARENT_SCOPE)
        return()
    endif()

    set(_need TRUE)
    if(EXISTS "${dst}")
        file(SIZE "${src}" _src_size)
        file(SIZE "${dst}" _dst_size)
        if(_src_size STREQUAL _dst_size)
            file(TIMESTAMP "${src}" _src_time "%s")
            file(TIMESTAMP "${dst}" _dst_time "%s")
            if(NOT _src_time GREATER _dst_time)
                set(_need FALSE)
            endif()
        endif()
    endif()

    if(_need)
        get_filename_component(_dir "${dst}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(COPY_FILE "${src}" "${dst}" ONLY_IF_DIFFERENT)
        math(EXPR _n "${YUMEIREN_COPIED_N} + 1")
        set(YUMEIREN_COPIED_N ${_n} PARENT_SCOPE)
    else()
        math(EXPR _n "${YUMEIREN_KEPT_N} + 1")
        set(YUMEIREN_KEPT_N ${_n} PARENT_SCOPE)
    endif()
endfunction()

# 平铺的库：目标名就是源文件名。
foreach(_src IN LISTS FILES)
    if(_src STREQUAL "")
        continue()
    endif()
    get_filename_component(_name "${_src}" NAME)
    _yumeiren_sync_one("${_src}" "${DST_DIR}/${_name}")
endforeach()

# 插件：按 PAIRS 里给的相对路径落到子目录(platforms/、multimedia/)。
foreach(_pair IN LISTS PAIRS)
    if(_pair STREQUAL "")
        continue()
    endif()
    string(FIND "${_pair}" "|" _bar)
    if(_bar LESS 0)
        message(WARNING "DeployQtRuntime: 忽略格式不正确的项 '${_pair}'，应为 <源>|<目标相对路径>")
        continue()
    endif()
    string(SUBSTRING "${_pair}" 0 ${_bar} _src)
    math(EXPR _after "${_bar} + 1")
    string(SUBSTRING "${_pair}" ${_after} -1 _rel)
    _yumeiren_sync_one("${_src}" "${DST_DIR}/${_rel}")
endforeach()

message(STATUS "Qt 运行时: 新补 ${YUMEIREN_COPIED_N} 个、已是最新 ${YUMEIREN_KEPT_N} 个 -> ${DST_DIR}")

if(YUMEIREN_MISSING)
    message(STATUS "Qt 运行时: 源文件不存在(跳过) ${YUMEIREN_MISSING}")
endif()
