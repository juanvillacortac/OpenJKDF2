set(OPENJKDF2_GLES_DESKTOP TRUE)
include(cmake_modules/target_linux_gles_link.cmake)

macro(plat_initialize)
    message(STATUS "Targeting Linux x86_64 (GLES desktop experiment)")

    set(BIN_NAME "openjkdf2")

    add_definitions(-DARCH_64BIT)
    add_definitions(-D_XOPEN_SOURCE=500)
    add_definitions(-D_DEFAULT_SOURCE)
    add_definitions(-DTARGET_LINUX_GLES)
    add_definitions(-DSMK_FAST)

    include(cmake_modules/plat_feat_full_sdl2.cmake)

    set(TARGET_USE_PHYSFS FALSE)
    set(TARGET_USE_CURL FALSE)
    set(TARGET_BUILD_TESTS FALSE)
    set(TARGET_FIND_OPENAL TRUE)
    set(TARGET_USE_GAMENETWORKINGSOCKETS FALSE)

    set(TARGET_LINUX TRUE)
    set(TARGET_LINUX_GLES TRUE)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=c11 -fshort-wchar -fno-builtin-wcslen -fno-builtin-wcslen -Werror=implicit-function-declaration -Wno-unused-variable -Wno-parentheses -Wno-incompatible-pointer-types")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fshort-wchar -fno-builtin-wcslen -fno-builtin-wcslen -Werror=implicit-function-declaration -Wno-unused-variable -Wno-parentheses")
    add_link_options(-fshort-wchar -pthread)
endmacro()

macro(plat_specific_deps)
    plat_sdl2_deps()
endmacro()
