# 本地运行时资源

本目录用于在本地放置 ARM64 运行库和模型。公开仓库只提供此说明，不包含原有预编译库、模型权重或设备厂商文件。

这些文件在 `.gitignore` 中排除，公开源码检查也会阻止把二进制运行资源加入提交。源码编译不需要这些文件；设备上执行推理需要准备兼容资源。

## 运行库

根据自己取得的、具有使用权限的运行环境提供以下文件：

- `libtensorflowlite_jni.so`：必须导出程序通过 `dlsym` 使用的 TensorFlow Lite C API；不是所有同名库都满足这一条件。
- `libQnnTFLiteDelegate.so`：必须与 `src/main.cpp` 中使用的 QNN delegate 接口及结构体布局兼容。
- 配套 QNN HTP / system / prepare 运行库，以及与目标芯片匹配的 HTP stub / skel 库。

必要的 Android system/vendor 库由 Installer 或 `init.bat` 从当前设备获取。不要将其他设备的系统库视为可通用、可再分发的依赖。

仓库未记录原预编译依赖的可验证版本信息，因此这里不承诺任意新版 QNN SDK 或 TensorFlow Lite 二进制与当前接口兼容。版本匹配和设备运行需要自行验证。

## 模型

默认模型必须存在；其他模型用于运行时切换：

| 文件名 | 输入约定 | 预测框数量 | 类别数量 | 输出方式 |
| --- | --- | --- | --- | --- |
| `model_fast256_int8.tflite`（默认） | INT8，`[1, 256, 256, 3]` | 1344 | 6 | 分离的 boxes / scores，FLOAT32 输出 |
| `model_int8.tflite` | INT8，`[1, 416, 416, 3]` | 3549 | 6 | 分离的 boxes / scores，FLOAT32 输出 |
| `model.tflite` | FLOAT32，`[1, 416, 416, 3]` | 3549 | 6 | 单输出，fp16 推理配置 |

文件名和配置来自 `src/main.cpp` 中的 `kModelVariants`；完整的布局、量化参数和预处理要求以 `Detector::validateTensors` 及相关推理代码为准。仅重命名一个不匹配的 `.tflite` 文件无法使它兼容。模型名称中 `fp16` 指推理配置，输入张量仍使用 FLOAT32。

## 打包和部署

1. 在本目录放入自己有权使用的运行库与匹配模型。
2. 构建 `:installer:assembleDebug` 或 `:installer:assembleRelease`。
3. 在兼容设备上授予 Shizuku / root 权限，使用 Installer 推送资源。
4. 构建并运行 Runner，查看缺失库、模型类型或维度错误的日志。

运行时目录为 `/data/local/tmp/com.enenh.ai/lib`。仅包含此 README 的 Installer 会明确提示缺少资源，不会把说明文件当作可用模型。
