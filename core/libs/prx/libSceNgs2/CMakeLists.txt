add_library(libSceNgs2 SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Ngs2.cpp
)
target_include_directories(libSceNgs2 PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(libSceNgs2 PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceNgs2)
target_link_libraries(libSceNgs2 PRIVATE libc libkernel)
