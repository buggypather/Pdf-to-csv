# GitHub Actions APK build

This project includes:

    .github/workflows/android-apk.yml

The workflow builds an installable Android **debug APK** on:

- pushes to `main` or `master`
- pull requests targeting `main` or `master`
- version tags beginning with `v`
- manual runs from the GitHub Actions tab

## First-time setup

1. Create a GitHub repository.
2. Upload/push the contents of this project directory to the repository root.
   `.github/workflows/android-apk.yml` must remain at that exact path.
3. Open the repository's **Actions** tab.
4. Select **Build Android APK**.
5. Choose **Run workflow** if you want to build immediately.

No Android SDK/NDK installation is required on your phone or computer for the
GitHub-hosted build. The workflow installs Android Platform 35, Build Tools
35.0.0, NDK 27.2.12479018, CMake 3.22.1, Java 17, and Gradle 8.9.

## Downloading the APK

When the run finishes:

1. Open the completed workflow run.
2. Scroll to **Artifacts**.
3. Download `pdfcsv-android-debug-<run number>`.
4. Unzip it.
5. The contained `app-debug.apk` is signed with the Android debug key and can
   be installed on an Android device after allowing installation from the
   app/browser you used to download it.

## What CI checks

Before building Android, CI also compiles and runs the portable C++ semantic
extraction smoke test. If that fails, the APK build stops.

## Release APKs

The current workflow intentionally builds a debug APK because it requires no
signing secrets and is immediately installable for testing. A proper release
APK/AAB should use a private keystore stored as encrypted GitHub Actions
secrets. Do not commit a release signing key to the repository.
