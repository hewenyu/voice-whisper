cmake_minimum_required(VERSION 3.10)

# 设置源文件
set(STREAM_SOURCES
    main.cpp
    common.cpp
    common-sdl.cpp
)

# 创建可执行文件
add_executable(stream ${STREAM_SOURCES})

# 设置包含目录
target_include_directories(stream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples
    ${SDL2_INCLUDE_DIRS}
)

# 链接依赖库
target_link_libraries(stream PRIVATE
    whisper
    SDL2::SDL2
)

# 设置输出目录
set_target_properties(stream PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 复制运行时依赖
if(WIN32)
    add_custom_command(TARGET stream POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:stream>
    )
endif() 