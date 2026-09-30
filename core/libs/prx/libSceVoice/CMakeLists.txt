add_library(libSceVoice SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Voice.cpp
)
target_include_directories(libSceVoice PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(libSceVoice PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceVoice)
target_link_libraries(libSceVoice PRIVATE libc libkernel)
