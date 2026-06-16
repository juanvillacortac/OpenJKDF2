include(cmake_modules/target_linux_all.cmake)

macro(plat_initialize)
    message( STATUS "Targeting Linux 64-bit" )
    set(BIN_NAME "openjkdf2")

    add_definitions(-DARCH_64BIT)
    add_definitions(-D_XOPEN_SOURCE=500)
    add_definitions(-D_DEFAULT_SOURCE)

    include(cmake_modules/plat_feat_full_sdl2.cmake)

    if(DEFINED ENV{OPENJKDF2_PORTMASTER_BUILD})
        message(STATUS "PortMaster x86_64: no GNS/curl/physfs (system OpenAL/SDL)")
        set(TARGET_USE_PHYSFS FALSE)
        set(TARGET_USE_CURL FALSE)
        set(TARGET_USE_GAMENETWORKINGSOCKETS FALSE)
        add_link_options(-static-libstdc++ -static-libgcc -Wl,-rpath,'$ORIGIN/libs.x86_64' -pthread)
    endif()

    set(TARGET_LINUX TRUE)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=c11 -fshort-wchar -fno-builtin-wcslen -fno-builtin-wcslen -Werror=implicit-function-declaration -Wno-unused-variable -Wno-parentheses -Wno-incompatible-pointer-types")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fshort-wchar -fno-builtin-wcslen -fno-builtin-wcslen -Werror=implicit-function-declaration -Wno-unused-variable -Wno-parentheses ")
    add_link_options(-fshort-wchar)
endmacro()

macro(plat_specific_deps)
    plat_sdl2_deps()
endmacro()