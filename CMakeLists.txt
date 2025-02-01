cmake_minimum_required(VERSION 3.10)
project(voice-whisper)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置模型目录路径
set(MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models")
add_definitions(-DMODELS_DIR="${MODELS_DIR}")

# Find SDL2
find_package(SDL2 REQUIRED)

# 添加whisper.cpp子目录
add_subdirectory(dep/whisper.cpp)

# 设置音频捕获源文件
set(AUDIO_CAPTURE_SOURCES
    audio_capture/windows/wasapi_capture.cpp
)

# 创建音频捕获库
add_library(audio_capture STATIC ${AUDIO_CAPTURE_SOURCES})

# 设置音频捕获库的包含目录
target_include_directories(audio_capture PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_capture/windows
)

# 链接Windows系统库
target_link_libraries(audio_capture PUBLIC
    ole32
    oleaut32
    avrt
)

# 设置主程序源文件
set(STREAM_SOURCES
    stream/main.cpp
)

# 创建可执行文件
add_executable(stream ${STREAM_SOURCES})

# 设置包含目录
target_include_directories(stream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dep/whisper.cpp
    ${SDL2_INCLUDE_DIRS}
)

# 链接依赖库
target_link_libraries(stream PRIVATE
    whisper
    audio_capture
    ${SDL2_LIBRARIES}
)

# 设置输出目录
set_target_properties(stream PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 复制models目录到输出目录
add_custom_command(TARGET stream POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MODELS_DIR}/ggml-small.bin"
        "${CMAKE_BINARY_DIR}/bin/models/"
)

# 启用优化
if(MSVC)
    target_compile_options(stream PRIVATE /O2)
else()
    target_compile_options(stream PRIVATE -O3)
endif()

# 设置Windows特定选项
if(WIN32)
    # 使用Unicode字符集
    add_definitions(-DUNICODE -D_UNICODE)
    
    # 设置为Windows子系统
    set_target_properties(stream PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif() 