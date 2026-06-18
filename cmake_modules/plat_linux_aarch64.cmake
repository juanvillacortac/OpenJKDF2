include(cmake_modules/target_linux_runtime_gl_aarch64.cmake)

macro(plat_initialize)
    message(STATUS "Targeting Linux AArch64 (GLES handheld)")

    set(BIN_NAME "openjkdf2")

    add_definitions(-DARCH_64BIT)
    add_definitions(-D_XOPEN_SOURCE=500)
    add_definitions(-D_DEFAULT_SOURCE)
    add_definitions(-DOPENJKDF2_RUNTIME_GL)
    add_definitions(-DSMK_FAST)

    include(cmake_modules/plat_feat_full_sdl2.cmake)

    set(TARGET_USE_PHYSFS FALSE)
    set(TARGET_USE_CURL FALSE)
    set(TARGET_BUILD_TESTS FALSE)
    set(TARGET_FIND_OPENAL FALSE)
    set(TARGET_USE_GAMENETWORKINGSOCKETS TRUE)

    set(TARGET_LINUX TRUE)
    set(OPENJKDF2_RUNTIME_GL TRUE)

    # Headers GLES/EGL Khronos (agnósticos de arquitectura) + sysroot aarch64
    include_directories(/usr/aarch64-linux-gnu/include /usr/include)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=c11 -fshort-wchar -fno-builtin-wcslen -fno-builtin-wcslen -Werror=implicit-function-declaration -Wno-unused-variable -Wno-parentheses -Wno-incompatible-pointer-types")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fshort-wchar -fno-builtin-wcslen -fno-builtin-wcslen -Werror=implicit-function-declaration -Wno-unused-variable -Wno-parentheses")
    add_compile_options(-fno-pie -fno-pic)
    # Static C++ runtime: avoids GLIBCXX_* mismatch on older CFW (ArkOS, etc.)
    add_link_options(-fshort-wchar -fno-pie -no-pie -static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-all -Wl,-rpath,'$ORIGIN/libs.aarch64')
endmacro()

macro(plat_specific_deps)
    plat_sdl2_deps()
endmacro()
