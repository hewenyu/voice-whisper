cmake_minimum_required(VERSION 3.12)

# 设置项目名称
project(stream_app)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 设置 whisper.cpp 选项
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Disable whisper examples" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Disable whisper tests" FORCE)
set(WHISPER_SUPPORT_SDL2 OFF CACHE BOOL "Disable SDL2 support" FORCE)

# 如果 whisper 目标不存在，才添加 whisper.cpp 依赖
if(NOT TARGET whisper)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../dep/whisper.cpp ${CMAKE_BINARY_DIR}/whisper)
endif()

# 查找必要的包
find_package(Threads REQUIRED)

# 添加可执行文件
add_executable(stream_app_exe
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../audio_capture/windows/wasapi_capture.cpp
)

# 包含头文件目录
target_include_directories(stream_app_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../dep/whisper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../audio_capture/windows
)

# 链接依赖库
target_link_libraries(stream_app_exe PRIVATE
    whisper
    Threads::Threads
)

# Windows 特定设置
if(WIN32)
    target_link_libraries(stream_app_exe PRIVATE
        ole32          # COM 支持
        oleaut32       # COM 类型库支持
        winmm          # Windows 多媒体
        ksuser         # Windows 内核流
        avrt          # Windows 音频运行时
        uuid          # COM GUID 支持
    )
endif()

# 设置输出名称
set_target_properties(stream_app_exe PROPERTIES OUTPUT_NAME "stream_app")

# 复制模型文件到输出目录（可选）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../models)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../models DESTINATION ${CMAKE_BINARY_DIR})
endif() 