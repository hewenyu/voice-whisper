cmake_minimum_required(VERSION 3.12)
project(whisper-stream)

# Find SDL2
find_package(SDL2 REQUIRED)

add_executable(whisper-stream stream.cpp)

target_include_directories(whisper-stream PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/examples
    ${CMAKE_SOURCE_DIR}/dep/whisper.cpp/include
)

target_link_libraries(whisper-stream PRIVATE
    whisper
    ${SDL2_LIBRARIES}
)

if(WIN32)
    target_link_libraries(whisper-stream PRIVATE winmm)
endif() 