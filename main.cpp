#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <vector>
#include <string>
#include "MainWindow.h"
#include "Player.h"

int main(int argc, char* argv[])
{
    // 强制 Qt 使用 XCB (X11) 后端，而非 Wayland。
    // SDL2 的 SDL_CreateWindowFrom / XReparentWindow 需要 X11 窗口句柄，
    // Wayland 后端下 winId() 返回的是 Wayland surface ID，SDL 无法使用。
    qputenv("QT_QPA_PLATFORM", "xcb");

    // SDL 必须在 QApplication 之前初始化
    if (!sdl_global_init()) {
        fprintf(stderr, "SDL initialization failed\n");
        return -1;
    }

    QApplication app(argc, argv);
    app.setApplicationName("Player");
    app.setApplicationVersion("2.0 Qt Edition");

    // ---- 命令行参数解析 ----
    QCommandLineParser parser;
    parser.setApplicationDescription("基于 FFmpeg + SDL2 + Qt 的多媒体播放器");
    parser.addHelpOption();
    parser.addVersionOption();

    // 支持传入多个文件作为播放列表
    parser.addPositionalArgument("files", "要播放的视频/音频文件（可多个）", "[files...]");

    parser.process(app);

    QStringList args = parser.positionalArguments();
    std::vector<std::string> files;
    for (const QString& arg : args) {
        files.push_back(arg.toStdString());
    }

    // ---- 构建主窗口 ----
    MainWindow window;

    // 先 show() 确保原生窗口句柄就绪，再加载播放列表
    window.show();

    if (!files.empty()) {
        // 延迟到 Qt 事件循环启动后再加载，确保 X11 原生窗口已映射
        QTimer::singleShot(0, [&window, files]() {
            window.loadPlaylist(files);
        });
    }

    return app.exec();
}
