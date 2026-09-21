#include "vec.h"

int main() {
    Vec2 p{3, 4};
    Vec2 q = p + Vec2{1, 1}.scaled(2);
    return q.lengthSquared() > 50 ? 0 : 1;
}
