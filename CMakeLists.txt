cmake_minimum_required(VERSION 3.5)
project(Player CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)


pkg_check_modules(AV REQUIRED
    libavformat
    libavcodec
    libswscale
    libswresample
    libavutil)


pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(${AV_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
link_directories(${AV_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS})
add_definitions(${AV_CFLAGS_OTHER} ${SDL2_CFLAGS_OTHER})

add_executable(player
    Player.cpp
    VideoState.cpp
    PacketQueue.cpp
    FrameQueue.cpp
    PCMQueue.cpp
    Demux.cpp
    Decoder_video.cpp
    Decoder_audio.cpp
)

target_link_libraries(player ${AV_LIBRARIES} ${SDL2_LIBRARIES} pthread)

set_target_properties(player PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

install(TARGETS player RUNTIME DESTINATION bin)
