cmake_minimum_required(VERSION 3.14)
project(mirage_rpc)

include(cmake/retrieve_files.cmake)
include(cmake/project_cpp_standard.cmake)

setup_project_options(
    STANDARD 23
    INTERFACE_TARGET mirage_rpc_options
)

find_package(grpc REQUIRED)
find_package(cppzmq REQUIRED)
find_package(spdlog REQUIRED)

set(FILES "")
retrieve_files(${CMAKE_CURRENT_SOURCE_DIR}/src FILES)
add_library(${PROJECT_NAME} STATIC ${FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} PUBLIC grpc cppzmq spdlog)
target_link_libraries(${PROJECT_NAME} PRIVATE mirage_rpc_options)
