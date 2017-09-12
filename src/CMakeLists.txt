
set (NSFW_LIBRARY_SOURCES
    Queue.cpp
    NativeInterface.cpp
    win32/ReadLoop.cpp
    win32/ReadLoopRunner.cpp
)

set (CMAKE_CXX_STANDARD 11)

add_library(nsfw STATIC ${NSFW_LIBRARY_SOURCES})
target_include_directories(nsfw PUBLIC ${NSFW_INCLUDE_DIR})
