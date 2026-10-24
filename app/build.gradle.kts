plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.threewiv.viewer"
    compileSdk = 35

    // NDK из установленного SDK (в этой системе — 28.2.13676358).
    // Если используется Android Studio с другим NDK — поменяйте на свой.
    ndkVersion = "28.2.13676358"
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "com.threewiv.viewer"
        minSdk = 24          // OpenGL ES 3.0 доступен с API 18 — запас с запасом
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 64-битные сборки: максимум производительности, минимум мусора
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-O2", "-fno-rtti")
                arguments += listOf("-DANDROID_STL=c++_static", "-DANDROID_ARM_NEON=ON")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false   // пока выключено: JNI-имена не трогаем (R8 позже с keep-правилами)
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // Намеренно НОЛЬ внешних зависимостей:
    // только фреймворк Android + Kotlin stdlib (добавляется плагином).
    // Это даёт минимальный размер APK и полный контроль над производительностью.
}