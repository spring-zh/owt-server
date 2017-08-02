/*
 * Copyright 2017 Intel Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to the
 * source code ("Material") are owned by Intel Corporation or its suppliers or
 * licensors. Title to the Material remains with Intel Corporation or its suppliers
 * and licensors. The Material contains trade secrets and proprietary and
 * confidential information of Intel or its suppliers and licensors. The Material
 * is protected by worldwide copyright and trade secret laws and treaty provisions.
 * No part of the Material may be used, copied, reproduced, modified, published,
 * uploaded, posted, transmitted, distributed, or disclosed in any way without
 * Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery of
 * the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be express
 * and approved by Intel in writing.
 */

#include "MediaFileOut.h"

#define KEYFRAME_REQ_INTERVAL (6 * 1000) // 6 seconds

namespace woogeen_base {

DEFINE_LOGGER(MediaFileOut, "woogeen.media.MediaFileOut");

inline AVCodecID frameFormat2VideoCodecID(int frameFormat)
{
    switch (frameFormat) {
        case FRAME_FORMAT_VP8:
            return AV_CODEC_ID_VP8;
        case FRAME_FORMAT_H264:
            return AV_CODEC_ID_H264;
        default:
            return AV_CODEC_ID_VP8;
    }
}

inline AVCodecID frameFormat2AudioCodecID(int frameFormat)
{
    switch (frameFormat) {
        case FRAME_FORMAT_PCMU:
            return AV_CODEC_ID_PCM_MULAW;
        case FRAME_FORMAT_PCMA:
            return AV_CODEC_ID_PCM_ALAW;
        case FRAME_FORMAT_OPUS:
            return AV_CODEC_ID_OPUS;
        case FRAME_FORMAT_AAC:
        case FRAME_FORMAT_AAC_48000_2:
            return AV_CODEC_ID_AAC;
        default:
            return AV_CODEC_ID_PCM_MULAW;
    }
}

MediaFileOut::MediaFileOut(const std::string& url, bool hasAudio, bool hasVideo, EventRegistry* handle)
    : m_uri{ url }
    , m_hasAudio(hasAudio)
    , m_hasVideo(hasVideo)
    , m_audioFormat(FRAME_FORMAT_UNKNOWN)
    , m_sampleRate(0)
    , m_channels(0)
    , m_videoFormat(FRAME_FORMAT_UNKNOWN)
    , m_width(0)
    , m_height(0)
    , m_context(nullptr)
    , m_audioStream(nullptr)
    , m_videoStream(nullptr)
    , m_videoSourceChanged(true)
{
    if (!m_hasAudio && !m_hasVideo) {
        ELOG_ERROR("Audio/Video not enabled");
        notifyAsyncEvent("init", "Audio/Video not enabled");
        return;
    }

    memset(&m_videoKeyFrame, 0, sizeof(m_videoKeyFrame));

    m_videoQueue.reset(new MediaFrameQueue());
    m_audioQueue.reset(new MediaFrameQueue());
    setEventRegistry(handle);

    if (ELOG_IS_TRACE_ENABLED())
        av_log_set_level(AV_LOG_DEBUG);
    else if (ELOG_IS_DEBUG_ENABLED())
        av_log_set_level(AV_LOG_INFO);
    else
        av_log_set_level(AV_LOG_WARNING);

    m_context = avformat_alloc_context();
    if (!m_context) {
        m_status = AVStreamOut::Context_CLOSED;
        notifyAsyncEvent("init", "cannot allocate context");
        ELOG_ERROR("avformat_alloc_context failed");
        return;
    }
    m_context->oformat = av_guess_format(nullptr, url.c_str(), nullptr);
    if (!m_context->oformat) {
        m_status = AVStreamOut::Context_CLOSED;
        notifyAsyncEvent("init", "cannot find proper output format");
        ELOG_ERROR("av_guess_format failed");
        avformat_free_context(m_context);
        return;
    }
    av_strlcpy(m_context->filename, url.c_str(), sizeof(m_context->filename));

    m_status = AVStreamOut::Context_INITIALIZING;
    notifyAsyncEvent("init", "");

    m_jobTimer.reset(new JobTimer(100, this));
}

MediaFileOut::~MediaFileOut()
{
    close();
}

void MediaFileOut::close()
{
    if (m_status == AVStreamOut::Context_CLOSED)
        return;
    if (m_jobTimer)
        m_jobTimer->stop();
    if (m_status == AVStreamOut::Context_READY)
        av_write_trailer(m_context);
    if (m_context) {
        if (m_context->pb && !(m_context->oformat->flags & AVFMT_NOFILE))
            avio_close(m_context->pb);
        avformat_free_context(m_context);
        m_context = nullptr;
    }
    if (m_videoKeyFrame.payload) {
        free(m_videoKeyFrame.payload);
        m_videoKeyFrame.payload = NULL;
        m_videoKeyFrame.length = 0;
    }
    m_status = AVStreamOut::Context_CLOSED;
    ELOG_DEBUG("closed");
}

void MediaFileOut::onFrame(const Frame& frame)
{
    if (m_status == AVStreamOut::Context_EMPTY || m_status == AVStreamOut::Context_CLOSED) {
        return;
    }

    switch (frame.format) {
    case FRAME_FORMAT_VP8:
    case FRAME_FORMAT_H264:
        if (!m_hasVideo) {
            ELOG_WARN("Video is not enabled");
            return;
        }

        if (m_videoSourceChanged) {
            if (frame.additionalInfo.video.isKeyFrame) {
                ELOG_DEBUG("key frame comes after video source changed!");
                m_videoSourceChanged = false;

                if (m_videoKeyFrame.payload) {
                    free(m_videoKeyFrame.payload);
                    m_videoKeyFrame.payload = NULL;
                    m_videoKeyFrame.length = 0;
                }
                m_videoKeyFrame.payload = (uint8_t *)malloc(frame.length);
                m_videoKeyFrame.length = frame.length;
                memcpy(m_videoKeyFrame.payload, frame.payload, frame.length);
            } else {
                ELOG_DEBUG("request key frame after video source changed!");
                deliverFeedbackMsg(FeedbackMsg{.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME});
                return;
            }
        }

        if (!m_videoStream) {
            if (!addVideoStream(frame.format, frame.additionalInfo.video.width, frame.additionalInfo.video.height)) {
                ELOG_ERROR("Can not add video stream");
                notifyAsyncEvent("fatal", "Can not add video stream");
                return close();
            }

            m_videoFormat   = frame.format;
            m_width         = frame.additionalInfo.video.width;
            m_height        = frame.additionalInfo.video.height;
        }

        if (frame.additionalInfo.video.width != m_width || frame.additionalInfo.video.height != m_height) {
            ELOG_DEBUG("video resolution changed: %dx%d -> %dx%d"
                    , m_width, m_height
                    , frame.additionalInfo.video.width, frame.additionalInfo.video.height
                    );

            m_width = frame.additionalInfo.video.width;
            m_height = frame.additionalInfo.video.height;
        }

        if (m_status == AVStreamOut::Context_READY) {
            if (!m_videoSourceChanged) {
                m_videoQueue->pushFrame(frame.payload, frame.length);
            } else {
                ELOG_DEBUG("video source changed, discard till key frame!");
            }
        }
        break;
    case FRAME_FORMAT_PCMU:
    case FRAME_FORMAT_PCMA:
    case FRAME_FORMAT_AAC:
    case FRAME_FORMAT_AAC_48000_2:
    case FRAME_FORMAT_OPUS:
        if (!m_hasAudio) {
            ELOG_WARN("Audio is not enabled");
            return;
        }

        if (!m_audioStream) {
            if (!addAudioStream(frame.format, frame.additionalInfo.audio.sampleRate, frame.additionalInfo.audio.channels)) {
                ELOG_ERROR("Can not add audio stream");
                notifyAsyncEvent("fatal", "Can not add audio stream");
                return close();
            }

            m_audioFormat = frame.format;
            m_sampleRate = frame.additionalInfo.audio.sampleRate;
            m_channels = frame.additionalInfo.audio.channels;
        }

        if (m_sampleRate != frame.additionalInfo.audio.sampleRate || m_channels != frame.additionalInfo.audio.channels) {
            ELOG_ERROR("invalid audio frame channels %d, or sample rate: %d", frame.additionalInfo.audio.channels, frame.additionalInfo.audio.sampleRate);
            notifyAsyncEvent("fatal", "invalid audio frame channels or sample rate");
            return close();
        }

        if (m_status == AVStreamOut::Context_READY) {
            uint8_t* payload = frame.payload;
            uint32_t length = frame.length;
            if (frame.additionalInfo.audio.isRtpPacket) {
                RTPHeader* rtp = reinterpret_cast<RTPHeader*>(payload);
                uint32_t headerLength = rtp->getHeaderLength();
                assert(length >= headerLength);
                payload += headerLength;
                length -= headerLength;
            }
            m_audioQueue->pushFrame(payload, length);
        }
        break;
    default:
        ELOG_ERROR("unsupported frame format: %d", frame.format);
        notifyAsyncEvent("fatal", "unsupported frame format");
        return close();
    }
}

void MediaFileOut::onVideoSourceChanged()
{
    ELOG_DEBUG("onVideoSourceChanged");

    deliverFeedbackMsg(FeedbackMsg{.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME});
    m_videoSourceChanged = true;
}

bool MediaFileOut::addAudioStream(FrameFormat format, uint32_t sampleRate, uint32_t channels)
{
    AVStream* stream = avformat_new_stream(m_context, nullptr);
    if (!stream) {
        ELOG_ERROR("cannot add audio stream");
        notifyAsyncEvent("fatal", "cannot add audio stream");
        close();
        return false;
    }

    AVCodecParameters *par = stream->codecpar;
    par->codec_type     = AVMEDIA_TYPE_AUDIO;
    par->codec_id       = frameFormat2AudioCodecID(format);
    par->sample_rate    = sampleRate;
    par->channels       = channels;
    par->channel_layout = av_get_default_channel_layout(par->channels);
    switch(par->codec_id) {
        case AV_CODEC_ID_AAC: //AudioSpecificConfig 48000-2
            par->extradata_size = 2;
            par->extradata      = (uint8_t *)av_malloc(par->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            par->extradata[0]   = 0x11;
            par->extradata[1]   = 0x90;
            break;
        case AV_CODEC_ID_OPUS: //OpusHead 48000-2
            par->extradata_size = 19;
            par->extradata      = (uint8_t *)av_malloc(par->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            par->extradata[0]   = 'O';
            par->extradata[1]   = 'p';
            par->extradata[2]   = 'u';
            par->extradata[3]   = 's';
            par->extradata[4]   = 'H';
            par->extradata[5]   = 'e';
            par->extradata[6]   = 'a';
            par->extradata[7]   = 'd';
            //Version
            par->extradata[8]   = 1;
            //Channel Count
            par->extradata[9]   = 2;
            //Pre-skip
            par->extradata[10]  = 0x38;
            par->extradata[11]  = 0x1;
            //Input Sample Rate (Hz)
            par->extradata[12]  = 0x80;
            par->extradata[13]  = 0xbb;
            par->extradata[14]  = 0;
            par->extradata[15]  = 0;
            //Output Gain (Q7.8 in dB)
            par->extradata[16]  = 0;
            par->extradata[17]  = 0;
            //Mapping Family
            par->extradata[18]  = 0;
            break;
        default:
            break;
    }

    m_audioStream = stream;

    if (!m_hasVideo || m_videoStream) {
        return getReady();
    }
    return true;
}

bool MediaFileOut::addVideoStream(FrameFormat format, uint32_t width, uint32_t height)
{
    AVStream* stream = avformat_new_stream(m_context, nullptr);
    if (!stream) {
        notifyAsyncEvent("fatal", "cannot add audio stream");
        close();
        ELOG_ERROR("cannot add video stream");
        return false;
    }

    AVCodecParameters *par = stream->codecpar;
    par->codec_type     = AVMEDIA_TYPE_VIDEO;
    par->codec_id       = frameFormat2VideoCodecID(format);
    par->width          = width;
    par->height         = height;
    if (par->codec_id == AV_CODEC_ID_H264) { //extradata
        AVCodecParserContext *parser = av_parser_init(par->codec_id);
        if (!parser) {
            ELOG_ERROR("Cannot find video parser");
            return false;
        }

        int size = parser->parser->split(NULL, m_videoKeyFrame.payload, m_videoKeyFrame.length);
        if (size > 0) {
            par->extradata_size = size;
            par->extradata      = (uint8_t *)av_malloc(par->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(par->extradata, m_videoKeyFrame.payload, par->extradata_size);
        } else {
            ELOG_WARN("Cannot find video extradata");
        }

        av_parser_close(parser);
    }

    m_videoStream = stream;

    if (!m_hasAudio || m_audioStream) {
        return getReady();
    }
    return true;
}

bool MediaFileOut::getReady()
{
    if (!(m_context->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_context->pb, m_context->filename, AVIO_FLAG_WRITE) < 0) {
            notifyAsyncEvent("init", "output file does not exist or cannot be opened for write");
            ELOG_ERROR("avio_open failed");
            return false;
        }
    }
    if (avformat_write_header(m_context, nullptr) < 0) {
        notifyAsyncEvent("init", "cannot write file header");
        ELOG_ERROR("avformat_write_header failed");
        return false;
    }
    av_dump_format(m_context, 0, m_context->filename, 1);
    m_status = AVStreamOut::Context_READY;
    ELOG_DEBUG("context ready");
    return true;
}

void MediaFileOut::onTimeout()
{
    if (m_status != AVStreamOut::Context_READY)
        return;

    boost::shared_ptr<EncodedFrame> mediaFrame;

    while (mediaFrame = m_audioQueue->popFrame())
        this->writeAVFrame(m_audioStream, *mediaFrame, false);
    while (mediaFrame = m_videoQueue->popFrame())
        this->writeAVFrame(m_videoStream, *mediaFrame, true);
}

int MediaFileOut::writeAVFrame(AVStream* stream, const EncodedFrame& frame, bool isVideo)
{
    AVPacket pkt;
    int ret;

    av_init_packet(&pkt);
    pkt.data = frame.m_payloadData;
    pkt.size = frame.m_payloadSize;
    pkt.pts = (int64_t)(frame.m_timeStamp / (av_q2d(stream->time_base) * 1000));
    pkt.dts = pkt.pts;
    pkt.stream_index = stream->index;

    if (isVideo) {
        if (stream->codecpar->codec_id == AV_CODEC_ID_H264)
            pkt.flags = isH264KeyFrame(frame.m_payloadData, frame.m_payloadSize) ? AV_PKT_FLAG_KEY : 0;
        else
            pkt.flags = isVp8KeyFrame(frame.m_payloadData, frame.m_payloadSize) ? AV_PKT_FLAG_KEY : 0;

        if (pkt.flags == AV_PKT_FLAG_KEY) {
            m_lastKeyFrameReqTime = frame.m_timeStamp;
        }

        if (frame.m_timeStamp - m_lastKeyFrameReqTime > KEYFRAME_REQ_INTERVAL) {
            m_lastKeyFrameReqTime = frame.m_timeStamp;

            ELOG_DEBUG("Request video key frame");
            deliverFeedbackMsg(FeedbackMsg{.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME});
        }
    }

    ret = av_interleaved_write_frame(m_context, &pkt);
    if (ret < 0)
        ELOG_ERROR("Cannot write frame, %s", ff_err2str(ret));

    return ret;
}

char *MediaFileOut::ff_err2str(int errRet)
{
    av_strerror(errRet, (char*)(&m_errbuff), 500);
    return m_errbuff;
}

} // namespace woogeen_base
