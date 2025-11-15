cmake_minimum_required(VERSION 3.10)
project(terminal_and_child_process CXX)

if(MSVC)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/execution-charset:.932)
else()
    add_compile_options(-fexec-charset=CP932)
    add_link_options(-static)
endif()

add_executable(call_createprocess       call_createprocess.cpp)
add_executable(call_createpseudoconsole call_createpseudoconsole.cpp)
add_executable(call_system              call_system.cpp)
