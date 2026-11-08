import org.jetbrains.kotlin.gradle.dsl.JvmTarget

// The Android port's Kotlin checks, run on the JVM with `./gradlew
// kotlinChecks` (CI's Android job). No test library: the checks in
// app/src/test/kotlin count their own failures and exit non-zero. They are
// compiled here with the app's Kotlin files that have no Android in them
// (listed below; a file added to the list must stay free of android.* and
// of JNI calls the checks would reach), and read the C++ those files mirror
// from the repo.
plugins {
    id("org.jetbrains.kotlin.jvm")
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
    sourceSets["main"].kotlin.apply {
        setSrcDirs(listOf("../app/src/test/kotlin", "../app/src/main/java"))
        include(
            "org/minicode/editor/checks/**",
            // Pty.kt loads the native library only when a Pty is opened;
            // the checks read its constants (inlined) and Pty.Screen.
            "org/minicode/editor/Pty.kt",
            "org/minicode/editor/LeaderKey.kt",
        )
    }
}

tasks.register<JavaExec>("kotlinChecks") {
    group = "verification"
    description = "Runs the Android port's Kotlin checks on the JVM."
    classpath = sourceSets["main"].runtimeClasspath
    mainClass.set("org.minicode.editor.checks.KotlinChecksKt")
    // The repo's root, for the C++ headers the checks compare against.
    args(rootDir.resolve("..").canonicalPath)
}
