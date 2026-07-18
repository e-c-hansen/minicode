# A sample Python file
import math

class Circle:
    """Represents a circle."""
    def __init__(self, radius):
        self.radius = radius

    def area(self):
        return math.pi * self.radius ** 2

if __name__ == "__main__":
    c = Circle(3.5)
    print(f"Area is {c.area():.2f}")
