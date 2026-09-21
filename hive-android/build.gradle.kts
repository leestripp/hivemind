plugins {
    id("com.android.application") version "8.7.3" apply false
    // 2.1.21: 2.0.21 is flagged for kotlin-stdlib Incorrect Default
    // Permissions (AIKIDO-2026-550292 / SNYK-JAVA-ORGJETBRAINSKOTLIN-2393744,
    // createTempDir/createTempFile info exposure) — fixed in 2.1.0+.
    id("org.jetbrains.kotlin.android") version "2.1.21" apply false
}
