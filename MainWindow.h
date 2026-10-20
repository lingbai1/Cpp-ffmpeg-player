#pragma once

#include <QMainWindow>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QTimer>
#include <QShortcut>
#include <memory>
#include <vector>
#include <string>
#include <thread>

class SdlVideoWidget;
class VideoState;
class QSplitter;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /** 加载播放列表并从头开始播放 */
    void loadPlaylist(const std::vector<std::string>& files);

    /** 加载单个文件 */
    void loadFile(const std::string& filePath);

signals:
    /** 请求切歌（由 MainWindow 内部处理，也可供外部调用） */
    void trackChangeRequested(int index);

private slots:
    // 播放列表交互
    void onPlaylistDoubleClicked(QListWidgetItem* item);    //双击播放列表项，切换到该曲目
    void onPrevClicked();
    void onPlayPauseClicked();
    void onNextClicked();
    void onAddFilesClicked();
    void onAddUrlClicked();
    void onToggleFullscreen();
    void onPlaylistContextMenu(const QPoint& pos);

    // 进度条
    void onProgressSliderPressed();
    void onProgressSliderReleased();
    void onProgressSliderMoved(int value);
    void onVolumeChanged(int value);

    // 定时刷新进度条
    void onRefreshProgress();

    // 视频区域鼠标拖拽进度条（来自 SdlVideoWidget）
    void onVideoMousePressed(double nx, double ny);
    void onVideoMouseMoved(double nx, double ny);
    void onVideoMouseReleased(double nx, double ny);

    /** 切到指定曲目 */
    void switchTrack(int index);

    /** 打开线程完成后的回调（在 GUI 线程执行） */
    void onOpenFinished(bool ok);

    /** 当前曲目播放到末尾（EOF），自动连播下一首 */
    void onTrackFinished();

    /** 打开失败后自动尝试下一首（全部失败则停止） */
    void autoSkipNext();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUi();
    void connectSignals();
    void setupShortcuts();
    void updatePlayPauseButton();
    void updateFullscreenButton();

    /** 启动/停止播放管线 */
    bool startPlayback(const std::string& filePath);
    void stopPlayback();
    void seekTo(double posSeconds);

    /** 将本地路径或网络 URL 加入播放列表（去重） */
    void addToPlaylist(const std::string& source);

    /** 列表显示名：本地文件显示文件名，网络流显示完整 URL */
    QString displayNameFor(const std::string& source) const;

    /** 判断是否为网络 URL */
    static bool isNetworkUrl(const std::string& s);

    /** 从播放列表移除指定行（若为当前播放项则停止播放） */
    void removePlaylistItem(int row);

    /** 拖拽排序后，按控件顺序重建 m_playlist 并重定位当前索引 */
    void syncPlaylistOrderFromWidget();

    /** 加载/保存 URL 输入历史（QSettings 持久化，最多 10 条） */
    void loadUrlHistory();
    void saveUrlHistory();

    // ---- UI 控件 ----
    SdlVideoWidget* m_videoWidget   = nullptr;
    QSplitter*      m_videoSplitter = nullptr;
    QWidget*        m_controlBar   = nullptr;
    QListWidget*    m_playlistWidget = nullptr;
    QPushButton*    m_prevBtn       = nullptr;
    QPushButton*    m_playPauseBtn  = nullptr;
    QPushButton*    m_nextBtn       = nullptr;
    QPushButton*    m_fullscreenBtn = nullptr;
    QPushButton*    m_addFileBtn    = nullptr;
    QComboBox*      m_urlCombo      = nullptr;
    QPushButton*    m_addUrlBtn     = nullptr;
    QSlider*        m_progressSlider = nullptr;
    QSlider*        m_volumeSlider = nullptr;
    QLabel*         m_timeLabel     = nullptr;//显示当前播放时间
    QLabel*         m_durationLabel = nullptr;//显示总时长
    QLabel*         m_statusLabel   = nullptr;
    QTimer*         m_progressTimer = nullptr;//定时刷新进度条

    // 快捷键（不受焦点影响，SDL 子窗口上也能触发）
    QShortcut*      m_spaceShortcut = nullptr;
    QShortcut*      m_leftShortcut  = nullptr;
    QShortcut*      m_rightShortcut = nullptr;
    QShortcut*      m_openShortcut  = nullptr;
    QShortcut*      m_fullscreenShortcut = nullptr;

    // ---- 数据 ----
    std::vector<std::string> m_playlist;
    int m_currentIndex = -1;
    // 当前正在播放的源（拖拽排序后用它重新定位索引）
    std::string m_playingSource;
    // 自动跳过起始索引（-1 表示未在自动跳过流程中）
    int m_autoSkipStart = -1;

    // ---- 播放器核心 ----
    std::unique_ptr<VideoState> m_videoState;

    // ---- 异步打开（网络流连接可能耗时，不阻塞 GUI） ----
    std::thread m_openThread;
    bool m_opening = false;

    // ---- 进度条状态 ----
    bool m_progressDragging = false;
    double m_dragRatio = 0.0;
};
