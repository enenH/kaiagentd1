# kaiagentd1

Android ARM64 原生实时画面采集、TensorFlow Lite / Qualcomm QNN 推理、悬浮层绘制与触控代理项目，包含运行时资源安装器和启动器。

本版本公开项目源码，已移除卡密登录、在线授权和心跳校验。应用仍使用 Android 系统权限与 Shizuku / root 能力完成设备操作。

## 仓库内容

| 路径 | 用途 |
| --- | --- |
| `src/`、`include/` | C++20 原生程序、画面采集、绘制、推理及触控代理 |
| `app/installer/` | 将本地准备的运行时资源部署到设备 |
| `app/runner/` | 打包并启动原生程序，无需卡密 |
| `app/shizuku-common/` | Shizuku 与 root 访问、设备兼容性检查 |
| `lib/arm64-v8a/README.md` | 自行准备运行库和模型的说明 |
| `scripts/check_public_tree.py` | 发布前检查，输出问题位置而不回显敏感值 |

程序的原生输出名为 `libkaiagentd.so`，实际是可执行程序，以该名称打包到 Runner 中。

## 环境与设备

- 构建工具：JDK 21、Android SDK Platform 36、Build Tools 36.0.0、Android NDK r29（29.0.14206865）、CMake 3.22.1 或更新版、Ninja。
- Gradle Wrapper 固定为 9.4.1，Android Gradle Plugin 固定为 9.2.1；无需另行安装 Gradle。
- 原生目标是 `arm64-v8a`，编译的最低 Android API 为 34。
- 应用中的设备检查要求 Android 15 / API 35 及以上和 Qualcomm 处理器；这不代表所有 ROM、驱动或芯片型号均经过实机验证。
- 运行需用户授予 Shizuku 或 root 权限，并准备兼容的 QNN / TensorFlow Lite 运行库和模型。

**本仓库不包含签名私钥、预编译推理库和模型权重。** 干净检出可以构建源码和 APK；要进行设备上的实际推理，先按 [运行时资源说明](lib/arm64-v8a/README.md) 放入自己有权使用的资源，再构建 Installer。未配置签名时，Release APK 为 unsigned，安装前需自行签名；Debug APK 使用本机构建工具生成的调试签名。

## 构建

### Linux / macOS

先安装上述工具，将 CMake、Ninja 加入 `PATH`，并配置本机 SDK 与 NDK 的路径。以下每段独立构建示例均从仓库根目录开始执行：

```sh
export ANDROID_HOME=/path/to/android-sdk
export ANDROID_NDK_HOME=/path/to/android-ndk

cmake -S . -B cmake-build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-34
cmake --build cmake-build-debug --target kaiagentd --parallel

cd app
./gradlew :installer:assembleDebug :runner:assembleDebug
```

CMake 会从公开上游获取 FreeType。首次 Gradle 构建会下载发行包和 Maven 依赖，需要能访问对应的公开服务。

如已经编译了原生程序，可让 Runner 直接打包该文件：

```sh
cd app
./gradlew :runner:assembleDebug \
  -Pkaiagentd.prebuiltBinary=/absolute/path/to/libkaiagentd.so
```

构建 Release 原生程序和 APK：

```sh
cd app
./gradlew :installer:assembleRelease :runner:assembleRelease \
  -Pkaiagentd.buildType=Release \
  -Pkaiagentd.buildDir=cmake-build-release
```

APK 位于 `app/installer/build/outputs/apk/` 和 `app/runner/build/outputs/apk/`。

### Windows

设置 `JAVA_HOME`、`ANDROID_HOME` 和 `ANDROID_NDK_HOME`，将 CMake 与 Ninja 加入 `PATH`，然后在仓库根目录运行：

```bat
build_app_debug.bat
build_app_release.bat
```

可通过 `KAIAGENTD_CMAKE` 指定 CMake 可执行文件。脚本使用当前仓库位置，不包含开发者的个人磁盘路径。

### 可选的 Release 签名

以下四个环境变量必须全部配置或全部省略，值由本机或构建环境提供：

| 变量 | 含义 |
| --- | --- |
| `KAIAGENTD_KEYSTORE` | 自有签名密钥文件的绝对路径 |
| `KAIAGENTD_STORE_PASSWORD` | keystore 密码 |
| `KAIAGENTD_KEY_ALIAS` | 签名条目别名 |
| `KAIAGENTD_KEY_PASSWORD` | 签名条目密码 |

项目不提供默认私钥或口令。密钥、`.env`、`local.properties` 和生成物均应保留在本地；不要将真实值写入构建脚本。

## 使用

1. 按 [运行时资源说明](lib/arm64-v8a/README.md) 准备 ARM64 运行库和模型，构建 Installer 与 Runner。
2. 在兼容设备上安装这两个 APK，启动 Shizuku 或使用 root，并向应用授予相应权限。
3. 打开 Installer，将资源部署到 `/data/local/tmp/com.enenh.ai/lib`。
4. 打开 Runner 启动程序。启动流程不需要登录或卡密。

程序默认使用 `model_fast256_int8.tflite`。其他模型的文件名、输入输出约定见运行时资源说明。缺少模型或动态库时，程序会输出错误；仅有源码构建产物不等于已具备完整的推理运行环境。

Windows 上也可先运行 `init.bat`，将本地运行库与模型部署到通过 ADB 连接的设备，再运行 `m.bat <path-to-libkaiagentd.so>` 推送并启动原生程序。`m.bat` 要求运行时资源已经就绪。

### Debug 画面采集诊断

完成资源部署后，在 Android 设备上具有所需权限的 shell 中直接启动 Debug 原生程序，可使用以下参数：

```sh
cd /data/local/tmp/com.enenh.ai/lib
export LD_LIBRARY_PATH="$PWD"
./libkaiagentd.so --capture-smoke-test
./libkaiagentd.so --capture-benchmark --capture-backend=cpuconsumer --capture-frames=120
./libkaiagentd.so --capture-benchmark --capture-backend=aimage --capture-frames=120
```

每次选择一条诊断命令执行。程序会进入后台，日志位于 `/data/local/tmp/com.enenh.ai/kaiagentd.log`。默认后端为 `cpuconsumer`；benchmark 实际帧数限制为 1–2000。Release 构建不启用这些调试参数。

## 检查与 CI

提交前先暂存文件，再运行公开源码检查，以检查实际即将提交的内容：

```sh
git add .
python3 scripts/check_public_tree.py
git diff --cached --check
```

检查器用于拦截已知类型的敏感文件、凭据和遗留授权实现，不替代人工审查，也不验证模型或第三方二进制的许可。

GitHub Actions 对每次 push 触发构建，不设置分支、标签或文件路径过滤，也不会自动取消此前的构建。可以在 Actions 页面手动运行；Pull Request 会执行编译检查。

push 或手动运行构建成功后，产物直接发布到 [GitHub Releases](https://github.com/enenH/kaiagentd1/releases)，不上传 Actions Artifacts。每次运行使用独立标签 `build-<构建编号>-<尝试编号>-<12位提交号>`，标签指向本次实际构建的提交；首次尝试编号为 1。重跑会创建新版本，已发布的文件不会被覆盖。默认分支当前提交的成功构建会标记为 Latest。删除分支或标签的事件不发布 Release。

每个 Release 提供：

| 文件 | 内容 |
| --- | --- |
| `kaiagentd1-installer-debug.apk` | Installer 调试 APK |
| `kaiagentd1-runner-debug.apk` | Runner 调试 APK |
| `kaiagentd1-installer-release-unsigned.apk` | Installer 未签名 Release APK |
| `kaiagentd1-runner-release-unsigned.apk` | Runner 未签名 Release APK |
| `libkaiagentd.so` | ARM64 原生可执行程序 |
| `licenses.zip` | 项目和第三方许可证说明 |
| `SHA256SUMS` | 上述六个文件的 SHA-256 校验值 |

发布使用 GitHub 自动提供的 `GITHUB_TOKEN` 和工作流的 `contents: write` 权限，无需额外配置 PAT。Pull Request 不发布 Release。CI 不依赖私有保护工具、外部消息服务或项目签名密钥。Android 设备上的采集、推理、绘制与触控行为仍需实机测试。

## 许可证

项目自有代码采用 [MIT License](LICENSE)。Gradle Wrapper、FreeType、Shizuku API 等组件保留各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 `licenses/`。项目许可证不授予任何未附带的运行库、模型或设备厂商文件的使用及再分发权。
