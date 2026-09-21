#pragma once

// A point or a direction in the plane.
struct Vec2 {
    double x = 0, y = 0;

    // Squared length: cheaper than the length, and fine for comparisons.
    double lengthSquared() const { return x * x + y * y; }

    // This vector stretched by a factor of k.
    Vec2 scaled(double k) const { return {x * k, y * k}; }
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
