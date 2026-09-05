#include <iostream>
#include <cstdlib>
#include "core/version.hpp"

int main() {
    if (vortyx_version_major() != 0) { std::cerr << "Major version mismatch\n"; return 1; }
    if (vortyx_version_minor() != 15) { std::cerr << "Minor version mismatch\n"; return 1; }
    if (vortyx_version_patch() != 0) { std::cerr << "Patch version mismatch\n"; return 1; }
    std::cout << "Version test passed.\n";
    return 0;
}
