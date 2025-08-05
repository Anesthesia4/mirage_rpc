# 文件: cmake/CompilerSetup.cmake

# ==============================================================================
# 函数：setup_project_options
# 描述：配置项目级的 C++ 标准、编译器警告、定义和依赖。
#      此函数遵循现代 CMake 实践，将所有配置封装到一个 INTERFACE 库中。
#
# 参数:
#   standard         - (必选) C++ 标准版本 (例如 17, 20, 23)。
#   INTERFACE_TARGET - (必选) 用于接收创建的 INTERFACE 库名称的变量名。
#
# 用法:
#   include(cmake/CompilerSetup.cmake)
#   setup_project_options(
#       STANDARD 20
#       INTERFACE_TARGET my_project_options
#   )
#   # ... 定义你的可执行文件或库
#   add_executable(my_app main.cpp)
#   # ... 将配置应用到目标上
#   target_link_libraries(my_app PRIVATE ${my_project_options})
# ==============================================================================
function(setup_project_options)
    # --- 参数解析 ---
    set(options "") # 无单值选项
    set(oneValueArgs STANDARD INTERFACE_TARGET) # 定义接收单个值的参数
    set(multiValueArgs "") # 无多值选项
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # --- 参数验证 ---
    if(NOT ARG_STANDARD OR NOT ARG_INTERFACE_TARGET)
        message(FATAL_ERROR "setup_project_options 必须提供 STANDARD 和 INTERFACE_TARGET 参数。")
    endif()

    set(VALID_STANDARDS 11 14 17 20 23)
    list(FIND VALID_STANDARDS ${ARG_STANDARD} _standard_index)
    if(_standard_index EQUAL -1)
        message(FATAL_ERROR "不支持的 C++ 标准: ${ARG_STANDARD}。有效值: ${VALID_STANDARDS}")
    endif()

    # --- 创建 INTERFACE 库 ---
    # 这是现代 CMake 的核心：创建一个虚拟目标来承载所有配置属性。
    add_library(${ARG_INTERFACE_TARGET} INTERFACE)
    message(STATUS "创建配置接口库: ${ARG_INTERFACE_TARGET}")

    # --- 设置 C++ 标准 (应用到接口库) ---
    target_compile_features(${ARG_INTERFACE_TARGET} INTERFACE cxx_std_${ARG_STANDARD})

    # --- 设置通用编译定义和选项 ---
    # 使用 target_compile_definitions 和 target_compile_options，并指定 INTERFACE
    # 这样任何链接到此库的目标都会继承这些属性。

    # --- 平台特定设置 ---
    if(WIN32)
        target_compile_definitions(${ARG_INTERFACE_TARGET} INTERFACE UNICODE _UNICODE)
        message(STATUS "为 Windows 添加 UNICODE 定义")
    endif()

    # --- 编译器特定设置 ---
    if(MSVC)
        # MSVC 特定选项
        target_compile_options(${ARG_INTERFACE_TARGET} INTERFACE
                /W4                # 更高警告等级
                # /WX                # 将警告视为错误 (可选，但推荐)
                /EHsc
                /utf-8             # 源码和执行字符集设为 UTF-8
                /Zc:__cplusplus    # 修正 __cplusplus 宏
                /wd4100            # 禁用警告: 未使用的形参
                /wd4996            # 禁用警告: 使用了被标记为否决的函数
        )
        message(STATUS "为 MSVC 添加特定编译选项")
    else() # GCC / Clang / AppleClang
        # 通用于 GCC 和 Clang 的选项
        target_compile_options(${ARG_INTERFACE_TARGET} INTERFACE
                -Wall
                -Wextra
                -Wpedantic         # 更加严格的警告
                -Werror            # 将所有警告视为错误 (可选，但推荐)
                -Wno-unused-parameter
        )

        # C++17 及以上标准的额外警告
        if(${ARG_STANDARD} GREATER_EQUAL 17)
            target_compile_options(${ARG_INTERFACE_TARGET} INTERFACE
                    -Wshadow
                    -Wnon-virtual-dtor
            )
        endif()

        # 【核心修复】区分处理 AppleClang 和标准 Clang/GCC
        # AppleClang 不支持 -finput-charset/-fexec-charset，并默认源码为 UTF-8
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
            target_compile_options(${ARG_INTERFACE_TARGET} INTERFACE
                    -finput-charset=UTF-8
                    -fexec-charset=UTF-8
            )
            message(STATUS "为 GCC/Clang 添加 UTF-8 字符集选项")
        else()
            message(STATUS "检测到 AppleClang，源码假定为 UTF-8，跳过字符集选项")
        endif()
        message(STATUS "为 GCC/Clang 添加特定编译选项")
    endif()

    # --- MinGW 特定设置 ---
    if(MINGW)
        # 为 C++17 及以上的 <filesystem> 支持添加链接库
        if(${ARG_STANDARD} GREATER_EQUAL 17)
            # 使用 target_link_libraries，这才是正确的方式
            target_link_libraries(${ARG_INTERFACE_TARGET} INTERFACE -lstdc++fs)
            message(STATUS "为 MinGW C++${ARG_STANDARD} 添加 libstdc++fs 依赖 (用于 <filesystem>)")
        endif()
    endif()

    # --- 将 INTERFACE 库的名称返回给调用者 ---
    set(${ARG_INTERFACE_TARGET} ${ARG_INTERFACE_TARGET} PARENT_SCOPE)
    message(STATUS "C++${ARG_STANDARD} 项目配置完成，请链接到 ${ARG_INTERFACE_TARGET} 目标。")

endfunction()