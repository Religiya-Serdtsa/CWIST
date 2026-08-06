cmake_minimum_required(VERSION 3.15)
project(cwist_tut_07 C)

set(CMAKE_C_STANDARD 11)

include_directories(../../include ../../lib ../../lib/libttak/include ../../lib/cjson)
link_directories(../../ ../../lib/cjson ../../lib/libttak/lib ../../lib/uriparser/build)

add_executable(tut07 main.c)
target_link_libraries(tut07 PRIVATE cwist cjson ttak pthread dl uriparser stdc++ m z)
