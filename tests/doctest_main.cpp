// The one translation unit that compiles the doctest framework and provides
// main(). Every other test file includes <doctest.h> without this macro.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
