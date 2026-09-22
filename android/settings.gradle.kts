// The Android port is its own Gradle build, beside the macOS app (src/) and
// the GTK port (linux/). It compiles the same portable C++ core from ../src
// through the NDK; nothing is copied.
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositories { google(); mavenCentral() }
}
rootProject.name = "MiniCode"
include(":app")
