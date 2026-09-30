add_library(libSceIme SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Ime.cpp
)
target_include_directories(libSceIme PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(libSceIme PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceIme)
target_link_libraries(libSceIme PRIVATE libc libkernel)
