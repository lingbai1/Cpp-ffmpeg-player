#include "VideoState.h"

VideoState::VideoState()
        : formatContext(nullptr), audioCodecContext(nullptr), videoCodecContext(nullptr),
          VideoStreamIndex(-1), AudioStreamIndex(-1), videoStream(nullptr), audioStream(nullptr),
          sample_rate(44100), channels(2), channel_layout(AV_CH_LAYOUT_STEREO), sample_fmt(AV_SAMPLE_FMT_S16),
          audioClock(0.0), videoClock(0.0),
          last_width(0), last_height(0), last_pix_fmt(AV_PIX_FMT_NONE),
          window(nullptr), renderer(nullptr), texture(nullptr), sws_ctx(nullptr), swr_ctx(nullptr), audio_dev(0),
          audio_enabled(false), volume(SDL_MIX_MAXVOLUME),
          yuv_frame(nullptr),
          demuxThread(nullptr), videoThread(nullptr), audioThread(nullptr),
          pause(false), quit(false), abort_request(false), eof(false),
          is_network_source(false), network_reconnecting(false), network_failed(false),
          network_error_code(0)
    {
        audio_buffer.buffer = nullptr;
        audio_buffer.size = 0;
        audio_buffer.read_offset = 0;
        audio_buffer.pts_seconds = 0.0;
        audio_buffer.duration = 0;
    }
VideoState::~VideoState()
{
    if (audio_dev != 0) {
            SDL_CloseAudioDevice(audio_dev);
            audio_dev = 0;
        }
        if (videoCodecContext) avcodec_free_context(&videoCodecContext);
        if (audioCodecContext) avcodec_free_context(&audioCodecContext);
        if (formatContext) avformat_close_input(&formatContext);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (swr_ctx) swr_free(&swr_ctx);
        if (audio_buffer.buffer) av_free(audio_buffer.buffer);
        if (yuv_frame) av_frame_free(&yuv_frame);

}
