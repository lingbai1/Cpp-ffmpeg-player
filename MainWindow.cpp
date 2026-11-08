#include "MainWindow.h"
#include "SdlVideoWidget.h"
#include "VideoState.h"
#include "Player.h"
#include "PacketQueue.h"
#include "FrameQueue.h"
#include "PCMQueue.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QMessageBox>
#include <QFileInfo>
#include <QFileDialog>
#include <QMenu>
#include <QSettings>
#include <algorithm>
#include <cstdio>

// ================ 构造与析构 ================

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setupUi();
    connectSignals();           // 绑定信号槽
    setupShortcuts();           // 绑定快捷键
    setWindowTitle("Player - Qt Edition");
    resize(1280, 800);

    // 给进度条安装事件过滤器，实现点击跳转
    m_progressSlider->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    stopPlayback();
}

// ================ UI 搭建 ================

void MainWindow::setupUi()
{
    // ---- 中央 Widget ----
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    // ---- 左侧：视频区域 ----
    m_videoWidget = new SdlVideoWidget(this);
    m_videoWidget->setMinimumSize(720, 405);
    m_videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoWidget->setStyleSheet("background-color: black;");

    // ---- 右侧：播放列表面板 ----
    QVBoxLayout* rightLayout = new QVBoxLayout();
    QLabel* playlistLabel = new QLabel("播放列表", this);
    playlistLabel->setStyleSheet("font-weight: bold; font-size: 14px; padding: 4px;");

    m_playlistWidget = new QListWidget(this);
    m_playlistWidget->setMinimumWidth(160);
    m_playlistWidget->setMaximumWidth(220);
    m_playlistWidget->setAlternatingRowColors(true);
    // 支持拖拽排序与右键菜单
    m_playlistWidget->setDragDropMode(QAbstractItemView::InternalMove);
    m_playlistWidget->setDefaultDropAction(Qt::MoveAction);
    m_playlistWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playlistWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    m_playlistWidget->setToolTip("拖拽可排序；右键可播放/移除");

    m_addFileBtn = new QPushButton("添加文件", this);
    m_addFileBtn->setToolTip("添加媒体文件到播放列表 (Ctrl+O)");

    // 网络 URL 输入行（可编辑下拉框，带最近输入历史）
    QHBoxLayout* urlLayout = new QHBoxLayout();
    urlLayout->setSpacing(4);
    m_urlCombo = new QComboBox(this);
    m_urlCombo->setEditable(true);
    m_urlCombo->setInsertPolicy(QComboBox::NoInsert);   // 历史由 saveUrlHistory 统一管理
    m_urlCombo->lineEdit()->setPlaceholderText("输入网络 URL：http://… rtsp://… rtmp://…");
    m_urlCombo->lineEdit()->setClearButtonEnabled(true);
    m_urlCombo->setToolTip("支持 HTTP/HLS、RTSP、RTMP、UDP 等网络流，回车直接加入播放列表");
    loadUrlHistory();
    m_addUrlBtn = new QPushButton("添加URL", this);
    m_addUrlBtn->setToolTip("将 URL 加入播放列表并开始播放");
    urlLayout->addWidget(m_urlCombo);
    urlLayout->addWidget(m_addUrlBtn);

    rightLayout->addWidget(playlistLabel);
    rightLayout->addLayout(urlLayout);
    rightLayout->addWidget(m_addFileBtn);
    rightLayout->addWidget(m_playlistWidget);
    rightLayout->addStretch();

    // ---- 底部控制栏 ----
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setSpacing(6);

    m_prevBtn      = new QPushButton("⏮", this);
    m_playPauseBtn = new QPushButton("⏯", this);
    m_nextBtn      = new QPushButton("⏭", this);
    m_fullscreenBtn = new QPushButton("全屏", this);

    m_prevBtn->setFixedSize(40, 36);
    m_playPauseBtn->setFixedSize(40, 36);
    m_nextBtn->setFixedSize(40, 36);
    m_fullscreenBtn->setFixedSize(56, 36);

    m_prevBtn->setToolTip("上一首");
    m_playPauseBtn->setToolTip("播放/暂停");
    m_nextBtn->setToolTip("下一首");
    m_fullscreenBtn->setToolTip("切换全屏 (F11)");

    m_timeLabel     = new QLabel("00:00", this);
    m_progressSlider = new QSlider(Qt::Horizontal, this);
    m_durationLabel = new QLabel("00:00", this);

    m_timeLabel->setFixedWidth(50);
    m_durationLabel->setFixedWidth(50);
    m_timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_durationLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_progressSlider->setRange(0, 1000);  // 用千分比表示进度
    m_progressSlider->setValue(0);
    m_progressSlider->setTracking(true);  // 拖动时实时更新

    QLabel* volumeLabel = new QLabel("音量", this);
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(100);
    m_volumeSlider->setToolTip("调节音量");

    controlLayout->addWidget(m_prevBtn);
    controlLayout->addWidget(m_playPauseBtn);
    controlLayout->addWidget(m_nextBtn);
    controlLayout->addWidget(m_fullscreenBtn);
    controlLayout->addWidget(m_timeLabel);
    controlLayout->addWidget(m_progressSlider);
    controlLayout->addWidget(m_durationLabel);
    controlLayout->addWidget(volumeLabel);
    controlLayout->addWidget(m_volumeSlider);

    m_statusLabel = new QLabel("就绪", this);
    m_statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    controlLayout->addWidget(m_statusLabel, 1);

    // ---- 整体布局 ----
    // 使用 QSplitter 让视频和列表可拖拽调整比例
    m_videoSplitter = new QSplitter(Qt::Horizontal, this);
    m_videoSplitter->addWidget(m_videoWidget);
    QWidget* rightPanel = new QWidget(this);
    rightPanel->setLayout(rightLayout);
    rightPanel->setMinimumWidth(180);
    rightPanel->setMaximumWidth(220);
    m_videoSplitter->addWidget(rightPanel);
    m_videoSplitter->setStretchFactor(0, 1);
    m_videoSplitter->setStretchFactor(1, 0);
    m_videoSplitter->setSizes({1050, 200});

    QVBoxLayout* rootLayout = new QVBoxLayout();
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(2);
    rootLayout->addWidget(m_videoSplitter, 1);
    m_controlBar = new QWidget(this);
    m_controlBar->setLayout(controlLayout);
    rootLayout->addWidget(m_controlBar, 0);

    central->setLayout(rootLayout);

    // ---- 进度刷新定时器 ----
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(250);  // 每 250ms 刷新一次进度条
}

void MainWindow::connectSignals()
{
    // 播放列表双击
    connect(m_playlistWidget, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onPlaylistDoubleClicked);

    // 控制按钮
    connect(m_prevBtn,      &QPushButton::clicked, this, &MainWindow::onPrevClicked);
    connect(m_playPauseBtn, &QPushButton::clicked, this, &MainWindow::onPlayPauseClicked);
    connect(m_nextBtn,      &QPushButton::clicked, this, &MainWindow::onNextClicked);
    connect(m_fullscreenBtn, &QPushButton::clicked, this, &MainWindow::onToggleFullscreen);
    connect(m_addFileBtn, &QPushButton::clicked, this, &MainWindow::onAddFilesClicked);
    connect(m_addUrlBtn, &QPushButton::clicked, this, &MainWindow::onAddUrlClicked);
    connect(m_urlCombo->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::onAddUrlClicked);

    // 播放列表右键菜单
    connect(m_playlistWidget, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onPlaylistContextMenu);

    // 拖拽排序完成后同步内部数据
    connect(m_playlistWidget->model(), &QAbstractItemModel::rowsMoved,
            this, [this](const QModelIndex&, int, int, const QModelIndex&, int) {
                syncPlaylistOrderFromWidget();
            });

    // 进度条 (Qt QSlider)
    connect(m_progressSlider, &QSlider::sliderPressed,  this, &MainWindow::onProgressSliderPressed);
    connect(m_progressSlider, &QSlider::sliderReleased, this, &MainWindow::onProgressSliderReleased);
    connect(m_progressSlider, &QSlider::sliderMoved,    this, &MainWindow::onProgressSliderMoved);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);

    // 视频区域鼠标拖拽进度条
    connect(m_videoWidget, &SdlVideoWidget::videoMousePressed,
            this, &MainWindow::onVideoMousePressed);
    connect(m_videoWidget, &SdlVideoWidget::videoMouseMoved,
            this, &MainWindow::onVideoMouseMoved);
    connect(m_videoWidget, &SdlVideoWidget::videoMouseReleased,
            this, &MainWindow::onVideoMouseReleased);

    // 进度刷新
    connect(m_progressTimer, &QTimer::timeout, this, &MainWindow::onRefreshProgress);
}

// ================ 播放列表加载 ================

void MainWindow::loadPlaylist(const std::vector<std::string>& files)
{
    m_playlist = files;
    m_playlistWidget->clear();

    // 清空播放列表

    for (size_t i = 0; i < files.size(); i++) {
        QString full = QString::fromStdString(files[i]);
        QListWidgetItem* item = new QListWidgetItem(displayNameFor(files[i]));
        item->setData(Qt::UserRole, full);      // 完整路径/URL 存 UserRole
        item->setToolTip(full);
        m_playlistWidget->addItem(item);
    }

    if (!files.empty()) {
        switchTrack(0);         
    }
}

void MainWindow::loadFile(const std::string& filePath)
{
    loadPlaylist({filePath});
}

// ================ 切歌逻辑（核心） ================

void MainWindow::switchTrack(int index)         // 切换到指定索引的歌曲
{
    if (index < 0 || index >= (int)m_playlist.size()) return;

    // 1. 先停止当前播放
    stopPlayback();

    m_currentIndex = index;

    // 2. 高亮当前曲目
    m_playlistWidget->setCurrentRow(index);
    m_playlistWidget->scrollToItem(m_playlistWidget->item(index));  //确保当前曲目可见

    // 3. 启动新播放（网络流在后台线程打开，成功后 onOpenFinished 开始渲染）
    const std::string& source = m_playlist[index];

    // 本地文件先做存在性检查，避免无谓的异步打开
    if (!isNetworkUrl(source) && !QFileInfo(QString::fromStdString(source)).exists()) {
        QMessageBox::warning(this, "错误",
            QString("文件不存在：%1").arg(QString::fromStdString(source)));
        return;
    }

    bool ok = startPlayback(source);
    if (!ok) {
        QMessageBox::warning(this, "错误",
            QString("无法播放：%1").arg(QString::fromStdString(source)));
        return;
    }
    m_playingSource = source;

    // 4. 渲染 + 进度刷新延迟到 onOpenFinished 再启动
    updatePlayPauseButton();
}

// ================ 播放控制 ================

bool MainWindow::startPlayback(const std::string& filePath)
{
    // 创建新的 VideoState
    m_videoState.reset(new VideoState());

    // 将 SDL 窗口绑定到 Qt Widget（必须在 GUI 线程创建）
    m_videoWidget->attachVideoState(m_videoState.get());

    VideoState* is = m_videoState.get();
    const std::string source = filePath;

    // 打开媒体源可能阻塞（尤其是网络流），放到后台线程执行，避免卡死界面
    m_opening = true;
    m_statusLabel->setText("正在连接：" + displayNameFor(source));

    m_openThread = std::thread([this, is, source]() {
        bool ok = player_open(is, source.c_str());
        // 切回 GUI 线程处理结果
        QMetaObject::invokeMethod(this, [this, is, ok]() {
            // 期间可能已切歌/关闭，状态不一致则忽略本次结果
            if (m_videoState.get() != is) return;
            onOpenFinished(ok);
        }, Qt::QueuedConnection);
    });

    return true;
}

void MainWindow::onOpenFinished(bool ok)
{
    m_opening = false;

    if (m_openThread.joinable()) m_openThread.join();

    if (!ok || !m_videoState) {
        // 打开失败：清理残留资源，并自动尝试下一首
        if (m_videoState) {
            player_close(m_videoState.get());
            m_videoState.reset();
        }
        updatePlayPauseButton();
        autoSkipNext();
        return;
    }

    // 打开成功：开始渲染 + 进度刷新，并重置自动跳过状态
    m_autoSkipStart = -1;
    m_videoWidget->startRendering(16);
    m_progressTimer->start();
    updatePlayPauseButton();

    QString name;
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_playlist.size()) {
        name = displayNameFor(m_playlist[m_currentIndex]);
    }
    m_statusLabel->setText("播放中：" + name);
    setWindowTitle("Player - " + name);
}

void MainWindow::stopPlayback()
{
    // 先置 quit，阻止定时渲染槽继续进入 render_frame。
    if (m_videoState) {
        m_videoState->quit = true;
    }

    m_progressTimer->stop();
    m_videoWidget->stopRendering();

    // 打开线程仍在运行时：先通过中断回调打断阻塞的 IO，再等待其结束
    if (m_openThread.joinable()) {
        if (m_videoState) m_videoState->abort_request = true;
        m_openThread.join();
    }
    m_opening = false;

    m_playingSource.clear();

    if (!m_videoState) return;

    // player_close 负责：停线程、清理 codec/format/queue/SDL 渲染资源
    player_close(m_videoState.get());

    // 释放 VideoState 对象（析构函数会清理残留资源，player_close 已设为 nullptr 的部分会安全跳过）
    m_videoState.reset();

    // 重置进度条
    m_progressSlider->setValue(0);
    m_timeLabel->setText("00:00");
    m_durationLabel->setText("00:00");
}

void MainWindow::seekTo(double posSeconds)
{
    if (!m_videoState) return;
    // 直播流无时长信息，不支持 seek
    if (get_total_seconds(m_videoState.get()) <= 0) return;
    seek(m_videoState.get(), posSeconds);
}

// ================ 槽函数：播放列表交互 ================

void MainWindow::onPlaylistDoubleClicked(QListWidgetItem* item)
{
    int row = m_playlistWidget->row(item);          //确定当前双击的行号 即是索引
    if (row >= 0 && row != m_currentIndex) {
        switchTrack(row);
    }
}

void MainWindow::onPrevClicked()
{
    if (m_playlist.empty()) return;
    int newIdx = (m_currentIndex - 1 + m_playlist.size()) % m_playlist.size();
    switchTrack(newIdx);
}

void MainWindow::onPlayPauseClicked()
{
    if (!m_videoState) return;

    m_videoState->pause = !m_videoState->pause;

    if (m_videoState->audio_dev != 0) {
        SDL_PauseAudioDevice(m_videoState->audio_dev, m_videoState->pause ? 1 : 0);
    }

    updatePlayPauseButton();
}

void MainWindow::onNextClicked()
{
    if (m_playlist.empty()) return;
    int newIdx = (m_currentIndex + 1) % m_playlist.size();
    switchTrack(newIdx);
}

void MainWindow::onAddFilesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        "选择要加入播放列表的文件",
        QString(),
        "媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv *.webm *.mp3 *.aac *.wav *.flac);;所有文件 (*.*)"
    );
    if (files.isEmpty()) return;

    for (const QString& f : files) {
        addToPlaylist(f.toStdString());
    }
}

void MainWindow::onAddUrlClicked()
{
    QString text = m_urlCombo->currentText().trimmed();
    if (text.isEmpty()) return;

    addToPlaylist(text.toStdString());
    saveUrlHistory();
    m_urlCombo->setCurrentIndex(-1);
    m_urlCombo->clearEditText();
}

void MainWindow::addToPlaylist(const std::string& source)
{
    if (source.empty()) return;

    // 去重
    if (std::find(m_playlist.begin(), m_playlist.end(), source) != m_playlist.end()) {
        return;
    }

    bool wasEmpty = m_playlist.empty();
    m_playlist.push_back(source);

    QString full = QString::fromStdString(source);
    QListWidgetItem* item = new QListWidgetItem(displayNameFor(source));
    item->setData(Qt::UserRole, full);
    item->setToolTip(full);
    m_playlistWidget->addItem(item);

    // 列表原本为空时，自动开始播放第一项
    if (wasEmpty) {
        switchTrack(0);
    }
}

QString MainWindow::displayNameFor(const std::string& source) const
{
    if (isNetworkUrl(source)) {
        return QString::fromStdString(source);   // 网络流显示完整 URL
    }
    QFileInfo fi(QString::fromStdString(source));
    QString name = fi.fileName();
    return name.isEmpty() ? QString::fromStdString(source) : name;
}

bool MainWindow::isNetworkUrl(const std::string& s)
{
    if (s.find("://") != std::string::npos) return true;
    static const char* schemes[] = { "http:", "https:", "rtsp:", "rtmp:",
                                     "udp:", "rtp:", "srt:", "mms:", "ftp:" };
    for (const char* p : schemes) {
        if (s.rfind(p, 0) == 0) return true;
    }
    return false;
}

// ================ 播放列表管理：右键菜单 / 删除 / 排序 ================

void MainWindow::onPlaylistContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_playlistWidget->itemAt(pos);
    if (!item) return;

    int row = m_playlistWidget->row(item);

    QMenu menu(this);
    QAction* playAction   = menu.addAction("播放");
    QAction* removeAction = menu.addAction("从列表移除");
    if (row == m_currentIndex) playAction->setEnabled(false);

    QAction* chosen = menu.exec(m_playlistWidget->viewport()->mapToGlobal(pos));
    if (chosen == playAction) {
        switchTrack(row);
    } else if (chosen == removeAction) {
        removePlaylistItem(row);
    }
}

void MainWindow::removePlaylistItem(int row)
{
    if (row < 0 || row >= (int)m_playlist.size()) return;

    bool removingCurrent = (row == m_currentIndex);

    if (removingCurrent) {
        stopPlayback();
        m_currentIndex = -1;
    } else if (row < m_currentIndex) {
        m_currentIndex--;
    }

    m_playlist.erase(m_playlist.begin() + row);
    delete m_playlistWidget->takeItem(row);

    if (m_playlist.empty()) {
        m_statusLabel->setText("就绪");
    }
    m_playlistWidget->setCurrentRow(m_currentIndex >= 0 ? m_currentIndex : -1);
}

void MainWindow::syncPlaylistOrderFromWidget()
{
    std::vector<std::string> newOrder;
    newOrder.reserve(m_playlistWidget->count());
    for (int i = 0; i < m_playlistWidget->count(); i++) {
        QListWidgetItem* item = m_playlistWidget->item(i);
        newOrder.push_back(item->data(Qt::UserRole).toString().toStdString());
    }
    if ((int)newOrder.size() != (int)m_playlist.size()) return;   // 防御

    m_playlist = std::move(newOrder);

    // 根据当前播放源重定位索引
    if (m_playingSource.empty()) {
        m_currentIndex = -1;
    } else {
        auto it = std::find(m_playlist.begin(), m_playlist.end(), m_playingSource);
        m_currentIndex = (it == m_playlist.end()) ? -1 : (int)(it - m_playlist.begin());
    }
}

// ================ URL 输入历史（QSettings 持久化） ================

void MainWindow::loadUrlHistory()
{
    QSettings settings("Player", "Player");
    QStringList history = settings.value("urlHistory").toStringList();
    m_urlCombo->addItems(history);
    m_urlCombo->setCurrentIndex(-1);
}

void MainWindow::saveUrlHistory()
{
    QString text = m_urlCombo->currentText().trimmed();
    if (text.isEmpty()) return;

    QSettings settings("Player", "Player");
    QStringList history = settings.value("urlHistory").toStringList();
    history.removeAll(text);
    history.prepend(text);
    while (history.size() > 10) history.removeLast();
    settings.setValue("urlHistory", history);

    // 同步刷新下拉列表
    m_urlCombo->blockSignals(true);
    m_urlCombo->clear();
    m_urlCombo->addItems(history);
    m_urlCombo->setCurrentIndex(-1);
    m_urlCombo->blockSignals(false);
}

// ================ 自动连播 / 失败自动跳过 ================

void MainWindow::onTrackFinished()
{
    if (!m_videoState || !m_videoState->eof) return;
    if (m_playlist.empty() || m_currentIndex < 0) return;

    if (m_currentIndex + 1 >= (int)m_playlist.size()) {
        // 已是最后一首：停止播放
        stopPlayback();
        m_statusLabel->setText("播放结束");
        setWindowTitle("Player - Qt Edition");
        return;
    }

    switchTrack(m_currentIndex + 1);
}

void MainWindow::autoSkipNext()
{
    m_statusLabel->setText("打开失败");

    if (m_playlist.empty()) return;

    // 只有一项：无下一首可跳
    if (m_playlist.size() <= 1) {
        int idx = m_currentIndex >= 0 ? m_currentIndex : 0;
        QString name = displayNameFor(m_playlist[idx]);
        stopPlayback();
        QMessageBox::warning(this, "错误", QString("无法打开：%1").arg(name));
        return;
    }

    if (m_autoSkipStart < 0) m_autoSkipStart = m_currentIndex;

    int next = (m_currentIndex + 1) % (int)m_playlist.size();
    if (next == m_autoSkipStart) {
        // 已轮完一圈，所有曲目都无法播放
        m_autoSkipStart = -1;
        stopPlayback();
        m_statusLabel->setText("播放失败");
        QMessageBox::warning(this, "错误", "所有曲目都无法播放");
        return;
    }

    // 静默尝试下一首
    switchTrack(next);
}

void MainWindow::onToggleFullscreen()
{
    if (isFullScreen()) {
        if (m_videoSplitter) m_videoSplitter->widget(1)->show();
        if (m_controlBar) m_controlBar->show();
        m_videoWidget->setMinimumSize(720, 405);
        showNormal();
    } else {
        if (m_videoSplitter) m_videoSplitter->widget(1)->hide();
        if (m_controlBar) m_controlBar->hide();
        m_videoWidget->setMinimumSize(0, 0);
        showFullScreen();
    }
    updateFullscreenButton();
}

void MainWindow::updatePlayPauseButton()
{
    if (!m_videoState) {
        m_playPauseBtn->setText("⏯");
        return;
    }
    m_playPauseBtn->setText(m_videoState->pause ? "▶" : "⏸");
}

void MainWindow::updateFullscreenButton()
{
    if (!m_fullscreenBtn) return;
    m_fullscreenBtn->setText(isFullScreen() ? "退出" : "全屏");
}

// ================ 槽函数：进度条控制 ================

void MainWindow::onProgressSliderPressed()
{
    m_progressDragging = true;
}

void MainWindow::onProgressSliderReleased()
{
    m_progressDragging = false;
    // 跳转到用户拖到的位置
    double total = get_total_seconds(m_videoState.get());
    if (total > 0) {
        double pos = (m_progressSlider->value() / 1000.0) * total;
        seekTo(pos);
    }
}

void MainWindow::onProgressSliderMoved(int value)
{
    // 拖动时实时显示时间
    double total = get_total_seconds(m_videoState.get());
    if (total > 0) {
        double pos = (value / 1000.0) * total;
        int mins = (int)pos / 60;
        int secs = (int)pos % 60;
        m_timeLabel->setText(QString("%1:%2")
            .arg(mins, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0')));
    }
}

void MainWindow::onVolumeChanged(int value)
{
    set_volume(m_videoState.get(), value);
}

void MainWindow::onRefreshProgress()
{
    if (!m_videoState || m_progressDragging) return;

    if (m_videoState->network_reconnecting) {
        m_statusLabel->setText("网络波动，正在重连...");
        return;
    }

    if (m_videoState->network_failed) {
        m_statusLabel->setText("网络异常，播放已停止");
        stopPlayback();
        return;
    }

    // 播放到末尾（EOF）：自动连播下一首
    if (m_videoState->eof) {
        onTrackFinished();
        return;
    }

    double total = get_total_seconds(m_videoState.get());
    double current = m_videoState->audio_enabled
        ? m_videoState->audioClock : m_videoState->videoClock;

    if (total <= 0) {
        // 直播流/未知时长：进度条不可用
        m_progressSlider->setValue(0);
        m_timeLabel->setText("--:--");
        m_durationLabel->setText("直播");
        return;
    }

    int val = (int)((current / total) * 1000);
    m_progressSlider->setValue(std::min(std::max(val, 0), 1000));

    int curMin = (int)current / 60;
    int curSec = (int)current % 60;
    int totMin = (int)total / 60;
    int totSec = (int)total % 60;

    m_timeLabel->setText(QString("%1:%2")
        .arg(curMin, 2, 10, QChar('0'))
        .arg(curSec, 2, 10, QChar('0')));
    m_durationLabel->setText(QString("%1:%2")
        .arg(totMin, 2, 10, QChar('0'))
        .arg(totSec, 2, 10, QChar('0')));
}

// ================ 槽函数：视频区域内鼠标拖拽 ================

void MainWindow::onVideoMousePressed(double nx, double ny)
{
    (void)ny;
    m_progressDragging = true;
    m_dragRatio = nx;
    m_progressSlider->setValue((int)(nx * 1000));
}

void MainWindow::onVideoMouseMoved(double nx, double ny)
{
    (void)ny;
    if (m_progressDragging) {
        m_dragRatio = nx;
        m_progressSlider->setValue((int)(nx * 1000));
    }
}

void MainWindow::onVideoMouseReleased(double nx, double ny)
{
    (void)ny;
    m_progressDragging = false;
    double total = get_total_seconds(m_videoState.get());
    if (total > 0) {
        seekTo(nx * total);
    }
}

// ================ 窗口事件 ================

void MainWindow::closeEvent(QCloseEvent* event)
{
    stopPlayback();
    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        onPlayPauseClicked();
        break;
    case Qt::Key_Right:
        if (m_videoState) {
            double current = m_videoState->audio_enabled
                ? m_videoState->audioClock : m_videoState->videoClock;
            seekTo(current + 10);
        }
        break;
    case Qt::Key_Left:
        if (m_videoState) {
            double current = m_videoState->audio_enabled
                ? m_videoState->audioClock : m_videoState->videoClock;
            seekTo(std::max(0.0, current - 10));
        }
        break;
    case Qt::Key_F11:
        onToggleFullscreen();
        break;
    case Qt::Key_Escape:
        if (isFullScreen()) {
            onToggleFullscreen();
        } else {
            QMainWindow::keyPressEvent(event);
        }
        break;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void MainWindow::setupShortcuts()
{
    // QShortcut 全局快捷键：不受焦点影响，SDL 子窗口上也能触发
    m_spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(m_spaceShortcut, &QShortcut::activated, this, &MainWindow::onPlayPauseClicked);

    m_leftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
    connect(m_leftShortcut, &QShortcut::activated, this, [this]() {
        if (m_videoState) {
            double current = m_videoState->audio_enabled
                ? m_videoState->audioClock : m_videoState->videoClock;
            seekTo(std::max(0.0, current - 10));
        }
    });

    m_rightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
    connect(m_rightShortcut, &QShortcut::activated, this, [this]() {
        if (m_videoState) {
            double current = m_videoState->audio_enabled
                ? m_videoState->audioClock : m_videoState->videoClock;
            seekTo(current + 10);
        }
    });

    m_openShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_O), this);
    connect(m_openShortcut, &QShortcut::activated, this, &MainWindow::onAddFilesClicked);

    m_fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(m_fullscreenShortcut, &QShortcut::activated, this, &MainWindow::onToggleFullscreen);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // QSlider 点击直接跳转到点击位置（默认只能拖拽）
    if (obj == m_progressSlider && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            QSlider* slider = m_progressSlider;
            // 计算点击位置对应的值（考虑滑块的样式边距）
            int value = QStyle::sliderValueFromPosition(
                slider->minimum(), slider->maximum(),
                me->pos().x(), slider->width());
            slider->setValue(value);
            // 触发 seek
            double total = get_total_seconds(m_videoState.get());
            if (total > 0) {
                seekTo((value / 1000.0) * total);
            }
            return true;  // 已处理，不再传递
        }
    }
    return QMainWindow::eventFilter(obj, event);
}
