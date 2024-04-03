#include "ddmedia_test/stdafx.h"
#include "ddbase/ddtest_case_factory.h"
#include "ddbase/ddmini_include.h"
#include "ddbase/ddtime.h"
#include "ddbase/ddio.h"
#include "ddbase/ddcolor.h"
#include "ddbase/thread/ddevent.h"
#include "ddbase/thread/ddtask_thread.h"

#include "ddimage/ddimage.h"
#include "ddmedia/ddmedia.h"
#include "ddmedia/decode/dddecode_impl_.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <initguid.h>
}

#pragma comment(lib, "Winmm.lib")
#include <windows.h>

namespace NSP_DD {
//static void emum_hw_device()
//{
//    ddtimer timer;
//    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
//    while (true) {
//        type = av_hwdevice_iterate_types(type);
//        if (type == AV_HWDEVICE_TYPE_NONE) {
//            break;
//        }
//        AVBufferRef* ctx = nullptr;
//        (void)av_hwdevice_ctx_create(&ctx, type, NULL, NULL, 0);
//        if (ctx == nullptr) {
//            continue;
//        }
//        av_buffer_unref(&ctx);
//        const char* type_name = av_hwdevice_get_type_name(type);
//        printf("Hardware acceleration type: %s\n", type_name);
//    }
//    ddcout(ddconsole_color::gray) << ddstr::format(L"create gif cost: %dms\r\n", timer.get_time_pass() / 1000000);
//    timer.reset();
//}

DDTEST(test_case_expend_mp4, 1)
{
    ddtimer timer;
    std::wstring base_path = ddpath::parent(ddstr::ansi_utf16(__FILE__));
    std::wstring full_path = ddpath::join({ base_path, L"test_folder", L"test.mp4" });
    std::wstring image_base_path = ddpath::join({ base_path, L"test_folder", L"expended" });
    if (!dddir::is_path_exist(image_base_path)) {
        dddir::create_dir_ex(image_base_path);
    }

    auto test = ddmedia_decode_impl::create(full_path);
    if (test == nullptr) {
        DDASSERT(false);
    }

    // s32 count = test->frame_count();
  /*  if (!test->seek(10)) {
        DDASSERT(false);
    }*/
    s32 index = 0;
    ddvideo_frame frame_info;
    while (test->get_next_video_frame(frame_info)) {
        frame_info.bitmap.flip_v();
        ddimage::save(frame_info.bitmap, ddpath::join(image_base_path, ddstr::format(L"%d.jpeg", index++)));
    }

    timer.reset();
}

void CALLBACK wave_out_proc(
    HWAVEOUT  hwo,
    UINT      uMsg,
    DWORD_PTR dwInstance,
    DWORD_PTR dwParam1,
    DWORD_PTR dwParam2
);

class ddwave
{
public:
    ~ddwave()
    {
        m_task_thread.stop();
        if (m_hwo != NULL) {
            ::waveOutReset(m_hwo);
            ::waveOutClose(m_hwo);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it : m_items) {
            delete it;
        }
        m_items.clear();
    }

    void on_wave_out()
    {
        m_task_thread.get_task_queue().push_task([this]() {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto item = m_items.front();
            m_items.pop_front();
            (void)::waveOutUnprepareHeader(m_hwo, &item->wh, sizeof(WAVEHDR));
            delete item;
            m_event.notify();
        });
    }

    void wait()
    {
        while (true) {
            if (m_items.size() < m_catch_count) {
                return;
            }

            m_event.wait();
        }
    }

    bool wait_for_end(u64 timeout)
    {
        timeout *= timeout * 1000000;
        ddtimer timer;
        while (true) {
            if (timer.get_time_pass() >= timeout) {
                return false;
            }

            if (m_items.empty()) {
                return true;
            }

            m_event.wait();
        }
    }

    bool output(std::unique_ptr<ddaudio_frame> frame)
    {
        ddtimer timer;
        if (m_hwo == NULL) {
            if (!lazy_open(frame.get())) {
                return false;
            }
        }

        if (m_hwo == NULL) {
            return false;
        }

        std::unique_ptr<ddwave_item> item(new ddwave_item());
        memset(&item->wh, 0, sizeof(WAVEHDR));
        item->wh.lpData = (LPSTR)frame->data.data();
        item->wh.dwBufferLength = (DWORD)frame->data.size();
        item->wh.dwFlags = 0L;
        item->wh.dwLoops = 1L;
        item->frame = std::move(frame);
        if (::waveOutPrepareHeader(m_hwo, &item->wh, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            return false;
        }

        if (::waveOutWrite(m_hwo, &item->wh, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_items.push_back(item.release());
        }
        std::cout << timer.get_time_pass() / 1000000 << std::endl;
        return true;
    }

private:
    bool lazy_open(ddaudio_frame* frame)
    {
        m_task_thread.start();
        DDASSERT(frame != nullptr);
        WAVEFORMATEX wfx = { 0 };
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD)(frame->channel_count);
        wfx.nSamplesPerSec = (DWORD)(frame->sample_rate);
        wfx.wBitsPerSample = (WORD)(frame->bit_depth);
        wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
        if (::waveOutOpen(&m_hwo, WAVE_MAPPER, &wfx, (DWORD_PTR)wave_out_proc, (DWORD_PTR)this, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            return true;
        }

        DWORD volume = (DWORD)(((WORD)10 << 16) | (WORD)10);
        ::waveOutSetVolume(m_hwo, volume);
        return false;
    }

    struct ddwave_item {
        WAVEHDR wh = { 0 };
        std::unique_ptr<ddaudio_frame> frame;
    };
    std::list<ddwave_item*> m_items;
    std::mutex m_mutex;
    ddtask_thread m_task_thread;
    HWAVEOUT m_hwo = NULL;
    ddevent m_event;
    s32 m_catch_count = 3;
};

void CALLBACK wave_out_proc(HWAVEOUT, UINT msg, DWORD_PTR instance, DWORD_PTR, DWORD_PTR)
{
    if (msg == WOM_DONE) {
        ((ddwave*)instance)->on_wave_out();
    }
}

DDTEST(test_case_expend_mp4_audio, 1)
{
    enum_hw();
    ddtimer timer;
    std::wstring base_path = ddpath::parent(ddstr::ansi_utf16(__FILE__));
    std::wstring full_path = ddpath::join({ base_path, L"test_folder", L"test.mp4" });
    std::wstring image_base_path = ddpath::join({ base_path, L"test_folder", L"expended_audio" });
    if (!dddir::is_path_exist(image_base_path)) {
        dddir::create_dir_ex(image_base_path);
    }

    auto test = ddmedia_decode_impl::create(full_path);
    if (test == nullptr) {
        DDASSERT(false);
    }

    std::mutex decode_mutex;
    std::list<ddaudio_frame*> frames;
    s32 max_catch_count = 10;
    ddevent decode_event;
    ddevent decode_event1;
    bool decode_end = false;
    std::thread decode_thread([&]() {
        while (true) {
            s32 size = 0;
            {
                std::lock_guard<std::mutex> lock(decode_mutex);
                size = (s32)frames.size();
            }
            if (size < max_catch_count) {
                std::unique_ptr<ddaudio_frame> frame(new ddaudio_frame());
                if (!test->get_next_audio_frame(*frame)) {
                    decode_end = true;
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(decode_mutex);
                    frames.push_back(frame.release());
                    decode_event1.notify();
                }
            } else {
                decode_event.wait(1000);
            }
        }
    });

    ddwave wave;
    while (true) {
        std::unique_ptr<ddaudio_frame> frame;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(decode_mutex);
                if (!frames.empty()) {
                    frame.reset(frames.front());
                    frames.pop_front();
                    decode_event.notify();
                }
            }

            if (frame == nullptr) {
                {
                    std::lock_guard<std::mutex> lock(decode_mutex);
                    if (decode_end) {
                        break;
                    }
                }
                // wait for the next frame decoded
                decode_event1.wait();
            } else {
                break;
            }
        }

        if (frame == nullptr) {
            break;
        }

        wave.wait();
        if (!wave.output(std::move(frame))) {
            DDASSERT(false);
        }
    }

    wave.wait_for_end(1000);
    decode_thread.join();
    timer.reset();
}
} // namespace NSP_DD
