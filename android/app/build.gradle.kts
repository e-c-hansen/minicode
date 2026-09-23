plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.minicode.editor"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.minicode.editor"
        // 29 is where scoped storage lands; the tree uses the document picker.
        minSdk = 29
        targetSdk = 36
        // scripts/release.sh passes the release version (-PminicodeVersion=1.4.0);
        // the code is derived from it, so updates always count upward.
        val release = (project.findProperty("minicodeVersion") as String?) ?: "0.1.0"
        val (major, minor, patch) = release.split(".").map { it.toInt() }
        versionName = release
        versionCode = major * 10000 + minor * 100 + patch
        ndk { abiFilters += listOf("arm64-v8a") }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=c++_shared") }
        }
    }
    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt") }
    }
    buildFeatures { viewBinding = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    // The release key lives outside the repo (~/.config/minicode/release.keystore,
    // password in the macOS Keychain); scripts/release.sh hands both over in
    // the environment. Every update must be signed with this same key, so a
    // lost key means users can no longer update. Without it, the release build
    // is left unsigned rather than signed with something else.
    val keystore = System.getenv("MINICODE_KEYSTORE")
    signingConfigs {
        if (keystore != null) create("release") {
            storeFile = file(keystore)
            storePassword = System.getenv("MINICODE_KEYSTORE_PASSWORD")
            keyAlias = "minicode"
            keyPassword = System.getenv("MINICODE_KEYSTORE_PASSWORD")
        }
    }
    buildTypes {
        release {
            // R8 shrinks the libraries (AppCompat, Material) that make up most
            // of the APK. Everything of MiniCode's own is kept whole, because
            // JNI finds its classes and methods by name (proguard-rules.pro).
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"),
                          "proguard-rules.pro")
            if (keystore != null) signingConfig = signingConfigs.getByName("release")
        }
    }
    packaging {
        // Android 15 and up want native code aligned to 16 KB pages.
        jniLibs { useLegacyPackaging = false }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("com.google.android.material:material:1.12.0")
}
