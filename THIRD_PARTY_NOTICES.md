# Third-party notices

The root MIT license covers this project's original code. Third-party components retain their own licenses and attribution. Dependencies obtained during a build are not relicensed by this repository.

## Gradle Wrapper and build tool

- Files: `app/gradlew`, `app/gradlew.bat`, `app/gradle/wrapper/gradle-wrapper.jar`.
- Upstream: <https://github.com/gradle/gradle>.
- License: Apache License 2.0; the wrapper script copyright notices are retained.
- Copy of the Gradle 9.4.1 upstream license and dependency notices: [licenses/Gradle-LICENSE.txt](licenses/Gradle-LICENSE.txt).
- Source of that text: <https://github.com/gradle/gradle/blob/v9.4.1/LICENSE>.

## FreeType

- Used for native text rendering and fetched from the `VER-2-14-3` tag at build time.
- Upstream: <https://github.com/freetype/freetype>.
- This project uses the FreeType License (FTL) option. The upstream also offers GPLv2 as an alternative; that alternative is not selected here.
- Portions of this software use the FreeType Project (<https://freetype.org>). All rights reserved.
- License text: [licenses/FreeType-FTL.txt](licenses/FreeType-FTL.txt), copied from <https://github.com/freetype/freetype/blob/VER-2-14-3/docs/FTL.TXT>.
- Preserve the upstream notices for any additional FreeType modules included in redistributed builds; its source contains further compatible licenses.

## Shizuku API

- Gradle dependencies: `dev.rikka.shizuku:api` and `dev.rikka.shizuku:provider`, version 13.1.5, with their transitive components.
- Upstream: <https://github.com/RikkaApps/Shizuku-API>.
- Copyright (c) 2021 RikkaW; MIT License.
- License text: [licenses/Shizuku-API-LICENSE.txt](licenses/Shizuku-API-LICENSE.txt).

## Android and other build dependencies

Android SDK/NDK, Android Gradle Plugin, AndroidX annotations, and build plugins are obtained from their respective upstream distributions. Preserve the licenses and notices supplied with those distributions when distributing derived artifacts. Their presence in the build does not make them subject to the root MIT license.

## Runtime libraries and model weights

Qualcomm QNN libraries, TensorFlow Lite runtime binaries, model weights, and device system/vendor libraries are not distributed in this repository. Obtain compatible copies through sources you are authorized to use, and comply with their individual licenses. The same applies if you package those files in a locally built Installer APK.
