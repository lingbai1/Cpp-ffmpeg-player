# Player - 基于 FFmpeg + SDL2 + Qt5 的多媒体播放器

一个使用 **FFmpeg + SDL2 + Qt5** 开发的本地/网络音视频播放器。项目实现了完整的音视频处理管线：解封装、音视频多线程独立解码、音频重采样、音画同步、像素格式转换，并通过 Qt Widgets 提供图形界面、播放列表与进度条等交互能力。视频画面由 SDL2 渲染后嵌入到 Qt 窗口（`SDL_CreateWindowFrom` + `winId()`），实现「Qt 管布局，SDL 管渲染」。

## 主要功能

- **图形化界面（Qt5 Widgets）**
  - 左侧视频区 + 右侧播放列表 + 底部控制栏，支持 `QSplitter` 拖拽调整布局比例。
  - 播放列表支持**双击切换、拖拽排序、右键菜单（播放/移除）**。
- **播放列表与连播**
  - 支持一次传入多个文件，按顺序播放。
  - 当前曲目播放到末尾（EOF）自动连播下一首；全部播放完自动停止。
  - 打开失败时自动跳过并尝试下一首，全部失败则弹窗提示。
- **本地文件与网络流**
  - 本地文件：通过「添加文件」按钮（`Ctrl+O`）或命令行传入。
  - 网络流：支持 `http(s)/HLS`、`rtsp`、`rtmp`、`udp`、`rtp`、`srt`、`mms`、`ftp` 等协议。
  - 网络 URL 输入框带最近输入历史（`QSettings` 持久化，最多 10 条）。
  - 网络流的打开在后台线程完成，避免阻塞 GUI。
- **播放控制**
  - 播放 / 暂停、上一首 / 下一首、进度条拖拽与点击跳转、视频区域内鼠标拖拽 Seek。
  - 底部音量滑块调节音量，支持无音频视频直接按视频时钟播放。
  - 全屏切换（`F11` / `Esc` 退出），窗口拉伸时画面自动维持宽高比居中显示（黑边填充）。
- **快捷键（全局，不受焦点影响）**

  | 按键 | 功能 |
  | --- | --- |
  | `Space` | 播放 / 暂停 |
  | `←` / `→` | 快退 / 快进 10 秒 |
  | `Ctrl+O` | 添加文件 |
  | `F11` | 切换全屏 |
  | `Esc` | 退出全屏 |

## 开发环境

- CMake >= 3.5
- 编译器：支持 C++11（g++ / clang++）
- FFmpeg：`libavformat`、`libavcodec`、`libavutil`、`libswscale`、`libswresample`
- SDL2
- Qt5（Widgets 模块）
- pkg-config
- X11（Linux 下 SDL 嵌入 Qt 窗口需要 XCB 后端）

## 编译与构建

推荐使用外部构建：

```bash
mkdir -p build && cd build
cmake ..
make
```
生成的可执行文件位于 `build/player`。

## 运行方式

```bash
./build/player [文件或URL ...]

# 运行示例：
./build/player ../test_video.mp4
./build/player ../test_video.mp4 ../out.mp3
./build/player "http://example.com/stream.m3u8"
./build/player "rtsp://127.0.0.1:8554/live"
```

也可以直接启动播放器，再通过界面上的「添加文件」或「添加URL」加入媒体。

> 说明：程序在启动时会强制使用 XCB（X11）后端（`QT_QPA_PLATFORM=xcb`），因为 SDL 的 `SDL_CreateWindowFrom` 需要 X11 原生窗口句柄，Wayland 后端下 `winId()` 返回的是 Wayland surface ID，SDL 无法使用。

## 核心实现与技术点

- **多线程架构**
  解封装（Demux）、视频解码、音频解码分别运行在独立线程（`SDL_Thread`），SDL 音频回调负责音频输出，Qt 的 `QTimer`（约 60fps）驱动画面渲染，主线程处理 GUI 事件。
- **线程同步与缓冲队列**
  基于 `std::mutex` 与 `std::condition_variable` 实现 `PacketQueue`、`FrameQueue`、`PCMQueue` 三类跨线程队列，并设置最大缓存阈值以限制内存增长；帧带 `serial` 序列号用于丢弃 Seek 后的过时数据。
- **音画同步（A/V Sync）**
  以音频播放进度为 Master Clock，视频渲染时比较视频帧 PTS 与音频时钟差值，动态使用 `SDL_Delay` 控制节奏，差异过大时执行丢帧策略，保持声画同步。
  对没有音频流的媒体，播放器跳过音频解码和音画同步，直接按视频帧解码与视频时钟播放。
- **重采样与像素转换**
  使用 `SwrContext` 将各种输入音频格式统一重采样为 SDL 播放格式；使用 `sws_scale` 将视频帧转换为 YUV420P 后交由 SDL 纹理渲染。
- **Seek 与资源管理**
  Seek 时清空编解码器缓冲与各线程队列（Flush），关闭播放时按序释放音频设备、编解码器、格式上下文、SDL 资源及 `sws_ctx` / `swr_ctx`，保证稳定运行。

## 项目结构

```
Player/
├── main.cpp               # 程序入口：初始化 SDL/Qt、解析命令行、创建主窗口
├── MainWindow.{h,cpp}     # Qt 主窗口：布局、播放列表、控制栏、快捷键、连播逻辑
├── SdlVideoWidget.{h,cpp} # 将 SDL2 窗口嵌入 Qt Widget 的封装
├── Player.{h,cpp}         # 播放器核心：打开/关闭媒体、音画同步、Seek、渲染
├── VideoState.{h,cpp}     # 播放状态结构体（格式上下文、解码器、SDL 资源等）
├── Demux.{h,cpp}          # 解封装线程
├── Decoder_video.{h,cpp}  # 视频解码线程
├── Decoder_audio.{h,cpp}  # 音频解码线程
├── PacketQueue.{h,cpp}    # AVPacket 缓冲队列
├── FrameQueue.{h,cpp}     # AVFrame 缓冲队列
├── PCMQueue.{h,cpp}       # PCM 音频缓冲队列
├── AudioBuffer.h          # 音频回调残余数据缓冲
├── FrameItem.h            # 队列中的视频帧结构
└── CMakeLists.txt         # 构建脚本
```
