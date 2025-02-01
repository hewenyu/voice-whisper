cmake_minimum_required(VERSION 3.10)

# 设置项目名称
project(stream)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 添加可执行文件
add_executable(stream
    stream.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples/common.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples/common-sdl.cpp
)

# 包含头文件目录
target_include_directories(stream PRIVATE
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp
)

# 链接依赖库
target_link_libraries(stream PRIVATE
    whisper
    SDL2::SDL2main
    SDL2::SDL2
    audio_capture
)

if(WIN32)
    target_link_libraries(stream PRIVATE
        ole32
        oleaut32
    )
endif() 