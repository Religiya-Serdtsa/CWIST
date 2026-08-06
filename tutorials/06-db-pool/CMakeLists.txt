cmake_minimum_required(VERSION 3.15)
project(cwist_tut_06 C)

set(CMAKE_C_STANDARD 11)

include_directories(../../include ../../lib ../../lib/libttak/include ../../lib/sqlite3 ../../lib/cjson)
link_directories(../../ ../../lib/cjson ../../lib/libttak/lib ../../lib/uriparser/build)

add_executable(tut06 main.c)
target_link_libraries(tut06 PRIVATE cwist cjson ttak pthread dl uriparser stdc++ m z)
