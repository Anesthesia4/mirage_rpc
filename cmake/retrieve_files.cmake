#[=======================================================================[
  平台匹配检查函数
  参数:
    platform: 平台标识符 (windows|linux|mac|mobile|desktop)
    is_match: 输出变量，表示当前平台是否匹配
#]=======================================================================]
function(is_current_platform platform is_match)
    # 设置默认值为TRUE（用于未知平台）
    set(matches FALSE)

    if(platform STREQUAL "windows")
        if(WIN32 OR CYGWIN)
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "linux")
        if(UNIX AND NOT APPLE)
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "mac")
        if(APPLE AND NOT IOS)
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "ios")
        if(IOS)
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "android")
        if(ANDROID)
            set(matches TRUE)
        endif()
        # 添加对unix平台的支持
    elseif(platform STREQUAL "unix")
        if(UNIX)
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "mobile")
        if(ANDROID OR IOS)
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "desktop")
        if(WIN32 OR (UNIX AND NOT APPLE) OR (APPLE AND NOT IOS))
            set(matches TRUE)
        endif()
    elseif(platform STREQUAL "web")
        if(EMSCRIPTEN)
            set(matches TRUE)
        endif()
    else()
        # 未知平台标识，默认匹配
        set(matches TRUE)
    endif()

    set(${is_match} ${matches} PARENT_SCOPE)
endfunction()

#[=======================================================================[
  主文件检索函数
  参数:
    path: 要检索的根目录路径
    extension: 文件扩展名列表
    out_files: 输出变量名，将包含筛选后的文件列表
#]=======================================================================]
function(retrieve_files_custom path extension out_files)
    # 1. 参数验证
    if(NOT IS_DIRECTORY "${path}")
        message(WARNING "错误：目录 '${path}' 不存在")
        return()
    endif()

    message(STATUS "正在检索目录: ${path}")

    # 2. 构建文件匹配模式
    set(file_patterns "")
    foreach(ext IN LISTS extension)
        list(APPEND file_patterns "${path}/*.${ext}")
    endforeach()

    # 3. 递归查找所有匹配的文件
    file(GLOB_RECURSE found_files
            RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
            CONFIGURE_DEPENDS ${file_patterns}
    )

    # 4. 处理找到的文件
    set(filtered_files "")
    foreach(current_file IN LISTS found_files)
        # 4.1 获取文件所在目录
        get_filename_component(file_dir "${current_file}" DIRECTORY)
        string(REPLACE "/" ";" dir_components "${file_dir}")

        # 4.2 检查平台兼容性
        set(should_skip_file FALSE)
        set(found_platform_dir FALSE)

        foreach(dir_name IN LISTS dir_components)
            # 检查是否是平台相关目录
            if(dir_name MATCHES "^(windows|linux|mac|ios|android|unix|mobile|desktop|web)$")
                set(found_platform_dir TRUE)
                is_current_platform(${dir_name} platform_matches)
                if(NOT platform_matches)
                    set(should_skip_file TRUE)
                    break()
                endif()
            endif()
        endforeach()

        # 如果文件需要跳过，继续处理下一个文件
        if(should_skip_file)
            continue()
        endif()

        # 4.3 添加符合条件的文件
        list(APPEND filtered_files "${current_file}")

        # 4.4 设置IDE文件分组
        # 计算相对路径作为分组名称
        get_filename_component(root_abs_path "${path}" ABSOLUTE)
        get_filename_component(file_dir_abs_path "${file_dir}" ABSOLUTE)
        file(RELATIVE_PATH group_path "${root_abs_path}" "${file_dir_abs_path}")

        # 处理根目录的特殊情况
        if(group_path STREQUAL ".")
            set(group_name "")
        else()
            string(REPLACE "/" "\\" group_name "${group_path}")
        endif()

        # 创建IDE分组
        source_group("${group_name}" FILES "${current_file}")
    endforeach()

    # 5. 设置输出变量
    set(${out_files} ${filtered_files} PARENT_SCOPE)
endfunction()

#[=======================================================================[
  便捷封装函数
  自动处理常见文件扩展名，针对不同平台添加特定文件类型
#]=======================================================================]
function(retrieve_files path out_files)
    # 设置基础文件类型
    set(file_extensions
            "h"     # 头文件
            "hpp"   # C++头文件
            "ini"   # 配置文件
            "cpp"   # C++源文件
            "c"     # C源文件
            "ixx"   # C++20模块文件
    )

    # 针对Mac平台添加额外文件类型
    if(APPLE)
        list(APPEND file_extensions "mm") # Objective-C++源文件
    endif()

    # 执行文件检索
    set(temp_files "")
    retrieve_files_custom(${path} "${file_extensions}" temp_files)

    # 合并结果到输出变量
    set(${out_files} ${${out_files}} ${temp_files} PARENT_SCOPE)
endfunction()

#[=======================================================================[
#  用于添加资源文件并在编译后复制到最终可执行文件所在目录
#  注意：此函数依赖于 CMAKE_RUNTIME_OUTPUT_DIRECTORY 或 EXECUTABLE_OUTPUT_PATH
#        变量的设置，以确定可执行文件的输出目录。请确保在项目中设置了其中之一。
#
#  参数:
#    TARGET_NAME:    (必需) - 关联的目标 (库或可执行文件) 的名称。
#                       资源复制命令将在 TARGET_NAME 构建后执行。
#    RESOURCE_FILES: (必需) - 一个或多个要复制的资源文件的路径列表 (相对或绝对)
#    OUTPUT_SUBDIR:  (可选) - 相对于可执行文件输出目录的子目录路径 (例如 "assets")
#
#  例子:
#    # 确保设置了可执行文件输出目录 (通常在顶层 CMakeLists.txt)
#    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
#
#    # 添加库
#    add_library(my_lib STATIC src/my_lib.cpp)
#
#    # 添加资源到 my_lib，但复制到最终可执行文件的输出目录下的 'config' 子目录
#    add_resource_file(
#        TARGET_NAME   my_lib
#        RESOURCE_FILES config/settings.json config/defaults.ini
#        OUTPUT_SUBDIR config
#    )
#
#    # 添加可执行文件
#    add_executable(my_app main.cpp)
#    target_link_libraries(my_app PRIVATE my_lib)
#
#    # 添加 my_app 的资源，复制到可执行文件输出目录的根目录
#    add_resource_file(
#        TARGET_NAME my_app
#        RESOURCE_FILES assets/icon.png
#    )
#]=======================================================================]
function(add_resource_file)
    # 定义预期的参数
    set(options "") # 无布尔选项
    set(oneValueArgs TARGET_NAME OUTPUT_SUBDIR)
    set(multiValueArgs RESOURCE_FILES)

    # 解析传递给函数的参数
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # --- 参数验证 ---
    if(NOT ARG_TARGET_NAME)
        message(FATAL_ERROR "**add_resource_file**: **缺少必需参数** **TARGET_NAME**.")
    endif()
    if(NOT ARG_RESOURCE_FILES)
        message(FATAL_ERROR "**add_resource_file**: **缺少必需参数** **RESOURCE_FILES**.")
    endif()
    if(NOT TARGET ${ARG_TARGET_NAME})
        message(WARNING "**add_resource_file**: 目标 '${ARG_TARGET_NAME}' (尚)不存在。请确保在调用 add_executable/add_library('${ARG_TARGET_NAME}') 之后调用此函数。")
        # 即使目标尚不存在，仍然尝试配置命令。CMake通常能处理好依赖关系。
    endif()

    # --- 确定最终可执行文件的目标基础目录 ---
    set(DESTINATION_BASE "")
    if(DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY AND CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(DESTINATION_BASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    elseif(DEFINED EXECUTABLE_OUTPUT_PATH AND EXECUTABLE_OUTPUT_PATH)
        # EXECUTABLE_OUTPUT_PATH 是旧变量，但也检查一下
        set(DESTINATION_BASE "${EXECUTABLE_OUTPUT_PATH}")
    else()
        # 如果是多配置生成器（如 Visual Studio, Xcode），需要考虑配置类型
        get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(is_multi_config)
            # 对于多配置，没有单一的顶级运行时目录变量。
            # 可以考虑使用 $<OUTPUT_DIRECTORY> 配合一个已知的可执行文件名，但这会使函数复杂化。
            # 最好的做法是要求用户设置 CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>
            # 或者我们直接报错，强制用户设置 CMAKE_RUNTIME_OUTPUT_DIRECTORY
            message(FATAL_ERROR "**add_resource_file**: **无法确定可执行文件输出目录**。请在您的项目中设置 **CMAKE_RUNTIME_OUTPUT_DIRECTORY** 变量 (例如 set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"\${CMAKE_BINARY_DIR}/bin\"))。对于多配置生成器，可能需要设置 CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG> 变量。")
        else()
            # 对于单配置生成器（如 Makefiles, Ninja），可以默认到 CMAKE_BINARY_DIR
            set(DESTINATION_BASE "${CMAKE_BINARY_DIR}")
            message(WARNING "**add_resource_file**: **未设置 CMAKE_RUNTIME_OUTPUT_DIRECTORY**。默认将资源复制到 CMAKE_BINARY_DIR ('${CMAKE_BINARY_DIR}')。强烈建议设置 CMAKE_RUNTIME_OUTPUT_DIRECTORY 以获得可预测的行为。")
        endif()
        # message(FATAL_ERROR "**add_resource_file**: **无法确定可执行文件输出目录**。请在您的项目中设置 **CMAKE_RUNTIME_OUTPUT_DIRECTORY** 变量 (例如 set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"\${CMAKE_BINARY_DIR}/bin\"))。")
    endif()

    # 处理子目录
    set(DESTINATION_DIR "${DESTINATION_BASE}") # 默认目标目录
    if(ARG_OUTPUT_SUBDIR)
        # 清理子目录路径字符串
        string(STRIP "${ARG_OUTPUT_SUBDIR}" _subdir)
        if(IS_ABSOLUTE "${_subdir}")
            message(FATAL_ERROR "**add_resource_file**: **OUTPUT_SUBDIR** ('${ARG_OUTPUT_SUBDIR}') **必须是相对路径**。")
        else()
            # 移除可能存在的前导/后导斜杠，以便干净地拼接路径
            string(REGEX REPLACE "^[/\\\\]+" "" _subdir "${_subdir}")
            string(REGEX REPLACE "[/\\\\]+$" "" _subdir "${_subdir}")
            if(_subdir) # 仅当子目录清理后非空时才追加
                set(DESTINATION_DIR "${DESTINATION_BASE}/${_subdir}")
            endif()
        endif()
    endif()

    # --- 准备源文件路径 ---
    set(ABS_RESOURCE_FILES "")
    foreach(RESOURCE_FILE ${ARG_RESOURCE_FILES})
        get_filename_component(RESOURCE_FILE_REALPATH "${RESOURCE_FILE}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        # if(IS_ABSOLUTE "${RESOURCE_FILE}")
        # # 如果已经是绝对路径，直接使用
        # list(APPEND ABS_RESOURCE_FILES "${RESOURCE_FILE}")
        # else()
        # # 如果是相对路径，相对于当前源目录进行解析
        # list(APPEND ABS_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_FILE}")
        # endif()
        list(APPEND ABS_RESOURCE_FILES "${RESOURCE_FILE_REALPATH}")

        # 检查文件是否存在 (在配置时发出警告，有助于早期发现错误)
        # list(GET ABS_RESOURCE_FILES -1 _current_abs_file) # 获取刚才添加的绝对路径
        if(NOT EXISTS "${RESOURCE_FILE_REALPATH}")
            message(WARNING "**add_resource_file**: **资源文件** '${RESOURCE_FILE}' (解析为 '${RESOURCE_FILE_REALPATH}') **在配置时不存在**。")
        endif()
    endforeach()

    # --- 添加自定义命令 ---
    # 使用 add_custom_command 在目标构建完成后执行复制操作
    if(ABS_RESOURCE_FILES) # 确保有文件需要复制
        # 注意：DESTINATION_DIR 可能包含特定于配置的路径（例如，如果 CMAKE_RUNTIME_OUTPUT_DIRECTORY
        # 设置为 ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin）。
        # add_custom_command 的 COMMAND 参数在构建时执行，此时这些变量/生成器表达式已解析。
        add_custom_command(
                TARGET ${ARG_TARGET_NAME}
                POST_BUILD                               # 指定在目标构建之后执行
                # 步骤 1: 确保目标目录存在 (copy_if_different 不会创建目录)
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DESTINATION_DIR}"
                # 步骤 2: 复制文件
                COMMAND ${CMAKE_COMMAND} -E copy_if_different # 使用CMake内置命令复制（仅当文件不同时）
                ${ABS_RESOURCE_FILES}            # 要复制的源文件列表（绝对路径）
                "${DESTINATION_DIR}"             # 最终可执行文件所在的目标目录 (带引号以处理空格)
                COMMENT "为 ${ARG_TARGET_NAME} 将资源复制到可执行文件目录: ${DESTINATION_DIR}..." # 构建时显示的注释
                VERBATIM                                 # 确保参数（尤其是路径和生成器表达式）被正确处理
        )
    else()
        message(WARNING "**add_resource_file**: 没有有效的资源文件提供给目标 '${ARG_TARGET_NAME}'。")
    endif()

    # --- 可选: 将资源文件添加到 IDE 项目结构中 ---
    if(ABS_RESOURCE_FILES)
        set(_source_group_name "Resource Files") # 基础组名
        if(ARG_OUTPUT_SUBDIR)
            # 使用与目标目录结构匹配的组名
            string(STRIP "${ARG_OUTPUT_SUBDIR}" _clean_subdir)
            string(REPLACE "\\" "/" _clean_subdir "${_clean_subdir}") # 统一使用正斜杠
            string(REGEX REPLACE "^[/]+" "" _clean_subdir "${_clean_subdir}")
            string(REGEX REPLACE "[/]+$" "" _clean_subdir "${_clean_subdir}")
            if(_clean_subdir)
                set(_source_group_name "Resource Files/${_clean_subdir}")
            endif()
        endif()
        # 使用 source_group 将文件添加到 IDE 的指定组下
        source_group(${_source_group_name} FILES ${ABS_RESOURCE_FILES})
    endif()

endfunction()
