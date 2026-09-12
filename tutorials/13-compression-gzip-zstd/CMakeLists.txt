cmake_minimum_required(VERSION 3.15)
project(cwist_tut_13 C)

set(CMAKE_C_STANDARD 11)

include_directories(../../include ../../lib ../../lib/libttak/include)
link_directories(../../ ../../lib/cjson ../../lib/libttak/lib ../../lib/uriparser/build ../../lib/boringssl/build ../../lib/lsquic/build/src/liblsquic ../../lib/cnats/build/lib)

add_executable(tut13 main.c)
target_link_libraries(tut13 PRIVATE cwist lsquic ssl crypto nats_static cjson ttak pthread dl uriparser stdc++ m z zstd brotlienc brotlicommon brotlidec curl)
