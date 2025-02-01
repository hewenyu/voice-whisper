cmake_minimum_required(VERSION 3.10)
project(virtual_audio_cable)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置虚拟音频设备项目
add_executable(virtual_audio_cable
    main.cpp
)

# 设置包含目录
target_include_directories(virtual_audio_cable PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../audio_capture/windows
    ${CMAKE_CURRENT_SOURCE_DIR}/../dep/whisper.cpp
)

# 设置编译选项
if(MSVC)
    target_compile_options(virtual_audio_cable PRIVATE 
        /O2 
        /W4 
        /EHsc
        /D_WIN32_WINNT=0x0601  # Windows 7 or later
    )
else()
    target_compile_options(virtual_audio_cable PRIVATE 
        -O3 
        -Wall 
        -Wextra
        -D_WIN32_WINNT=0x0601  # Windows 7 or later
    )
endif()

# 链接必要的库
target_link_libraries(virtual_audio_cable PRIVATE
    audio_capture
    ole32
    oleaut32
    avrt
    winmm
    uuid
)

# 设置输出目录
set_target_properties(virtual_audio_cable PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 设置Windows特定选项
if(WIN32)
    # 使用Unicode字符集
    add_definitions(-DUNICODE -D_UNICODE)
    
    # 设置为Windows子系统
    set_target_properties(virtual_audio_cable PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif() 