cmake_minimum_required(VERSION 3.20)

project(check_git_status_cpp VERSION 1.0.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

function(enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(check_git_status_cpp_lib
    src/app.cpp
    src/process.cpp
)
target_include_directories(check_git_status_cpp_lib PUBLIC include)
target_compile_features(check_git_status_cpp_lib PUBLIC cxx_std_20)
target_link_libraries(check_git_status_cpp_lib PUBLIC Threads::Threads)
enable_project_warnings(check_git_status_cpp_lib)

add_executable(check_git_status_cpp src/main.cpp)
target_link_libraries(check_git_status_cpp PRIVATE check_git_status_cpp_lib)
set_target_properties(check_git_status_cpp PROPERTIES OUTPUT_NAME check-git-status-cpp)
enable_project_warnings(check_git_status_cpp)
if(MINGW)
    target_link_options(check_git_status_cpp PRIVATE -municode)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(check_git_status_cpp_tests tests/tests.cpp)
    target_link_libraries(check_git_status_cpp_tests PRIVATE check_git_status_cpp_lib)
    enable_project_warnings(check_git_status_cpp_tests)
    add_test(NAME check_git_status_cpp_tests COMMAND check_git_status_cpp_tests)
endif()
