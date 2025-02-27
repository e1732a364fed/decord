//
// Created by Yin, Weisu on 1/8/21.
//

#include "audio_reader.h"
#include "../runtime/str_util.h"
#include <memory>
#include <cmath>
#include <libavutil/channel_layout.h>  // Para AVChannelLayout y constantes relacionadas

namespace decord {
// AVIO buffer size cuando se lee desde bytes raw
static const int AVIO_BUFFER_SIZE = std::stoi(runtime::GetEnvironmentVariableOrDefault("DECORD_AVIO_BUFFER_SIZE", "40960"));

AudioReader::AudioReader(std::string fn, int sampleRate, DLContext ctx, int io_type, bool mono)
    : ctx(ctx), io_ctx_(), pFormatContext(nullptr), swr(nullptr), pCodecParameters(nullptr),
      pCodecContext(nullptr), audioStreamIndex(-1), outputVector(), output(), padding(-1.0), filename(fn),
      originalSampleRate(0), targetSampleRate(sampleRate), numChannels(0), mono(mono), totalSamplesPerChannel(0),
      totalConvertedSamplesPerChannel(0), timeBase(0.0), duration(0.0) {
    // av_register_all está obsoleto en versiones recientes
#if (LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100))
    av_register_all();
#endif

    if (Decode(fn, io_type) == -1) {
        avformat_close_input(&pFormatContext);
        return;
    }
    avformat_close_input(&pFormatContext);
    // Calcular duración precisa
    duration = totalSamplesPerChannel / static_cast<double>(originalSampleRate);
    // Construir NDArray
    ToNDArray();
}

AudioReader::~AudioReader() {}

NDArray AudioReader::GetNDArray() {
    return output;
}

int AudioReader::GetNumPaddingSamples() {
    return std::ceil(padding * targetSampleRate);
}

double AudioReader::GetDuration() {
    return duration;
}

int64_t AudioReader::GetNumSamplesPerChannel() {
    if (outputVector.size() <= 0) return 0;
    return outputVector[0].size();
}

int AudioReader::GetNumChannels() {
    return numChannels;
}

void AudioReader::GetInfo() {
    std::cout << "audio stream [" << audioStreamIndex << "]: " << std::endl
              << "start time: " << std::endl
              << padding << std::endl
              << "duration: " << std::endl
              << duration << std::endl
              << "original sample rate: " << std::endl
              << originalSampleRate << std::endl
              << "target sample rate: " << std::endl
              << targetSampleRate << std::endl
              << "number of channels: " << std::endl
              << numChannels << std::endl
              << "total original samples per channel: " << std::endl
              << totalSamplesPerChannel << std::endl
              << "total original samples: " << std::endl
              << totalSamplesPerChannel * numChannels << std::endl
              << "total resampled samples per channel: " << std::endl
              << totalConvertedSamplesPerChannel << std::endl
              << "total resampled samples: " << std::endl
              << totalConvertedSamplesPerChannel * numChannels << std::endl;
}

int AudioReader::Decode(std::string fn, int io_type) {
    pFormatContext = avformat_alloc_context();
    CHECK(pFormatContext != nullptr) << "Unable to alloc avformat context";

    int formatOpenRet = 1;
    if (io_type == kDevice) {
        LOG(FATAL) << "Not implemented";
        return -1;
    } else if (io_type == kRawBytes) {
        filename = "BytesIO";
        io_ctx_.reset(new ffmpeg::AVIOBytesContext(fn, AVIO_BUFFER_SIZE));
        pFormatContext->pb = io_ctx_->get_avio();
        if (!pFormatContext->pb) {
            LOG(FATAL) << "Unable to init AVIO from memory buffer";
            return -1;
        }
        formatOpenRet = avformat_open_input(&pFormatContext, NULL, NULL, NULL);
    } else if (io_type == kNormal) {
        formatOpenRet = avformat_open_input(&pFormatContext, fn.c_str(), NULL, NULL);
    } else {
        LOG(FATAL) << "Invalid io type: " << io_type;
        return -1;
    }

    if (formatOpenRet != 0) {
        char errstr[200];
        av_strerror(formatOpenRet, errstr, 200);
        if (io_type != kBytes) {
            LOG(FATAL) << "ERROR opening: " << fn.c_str() << ", " << errstr;
        } else {
            LOG(FATAL) << "ERROR opening " << fn.size() << " bytes, " << errstr;
        }
        return -1;
    }

    avformat_find_stream_info(pFormatContext, NULL);

    for (auto i = 0; i < int(pFormatContext->nb_streams); i++) {
        AVCodecParameters *tempCodecParameters = pFormatContext->streams[i]->codecpar;
        if (tempCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            timeBase = (double)pFormatContext->streams[i]->time_base.num / (double)pFormatContext->streams[i]->time_base.den;
            duration = (double)pFormatContext->streams[i]->duration * timeBase;
            pCodecParameters = tempCodecParameters;
            originalSampleRate = tempCodecParameters->sample_rate;
            if (targetSampleRate == -1) targetSampleRate = originalSampleRate;
            numChannels = tempCodecParameters->ch_layout.nb_channels;  // Usar ch_layout en lugar de channels
            break;
        }
    }

    if (audioStreamIndex == -1) {
        LOG(FATAL) << "Can't find audio stream";
        return -1;
    }

    auto pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    CHECK(pCodec != nullptr) << "ERROR Decoder not found. The codec is not supported.";
    pCodecContext = avcodec_alloc_context3(pCodec);
    CHECK(pCodecContext != nullptr) << "ERROR Could not allocate a decoding context.";
    CHECK_GE(avcodec_parameters_to_context(pCodecContext, pCodecParameters), 0) << "ERROR Could not set context parameters.";
    int codecOpenRet = avcodec_open2(pCodecContext, pCodec, NULL);
    if (codecOpenRet < 0) {
        char errstr[200];
        av_strerror(codecOpenRet, errstr, 200);
        avcodec_close(pCodecContext);
        avcodec_free_context(&pCodecContext);
        avformat_close_input(&pFormatContext);
        LOG(FATAL) << "ERROR open codec through avcodec_open2: " << errstr;
        return -1;
    }

    pCodecContext->pkt_timebase = pFormatContext->streams[audioStreamIndex]->time_base;

    AVPacket *pPacket = av_packet_alloc();
    AVFrame *pFrame = av_frame_alloc();
    DecodePacket(pPacket, pCodecContext, pFrame, audioStreamIndex);

    return 0;
}

void AudioReader::DecodePacket(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame, int streamIndex) {
    InitSWR(pCodecContext);

    int pktRet = -1;
    while ((pktRet = av_read_frame(pFormatContext, pPacket)) != AVERROR_EOF) {
        if (pktRet != 0) {
            LOG(WARNING) << "ERROR Fail to get packet.";
            break;
        }
        if (pPacket->stream_index != streamIndex) {
            av_packet_unref(pPacket);
            continue;
        }
        int sendRet = avcodec_send_packet(pCodecContext, pPacket);
        if (sendRet != 0) {
            if (sendRet != AVERROR(EAGAIN)) {
                LOG(WARNING) << "ERROR Fail to send packet.";
                av_packet_unref(pPacket);
                break;
            }
        }
        av_packet_unref(pPacket);
        int receiveRet = -1;
        while ((receiveRet = avcodec_receive_frame(pCodecContext, pFrame)) == 0) {
            totalSamplesPerChannel += pFrame->nb_samples;
            HandleFrame(pCodecContext, pFrame);
        }
        if (receiveRet != AVERROR(EAGAIN)) {
            LOG(WARNING) << "ERROR Fail to receive frame.";
            break;
        }
    }
    DrainDecoder(pCodecContext, pFrame);
    av_frame_free(&pFrame);
    av_packet_free(&pPacket);
    avcodec_close(pCodecContext);
    swr_close(swr);
    swr_free(&swr);
    avcodec_free_context(&pCodecContext);
    avformat_close_input(&pFormatContext);
}

void AudioReader::HandleFrame(AVCodecContext *pCodecContext, AVFrame *pFrame) {
    if (padding == -1.0) {
        padding = 0.0;
        if ((pFrame->pts * timeBase) > 0) {
            padding = pFrame->pts * timeBase;
        }
    }
    int ret = 0;
    float** outBuffer;
    int outLinesize = 0;
    int outNumChannels = mono ? 1 : pFrame->ch_layout.nb_channels;  // Usar ch_layout en lugar de channel_layout
    numChannels = outNumChannels;
    int outNumSamples = av_rescale_rnd(pFrame->nb_samples,
                                       this->targetSampleRate, pFrame->sample_rate, AV_ROUND_UP);
    if ((ret = av_samples_alloc_array_and_samples((uint8_t***)&outBuffer, &outLinesize, outNumChannels, outNumSamples,
                                                  AV_SAMPLE_FMT_FLTP, 0)) < 0) {
        LOG(FATAL) << "ERROR Failed to allocate resample buffer";
    }
    int gotSamples = swr_convert(this->swr, (uint8_t**)outBuffer, outNumSamples, (const uint8_t**)pFrame->extended_data, pFrame->nb_samples);
    totalConvertedSamplesPerChannel += gotSamples;
    CHECK_GE(gotSamples, 0) << "ERROR Failed to resample samples";
    SaveToVector(outBuffer, outNumChannels, gotSamples);
    while (gotSamples > 0) {
        gotSamples = swr_convert(this->swr, (uint8_t**)outBuffer, outNumSamples, NULL, 0);
        CHECK_GE(gotSamples, 0) << "ERROR Failed to flush resample buffer";
        totalConvertedSamplesPerChannel += gotSamples;
        SaveToVector(outBuffer, outNumChannels, gotSamples);
    }
    if (outBuffer) {
        av_freep(&outBuffer[0]);
    }
    av_freep(&outBuffer);
}

void AudioReader::DrainDecoder(AVCodecContext *pCodecContext, AVFrame *pFrame) {
    int ret = avcodec_send_packet(pCodecContext, NULL);
    if (ret != 0) {
        LOG(WARNING) << "Failed to send packet while draining";
        return;
    }
    int receiveRet = -1;
    while ((receiveRet = avcodec_receive_frame(pCodecContext, pFrame)) == 0) {
        totalSamplesPerChannel += pFrame->nb_samples;
        HandleFrame(pCodecContext, pFrame);
    }
    if (receiveRet != AVERROR(EAGAIN) && receiveRet != AVERROR_EOF) {
        LOG(WARNING) << "ERROR Fail to receive frame.";
    }
}

void AudioReader::InitSWR(AVCodecContext *pCodecContext) {
    int ret = 0;
    this->swr = swr_alloc();
    if (!this->swr) {
        LOG(FATAL) << "ERROR Failed to allocate resample context";
    }

    // Configurar layout de entrada
    av_opt_set_chlayout(this->swr, "in_channel_layout", &pCodecContext->ch_layout, 0);

    // Configurar layout de salida
    AVChannelLayout out_ch_layout;
    if (mono) {
        out_ch_layout = AV_CHANNEL_LAYOUT_MONO;  // Layout mono predefinido
    } else {
        out_ch_layout = pCodecContext->ch_layout;  // Mantener el layout del codec
    }
    av_opt_set_chlayout(this->swr, "out_channel_layout", &out_ch_layout, 0);

    av_opt_set_int(this->swr, "in_sample_rate", pCodecContext->sample_rate, 0);
    av_opt_set_int(this->swr, "out_sample_rate", this->targetSampleRate, 0);
    av_opt_set_sample_fmt(this->swr, "in_sample_fmt", pCodecContext->sample_fmt, 0);
    av_opt_set_sample_fmt(this->swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    if ((ret = swr_init(this->swr)) < 0) {
        LOG(FATAL) << "ERROR Failed to initialize resample context";
    }
}

void AudioReader::ToNDArray() {
    if (outputVector.empty()) return;
    int totalNumSamplesPerChannel = outputVector[0].size();
    std::vector<int64_t> shape {numChannels, totalNumSamplesPerChannel};
    output = NDArray::Empty(shape, kFloat32, ctx);
    std::vector<int64_t> channelShape {totalNumSamplesPerChannel};
    for (int c = 0; c < numChannels; c++) {
        uint64_t offset = c * totalNumSamplesPerChannel;
        NDArray channelOutput = NDArray::Empty(channelShape, kFloat32, ctx);
        channelOutput.CopyFrom(outputVector[c], channelShape);
        auto view = output.CreateOffsetView(channelShape, channelOutput.data_->dl_tensor.dtype, &offset);
        channelOutput.CopyTo(view);
    }
}

void AudioReader::SaveToVector(float **buffer, int numChannels, int numSamples) {
    if (outputVector.empty()) {
        outputVector = std::vector<std::vector<float>>(numChannels, std::vector<float>());
    }
    for (int c = 0; c < numChannels; c++) {
        for (int s = 0; s < numSamples; s++) {
            float val = buffer[c][s];
            outputVector[c].push_back(val);
        }
    }
}

}  // namespace decord