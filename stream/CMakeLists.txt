cmake_minimum_required(VERSION 3.12)
project(whisper-stream)

# Find SDL2
find_package(SDL2 REQUIRED)

add_executable(whisper-stream 
    stream.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples/common.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples/common-sdl.cpp
)

target_include_directories(whisper-stream PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/include
)

target_compile_definitions(whisper-stream PRIVATE
    HAVE_SDL2
)

if(WIN32)
    target_link_libraries(whisper-stream PRIVATE winmm)
    
    # Windows subsystem settings
    if(MSVC)
        set_target_properties(whisper-stream PROPERTIES
            WIN32_EXECUTABLE FALSE
        )
    else()
        set_target_properties(whisper-stream PROPERTIES
            WIN32_EXECUTABLE FALSE
        )
        target_link_options(whisper-stream PRIVATE -mconsole)
    endif()
endif()

# Link libraries in the correct order
target_link_libraries(whisper-stream PRIVATE
    SDL2::SDL2main
    SDL2::SDL2
    whisper
) 