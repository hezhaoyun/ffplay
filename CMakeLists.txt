cmake_minimum_required(VERSION 3.15)

project(ffplayer VERSION 1.0)

file(GLOB SRCs ${CMAKE_SOURCE_DIR}/*.c)
set(FFMPEG_LIBs avutil avformat avcodec avutil swscale swresample)

include_directories(/usr/local/Cellar/ffmpeg/5.1.2_1/include)
include_directories(/usr/local/Cellar/sdl2/2.26.1/include)

link_directories(/usr/local/Cellar/ffmpeg/5.1.2_1/lib)
link_directories(/usr/local/Cellar/sdl2/2.26.1/lib)

add_executable(${PROJECT_NAME} ${SRCs})

target_link_libraries(${PROJECT_NAME} ${FFMPEG_LIBs} sdl2)