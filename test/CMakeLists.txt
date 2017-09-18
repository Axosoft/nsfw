add_library(Catch INTERFACE)
target_include_directories(Catch INTERFACE ../external/catch)


set(ExcludeDirectories u_ExcludeDirectories.cpp)
add_executable(ExcludeDirectories ${ExcludeDirectories})
target_link_libraries(ExcludeDirectories Catch)


set(ExcludeFiles u_ExcludeFiles.cpp)
add_executable(ExcludeFiles ${ExcludeFiles})
target_link_libraries(ExcludeFiles Catch)

