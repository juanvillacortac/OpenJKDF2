#if defined(TARGET_LINUX_GLES) || defined(OPENJKDF2_RUNTIME_GL)

#include <locale>

// Ejecutar antes que los constructores de iostream en otros .cpp
__attribute__((constructor(101)))
static void openjkdf2_gles_cpp_runtime_init(void)
{
    std::locale::global(std::locale::classic());
}

#endif
