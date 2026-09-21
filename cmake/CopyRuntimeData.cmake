# ---------------------------------------------------------------------------
# @file   CopyRuntimeData.cmake
# @author rain
# @date   2026-09-20
# @brief  把项目 data/ 补齐到编译输出目录的 data/，只补缺失文件、已存在的一律不动。
# @note   以 cmake -P 脚本模式运行，参数：-DSRC_DIR=<项目 data> -DDST_DIR=<输出 data>。
# @copyright 本项目遵循仓库 LICENSE。
# ---------------------------------------------------------------------------

# 只补不盖：build/data 归用户管（手工放的模型/视频都在里面），覆盖同名文件等于毁素材。
# 想拿到项目里的原始版本时，用户自己删掉那一个文件再构建即可。

if(NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
    message(FATAL_ERROR "需要 -DSRC_DIR=<源 data 目录> -DDST_DIR=<目标 data 目录>")
endif()

if(NOT IS_DIRECTORY "${SRC_DIR}")
    message(STATUS "runtime data: 源目录不存在，跳过 (${SRC_DIR})")
    return()
endif()

# 在构建期而非 configure 期求值，项目 data/ 新增的素材下一次构建就能进来。
file(GLOB_RECURSE _rel_files RELATIVE "${SRC_DIR}" "${SRC_DIR}/*")

set(_copied 0)
foreach(_rel IN LISTS _rel_files)
    set(_src "${SRC_DIR}/${_rel}")
    if(IS_DIRECTORY "${_src}")
        continue()
    endif()

    set(_dst "${DST_DIR}/${_rel}")
    if(EXISTS "${_dst}")
        continue()
    endif()

    get_filename_component(_dst_dir "${_dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    configure_file("${_src}" "${_dst}" COPYONLY)
    math(EXPR _copied "${_copied} + 1")
endforeach()

message(STATUS "runtime data: 补齐 ${_copied} 个文件 -> ${DST_DIR}")
