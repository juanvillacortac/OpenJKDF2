macro(plat_link_and_package)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLESv2 REQUIRED glesv2)
    pkg_check_modules(EGL REQUIRED egl)

    target_link_libraries(sith_engine PRIVATE PNG::PNG ZLIB::ZLIB)
    target_link_libraries(sith_engine PRIVATE ${SDL2_COMMON_LIBS}
        ${OPENAL_LIBRARY} GLEW::GLEW GL
        ${GLESv2_LIBRARIES} ${EGL_LIBRARIES})

    if(TARGET_USE_PHYSFS)
        target_link_libraries(sith_engine PRIVATE PhysFS::PhysFS_s)
        target_link_libraries(${BIN_NAME} PRIVATE PhysFS::PhysFS_s)
    endif()

    target_link_libraries(sith_engine PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(sith_engine PRIVATE dl pthread m)

    if(OPENJKDF2_RUNTIME_GL_DESKTOP)
        target_link_libraries(sith_engine PRIVATE ${GTK3_LIBRARIES})
    endif()
endmacro()

macro(plat_extra_deps)
    if(OPENJKDF2_RUNTIME_GL_DESKTOP)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        include_directories(${GTK3_INCLUDE_DIRS})
        link_directories(${GTK3_LIBRARY_DIRS})
        add_definitions(${GTK3_CFLAGS_OTHER})
    endif()
endmacro()
