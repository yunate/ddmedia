#include "ddmedia/stdafx.h"
#include "ddmedia/decode/dddecode_impl_.h"

#include <memory>

namespace NSP_DD {
void enum_hw()
{
    std::vector<AVHWDeviceType> types;
    AVHWDeviceType pre_type = AV_HWDEVICE_TYPE_NONE;
    while (true) {
        pre_type = av_hwdevice_iterate_types(pre_type);
        if (pre_type == AV_HWDEVICE_TYPE_NONE) {
            break;
        }
        types.push_back(pre_type);
    }
}

static void catch_frame(std::list<AVFrame*>& frame_catch, AVFrame* frame, s32 max_size = 24)
{
    if (frame_catch.size() >= max_size) {
        AVFrame* it = frame_catch.front();
        av_frame_free(&it);
        frame_catch.pop_front();
    }
    frame_catch.push_back(frame);
}

ddmedia_decode_impl::ddmedia_decode_stream::~ddmedia_decode_stream()
{
    if (m_stream_ctx != nullptr) {
        avcodec_free_context(&m_stream_ctx);
    }
    if (!m_frame_catch.empty()) {
        for (auto* it : m_frame_catch) {
            av_frame_free(&it);
        }
        m_frame_catch.clear();
    }
}

bool ddmedia_decode_impl::ddmedia_decode_stream::init(AVFormatContext* formate_ctx, AVMediaType media_type)
{
    DDASSERT(formate_ctx != nullptr);
    m_stream_index = -1;
    for (u32 i = 0; i < formate_ctx->nb_streams; i++) {
        if (formate_ctx->streams[i]->codecpar->codec_type == media_type) {
            m_stream_index = i;
            m_stream = formate_ctx->streams[i];
            break;
        }
    }

    if (m_stream_index == -1) {
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
    if (codec == nullptr) {
        return false;
    }

    m_stream_ctx = avcodec_alloc_context3(codec);
    if (m_stream_ctx == nullptr) {
        return false;
    }

    if (avcodec_parameters_to_context(m_stream_ctx, m_stream->codecpar) < 0) {
        return false;
    }

    if (avcodec_open2(m_stream_ctx, codec, NULL) < 0) {
        return false;
    }

    // 使用硬件加速
    (void)av_hwdevice_ctx_create(&m_stream_ctx->hw_device_ctx, AV_HWDEVICE_TYPE_QSV, NULL, NULL, 0);
    (void)av_hwdevice_ctx_create(&m_stream_ctx->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
    (void)av_hwdevice_ctx_create(&m_stream_ctx->hw_device_ctx, AV_HWDEVICE_TYPE_DXVA2, NULL, NULL, 0);
    (void)av_hwdevice_ctx_create(&m_stream_ctx->hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0);
    if (m_stream_ctx->hw_device_ctx == nullptr) {
        return false;
    }
    return true;
}

bool ddmedia_decode_impl::ddmedia_decode_stream::decode_pkt(AVPacket* packet)
{
    while (true) {
        int result = avcodec_send_packet(m_stream_ctx, packet);
        // if result equal AVERROR(EAGAIN), it call avcodec_receive_frame first andthen call avcodec_send_packet again.
        if (result != 0 && result != AVERROR(EAGAIN)) {
            return false;
        }

        while (true) {
            AVFrame* frame = av_frame_alloc();
            if (frame == nullptr) {
                return false;
            }

            int receive_result = avcodec_receive_frame(m_stream_ctx, frame);
            if (receive_result != 0) {
                av_frame_free(&frame);
                if (receive_result == AVERROR_EOF || receive_result == AVERROR(EAGAIN)) {
                    // need send more pkt or eof
                    break;
                }
                return false;
            }

            if (frame->hw_frames_ctx == nullptr) {
                catch_frame(m_frame_catch, frame);
            } else {
                AVFrame* sw_frame = av_frame_alloc();
                if (sw_frame != nullptr) {
                    if (av_hwframe_transfer_data(sw_frame, frame, 0) == 0) {
                        (void)av_frame_copy_props(sw_frame, frame);
                        catch_frame(m_frame_catch, sw_frame);
                    } else {
                        av_frame_free(&sw_frame);
                    }
                }
                av_frame_free(&frame);
            }
        }

        // if result equal AVERROR(EAGAIN), it call avcodec_receive_frame first andthen call avcodec_send_packet again.
        if (result != AVERROR(EAGAIN)) {
            return true;
        }
    }
}

ddmedia_decode_impl::~ddmedia_decode_impl()
{
    for (auto it : m_decode_streams) {
        delete it;
        m_decode_streams.clear();
    }

    if (m_sws_ctx != nullptr) {
        sws_freeContext(m_sws_ctx);
    }

    if (m_swr_ctx != nullptr) {
        swr_free(&m_swr_ctx);
    }

    if (m_packet.buf != nullptr) {
        av_packet_unref(&m_packet);
    }

    if (m_format_ctx != nullptr) {
        avformat_close_input(&m_format_ctx);
    }
}

std::unique_ptr<ddmedia_decode_impl> ddmedia_decode_impl::create(const std::wstring& url, s32 stream_skip)
{
    std::unique_ptr<ddmedia_decode_impl> inst(new(std::nothrow)ddmedia_decode_impl());
    if (inst == nullptr) {
        return nullptr;
    }

    if (!inst->init(url, stream_skip)) {
        return nullptr;
    }

    return std::move(inst);
}
bool ddmedia_decode_impl::seek(s64 second)
{
    if (m_decode_streams.empty()) {
        return false;
    }
    ddmedia_decode_stream* stream = m_decode_streams[0];
    s32 index = 0;
    AVRational time_base = stream->m_stream->time_base;
    s64 target_time_stamp = av_rescale(second, time_base.den, time_base.num);
    if (av_seek_frame(m_format_ctx, index, target_time_stamp, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    // all catchs are invalid.
    for (auto it : m_decode_streams) {
        for (AVFrame* frame : it->m_frame_catch) {
            av_frame_free(&frame);
        }
        it->m_frame_catch.clear();
        avcodec_flush_buffers(it->m_stream_ctx);
    }

    s32 pt = 1000 / frame_rate();
    while (true) {
        AVFrame* frame = get_next_frame(stream);
        if (frame == nullptr) {
            return false;
        }
        s64 time_stamp = (s64)(frame->pkt_dts * 1000 * av_q2d(stream->m_stream->time_base));
        av_frame_free(&frame);
        if (time_stamp + pt >= second * 1000) {
            return true;
        }
    }

    return true;
}
s64 ddmedia_decode_impl::frame_count()
{
    if (m_decode_streams.empty()) {
        return 0;
    }
    return m_decode_streams[0]->m_stream->nb_frames;
}
s32 ddmedia_decode_impl::frame_rate()
{
    if (m_decode_streams.empty()) {
        return 24;
    }
    AVRational r = m_decode_streams[0]->m_stream->r_frame_rate;
    return (s32)((r.num + r.den - 1) / r.den);
}
s64 ddmedia_decode_impl::time_length()
{
    return frame_count() * 1000 / frame_rate();
}

void ddmedia_decode_impl::process_next_pkt_until_stream(ddmedia_decode_stream* stream)
{
    DDASSERT(stream != nullptr);
    while (true) {
        if (av_read_frame(m_format_ctx, &m_packet) < 0) {
            return;
        }

        bool decode_result = true;
        s32 index = (s32)m_packet.stream_index;
        for (auto* it : m_decode_streams) {
            if (it->m_stream_index == index) {
                decode_result = it->decode_pkt(&m_packet);
                break;
            }
        }

        av_packet_unref(&m_packet);
        if (!decode_result) {
            return;
        }

        // if current pkt is a B frame, it need the next pkt, so check if the catch is not empty.
        if (index == stream->m_stream_index && !stream->m_frame_catch.empty()) {
            break;
        }
    }
}

AVFrame* ddmedia_decode_impl::get_next_frame(ddmedia_decode_stream* stream)
{
    if (stream == nullptr) {
        return nullptr;
    }

    if (stream->m_frame_catch.empty()) {
        process_next_pkt_until_stream(stream);
    }

    if (!stream->m_frame_catch.empty()) {
        AVFrame* frame = stream->m_frame_catch.front();
        stream->m_frame_catch.pop_front();
        return frame;
    }
    return nullptr;
}

AVFrame* ddmedia_decode_impl::get_next_video_frame()
{
    return get_next_frame(m_video_decode_stream_ref);
}

AVFrame* ddmedia_decode_impl::get_next_audio_frame()
{
    return get_next_frame(m_audio_decode_stream_ref);
}

bool ddmedia_decode_impl::get_next_video_frame(ddvideo_frame& frame)
{
    AVFrame* video_frame = get_next_video_frame();
    if (video_frame == nullptr) {
        return false;
    }

    DDASSERT(m_video_decode_stream_ref != nullptr);
    AVCodecContext* stream_ctx = m_video_decode_stream_ref->m_stream_ctx;
    frame.time_stamp = (s64)(video_frame->pkt_dts * 1000 * av_q2d(m_video_decode_stream_ref->m_stream->time_base));
    frame.bitmap.resize(stream_ctx->width, stream_ctx->height);
    uint8_t* color_data[AV_NUM_DATA_POINTERS] = { (u8*)frame.bitmap.colors.data() };
    int color_lines[AV_NUM_DATA_POINTERS] = { stream_ctx->width * 4 };

    if (m_sws_ctx == nullptr) {
        m_sws_ctx = sws_getContext(
            stream_ctx->width, stream_ctx->height, (AVPixelFormat)video_frame->format,
            stream_ctx->width, stream_ctx->height, AV_PIX_FMT_RGB32, SWS_BILINEAR,
            NULL, NULL, NULL);
    }

    sws_scale(m_sws_ctx, (uint8_t const* const*)video_frame->data, video_frame->linesize, 0, stream_ctx->height, color_data, color_lines);
    av_frame_free(&video_frame);
    return true;
}

bool ddmedia_decode_impl::get_next_audio_frame(ddaudio_frame& frame)
{
    AVFrame* audio_frame = get_next_audio_frame();
    if (audio_frame == nullptr) {
        return false;
    }

    ddexec_guard guard([&audio_frame]() {
        av_frame_free(&audio_frame);
    });

    s32 sample_rate = (s32)audio_frame->sample_rate;
    frame.bit_depth = 16;
    frame.sample_rate = sample_rate > 44100 ? 44100 : sample_rate;
    frame.channel_count = s32(m_audio_decode_stream_ref->m_stream_ctx->ch_layout.nb_channels);

    DDASSERT(m_audio_decode_stream_ref != nullptr);
    if (m_swr_ctx == nullptr) {
        m_swr_ctx = swr_alloc();
        if (!m_swr_ctx) {
            return false;
        }

        int rtn = av_opt_set_chlayout(m_swr_ctx, "in_chlayout", &m_audio_decode_stream_ref->m_stream_ctx->ch_layout, 0);
        rtn = av_opt_set_int(m_swr_ctx, "in_sample_rate", sample_rate, 0);
        rtn = av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt", m_audio_decode_stream_ref->m_stream_ctx->sample_fmt, 0);

        rtn = av_opt_set_int(m_swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        rtn = av_opt_set_int(m_swr_ctx, "out_sample_rate", frame.sample_rate, 0);
        rtn = av_opt_set_sample_fmt(m_swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

        if (swr_init(m_swr_ctx) < 0) {
            swr_free(&m_swr_ctx);
            return false;
        }
    }

    int out_len = swr_get_out_samples(m_swr_ctx, audio_frame->nb_samples);
    frame.data.resize(out_len * frame.bit_depth / 8 * frame.channel_count);
    u8* buff = frame.data.data();
    frame.sample_count = swr_convert(m_swr_ctx, &buff, out_len, (const uint8_t**)audio_frame->data, audio_frame->nb_samples);
    frame.data.resize(frame.sample_count * frame.bit_depth / 8 * frame.channel_count);
    return true;
}

bool ddmedia_decode_impl::init(const std::wstring& url, s32 stream_skip)
{
    if (!create_format_ctx(url)) {
        return false;
    }

    if (!(stream_skip & DDMEDIA_STREAM_SKIP_VIDEO)) {
        m_video_decode_stream_ref = new (std::nothrow)ddmedia_decode_stream();
        if (m_video_decode_stream_ref != nullptr) {
            if (!m_video_decode_stream_ref->init(m_format_ctx, AVMEDIA_TYPE_VIDEO)) {
                delete m_video_decode_stream_ref;
            } else {
                m_decode_streams.push_back(m_video_decode_stream_ref);
            }
        }
    }

    if (!(stream_skip & DDMEDIA_STREAM_SKIP_AUDIO)) {
        m_audio_decode_stream_ref = new (std::nothrow)ddmedia_decode_stream();
        if (m_audio_decode_stream_ref != nullptr) {
            if (!m_audio_decode_stream_ref->init(m_format_ctx, AVMEDIA_TYPE_AUDIO)) {
                delete m_audio_decode_stream_ref;
            } else {
                m_decode_streams.push_back(m_audio_decode_stream_ref);
            }
        }
    }

    if (m_decode_streams.empty()) {
        return false;
    }

    return true;
}

bool ddmedia_decode_impl::create_format_ctx(const std::wstring& url)
{
    if (avformat_open_input(&m_format_ctx, ddstr::utf16_ansi(url).c_str(), NULL, NULL) != 0) {
        return false;
    }

    if (avformat_find_stream_info(m_format_ctx, NULL) < 0) {
        return false;
    }
    return true;
}
} // namespace NSP_DD
