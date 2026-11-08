#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QFileInfo>
#include <vector>
#include <string>
#include "MainWindow.h"
#include "VideoState.h"
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

    QCommandLineOption seekStressOpt(
        "seek-stress",
        "启用 seek 压测模式：后台打开媒体并循环 seek，完成后自动退出（用于排查 seek 崩溃）"
    );
    parser.addOption(seekStressOpt);

    // 支持传入多个文件作为播放列表
    parser.addPositionalArgument("files", "要播放的视频/音频文件（可多个）", "[files...]");

    parser.process(app);

    QStringList args = parser.positionalArguments();
    const bool seekStress = parser.isSet(seekStressOpt);
    std::vector<std::string> files;
    for (const QString& arg : args) {
        files.push_back(arg.toStdString());
    }

    if (seekStress) {
        if (files.empty()) {
            fprintf(stderr, "--seek-stress requires one input file\n");
            return 2;
        }

        const QString path = QString::fromStdString(files.front());
        if (!QFileInfo(path).exists()) {
            fprintf(stderr, "seek-stress input not found: %s\n", path.toUtf8().constData());
            return 2;
        }

        VideoState is;
        // seek 压测无需视频渲染资源；创建最小 SDL 隐藏窗口与软件渲染器即可。
        is.window = SDL_CreateWindow("seek-stress", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                     64, 64, SDL_WINDOW_HIDDEN);
        if (!is.window) {
            fprintf(stderr, "seek-stress SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 3;
        }
        is.renderer = SDL_CreateRenderer(is.window, -1, SDL_RENDERER_SOFTWARE);
        if (!is.renderer) {
            fprintf(stderr, "seek-stress SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(is.window);
            is.window = nullptr;
            return 3;
        }

        if (!player_open(&is, files.front().c_str())) {
            fprintf(stderr, "seek-stress player_open failed\n");
            player_close(&is);
            return 4;
        }

        const double total = get_total_seconds(&is);
        if (total <= 5.0) {
            fprintf(stderr, "seek-stress skipped: total duration too short or unknown\n");
            player_close(&is);
            return 5;
        }

        // 连续 seek，覆盖前中后段，观察是否崩溃。
        for (int i = 0; i < 60; ++i) {
            double ratio = (i % 10) / 10.0;
            double target = 1.0 + ratio * (total - 2.0);
            seek(&is, target);
            SDL_Delay(30);
        }

        player_close(&is);
        fprintf(stdout, "seek-stress done\n");
        return 0;
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
