
#ifndef ddmedia_decode_dddecode_impl__h_
#define ddmedia_decode_dddecode_impl__h_

#include "ddbase/ddmini_include.h"
#include "ddmedia/ddmedia.h"
#include "ddbase/ddbitmap.h"

#include <list>

extern "C" {
#pragma warning(push)
#pragma warning(disable: 4244)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <initguid.h>
#pragma warning(pop)
}

#define DDMEDIA_STREAM_SKIP_VIDEO 1
#define DDMEDIA_STREAM_SKIP_AUDIO 2
#define DDMEDIA_STREAM_SKIP_SUBTITLE 4
namespace NSP_DD {
struct ddvideo_frame
{
    ddbitmap bitmap;
    s64 time_stamp = 0; // 从0开始, ms
    s64 crop_top = 0;
    s64 crop_bottom = 0;
    s64 crop_left = 0;
    s64 crop_right = 0;
};

struct ddaudio_frame
{
    s32 sample_rate = 0;
    s32 bit_depth = 0;
    s32 channel_count = 0;
    s32 sample_count = 0;
    // sample: |ch1, ch2,..., ch(channel_count)|
    // data: |sample1, sample2, ..., sample(sample_count)|
    ddbuff data;
};

struct ddhw_desc
{
    s32 index = 0;
};

void enum_hw();
class ddmedia_decode_impl
{
public:
    ~ddmedia_decode_impl();
    static const std::vector<ddhw_desc> get_support_hw();
    static std::unique_ptr<ddmedia_decode_impl> create(const std::wstring& url, s32 stream_skip = 0);
    bool seek(s64 second);
    s64 frame_count();
    s32 frame_rate();
    s64 time_length(); // 单位毫秒
    AVFrame* get_next_video_frame();
    bool get_next_video_frame(ddvideo_frame& frame);
    AVFrame* get_next_audio_frame();
    bool get_next_audio_frame(ddaudio_frame& frame);

private:
    struct ddmedia_decode_stream
    {
        ~ddmedia_decode_stream();
        bool init(AVFormatContext* formate_ctx, AVMediaType media_type);
        bool decode_pkt(AVPacket* packet);

        AVCodecContext* m_stream_ctx = nullptr;
        s32 m_stream_index = -1;
        AVStream* m_stream = nullptr;
        std::list<AVFrame*> m_frame_catch;
    };

private:
    bool init(const std::wstring& url, s32 stream_skip);
    bool create_format_ctx(const std::wstring& url);
    void process_next_pkt_until_stream(ddmedia_decode_stream* stream);
    AVFrame* get_next_frame(ddmedia_decode_stream* stream);

    AVFormatContext* m_format_ctx = nullptr;
    std::vector<ddmedia_decode_stream*> m_decode_streams;
    // video
    struct SwsContext* m_sws_ctx = nullptr;
    ddmedia_decode_stream* m_video_decode_stream_ref = nullptr;
    // audio
    ddmedia_decode_stream* m_audio_decode_stream_ref = nullptr;
    SwrContext* m_swr_ctx = nullptr;
    // packet
    AVPacket m_packet{ 0 };
};
} // namespace NSP_DD
#endif // ddmedia_decode_dddecode_impl__h_
