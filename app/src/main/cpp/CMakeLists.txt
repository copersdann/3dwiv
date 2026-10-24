cmake_minimum_required(VERSION 3.22.1)
project(threewiv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(threewiv SHARED
        native_bridge.cpp
        mesh.cpp
        camera.cpp
        gl_renderer.cpp
        stl_parser.cpp
        threemf_parser.cpp
        zip_reader.cpp
        inflate.cpp
        json_mini.cpp
        obj_parser.cpp
        ply_parser.cpp
        amf_parser.cpp
        gltf_parser.cpp
        step_parser.cpp)

# -O2 обязателен для скорости; -Wall/-Wextra для контроля ошибок.
target_compile_options(threewiv PRIVATE -Wall -Wextra -O2 -fno-rtti)
target_link_libraries(threewiv android log GLESv3)