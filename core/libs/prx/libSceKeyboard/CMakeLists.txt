add_library(libSceKeyboard SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Keyboard.cpp
)
target_include_directories(libSceKeyboard PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(libSceKeyboard PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceKeyboard)
target_link_libraries(libSceKeyboard PRIVATE libc libkernel)
