#include <iostream>
// entry point
int main() {
    const char* msg = "hello, world";
    for (int i = 0; i < 3; ++i) {
        std::cout << msg << std::endl;
    }
    return 0;
}
