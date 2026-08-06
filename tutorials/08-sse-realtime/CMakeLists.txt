cmake_minimum_required(VERSION 3.15)
project(cwist_tut_08 C)

set(CMAKE_C_STANDARD 11)

include_directories(../../include ../../lib ../../lib/libttak/include)
link_directories(../../ ../../lib/cjson ../../lib/libttak/lib ../../lib/uriparser/build)

add_executable(tut08 main.c)
target_link_libraries(tut08 PRIVATE cwist cjson ttak pthread dl uriparser stdc++ m z)
