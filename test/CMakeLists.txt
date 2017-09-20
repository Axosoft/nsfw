add_library(Catch INTERFACE)
target_include_directories(Catch INTERFACE ../external/catch)

find_package (Threads)


set(ExcludeDirectories u_ExcludeDirectories.cpp)
add_executable(ExcludeDirectories ${ExcludeDirectories})
target_link_libraries(ExcludeDirectories Catch ${CMAKE_THREAD_LIBS_INIT})


set(ExcludeFiles u_ExcludeFiles.cpp)
add_executable(ExcludeFiles ${ExcludeFiles})
target_link_libraries(ExcludeFiles Catch ${CMAKE_THREAD_LIBS_INIT})

set(FileWatcher u_FileWatcher.cpp)
add_executable(FileWatcher ${FileWatcher})
target_link_libraries(FileWatcher Catch ${CMAKE_THREAD_LIBS_INIT} nsfw)
