plugins {
    id("com.android.application")
    id("kotlin-android")
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.yuvalino.tvm"
    compileSdk = flutter.compileSdkVersion
    // Reuse the same NDK the TVM native libs are cross-compiled with (r27c),
    // rather than flutter's default, so gradle's externalNativeBuild matches.
    ndkVersion = "27.2.12479018"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        applicationId = "com.yuvalino.tvm"
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        // Only arm64: TVM's userland libs are cross-compiled for aarch64 only.
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    // The sidecar libs (libdropbear.so etc.) are dlopen'd by full path and
    // exec'd by name via PATH from within TVM, so they must be real files in
    // the app's nativeLibraryDir — force extraction rather than mmap-from-APK.
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

flutter {
    source = "../.."
}
