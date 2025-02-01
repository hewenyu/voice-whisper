# 设置stream项目
add_executable(stream
    main.cpp
)

# 查找SDL2
find_package(SDL2 REQUIRED)

# 设置包含目录
target_include_directories(stream PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../dep/whisper.cpp
)

# 链接必要的库
target_link_libraries(stream PRIVATE
    ${SDL2_LIBRARIES}
    whisper
)

# 设置输出目录
set_target_properties(stream PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 启用优化
if(MSVC)
    target_compile_options(stream PRIVATE /O2 /W4 /EHsc)
else()
    target_compile_options(stream PRIVATE -O3 -Wall -Wextra)
endif()

# 设置Windows特定选项
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
    set_target_properties(stream PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif() 