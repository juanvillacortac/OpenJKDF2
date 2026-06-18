macro(plat_link_and_package)
    target_link_libraries(sith_engine PRIVATE PNG::PNG ZLIB::ZLIB)
    target_link_libraries(sith_engine PRIVATE ${SDL2_COMMON_LIBS}
        ${CMAKE_SOURCE_DIR}/stubs_aarch64/libGLESv2.so
        ${CMAKE_SOURCE_DIR}/stubs_aarch64/libEGL.so
        ${OPENAL_LIBRARY})

    if(TARGET_USE_PHYSFS)
        target_link_libraries(sith_engine PRIVATE PhysFS::PhysFS_s)
        target_link_libraries(${BIN_NAME} PRIVATE PhysFS::PhysFS_s)
    endif()

    target_link_libraries(sith_engine PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(sith_engine PRIVATE dl pthread m)
endmacro()

macro(plat_extra_deps)
endmacro()
