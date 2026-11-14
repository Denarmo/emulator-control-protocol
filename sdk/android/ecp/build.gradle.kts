// The ECP Android library. One build file for both kinds of consumer:
// - AGP 9 or later: Kotlin is built in, so nothing else is needed (Java-only apps included).
// - AGP 8.x: the build also needs the Kotlin Android plugin on its classpath (declare
//   id("org.jetbrains.kotlin.android") apply false in the root build); it is applied here.
plugins {
    id("com.android.library")
}

val agpMajor = com.android.Version.ANDROID_GRADLE_PLUGIN_VERSION.substringBefore('.').toInt()
if (agpMajor < 9) apply(plugin = "org.jetbrains.kotlin.android")

android {
    namespace = "io.github.ecp"
    compileSdk = 36

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinJvmCompile>().configureEach {
    compilerOptions.jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
}

dependencies {
    // org.json is part of Android; the JVM unit tests need the real one.
    testImplementation("org.json:json:20240303")
    testImplementation("junit:junit:4.13.2")
}
