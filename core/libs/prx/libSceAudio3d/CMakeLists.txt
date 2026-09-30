add_library(libSceAudio3d SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Audio3d.cpp
)
target_include_directories(libSceAudio3d PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(libSceAudio3d PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceAudio3d)
target_link_libraries(libSceAudio3d PRIVATE libc libkernel)
