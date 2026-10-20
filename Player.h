#pragma once

struct VideoState;

/**
 * 全局 SDL 初始化（整个进程生命周期只调用一次）
 * 在 main() 中 QApplication 构造之前调用
 * @return 成功返回 true
 */
bool sdl_global_init();

/**
 * 打开媒体源并启动播放管线
 * 
 * 调用前要求：
 *   is->window 和 is->renderer 必须已由 SdlVideoWidget::attachVideoState() 创建好。
 *   即：先 attachVideoState(is)，再 player_open(is, url)。
 * 
 * 本函数负责：
 *   1. avformat_open_input + avformat_find_stream_info
 *   2. 查找音视频流、打开编解码器
 *   3. 打开 SDL 音频设备
 *   4. 创建 PacketQueue / FrameQueue / PCMQueue
 *   5. 启动 Demux / VideoDecoder / AudioDecoder 三条线程
 * 
 * @return 成功返回 true
 */
bool player_open(VideoState* is, const char* url);

/**
 * 停止播放并清理资源
 * 
 * 本函数负责：
 *   1. 设置 quit 标志，abort 所有队列
 *   2. 等待三条工作线程结束
 *   3. 关闭音频设备、释放编解码器、关闭格式上下文
 *   4. 销毁 SDL window / renderer / texture
 *   5. 释放 sws_ctx / swr_ctx 等
 * 
 * 调用后 VideoState 对象可安全 delete 或 reset()
 */
void player_close(VideoState* is);

/**
 * 渲染一帧视频
 * 由 SdlVideoWidget 的 QTimer 定期调用（~60fps）
 */
void render_frame(VideoState* is);

/**
 * Seek 到指定位置（秒）
 */
void seek(VideoState* is, double pos_seconds);

/**
 * 获取媒体总时长（秒），失败返回 0.0
 */
double get_total_seconds(VideoState* is);

/** 设置音量，范围为 0-100。 */
void set_volume(VideoState* is, int volume_percent);
