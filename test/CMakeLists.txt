add_library(Catch INTERFACE)
target_include_directories(Catch INTERFACE ../external/catch)

find_package (Threads)

function(nsfw_add_test test_name cpp_file)
  add_executable(${test_name} ${cpp_file})
  target_link_libraries(${test_name} Catch ${CMAKE_THREAD_LIBS_INIT} nsfw)
  add_test(${test_name} ${test_name})
endfunction()

nsfw_add_test(ExcludeDirectories u_ExcludeDirectories.cpp)
nsfw_add_test(FileWatcher u_FileWatcher.cpp)
nsfw_add_test(ExcludeFiles u_ExcludeFiles.cpp)
