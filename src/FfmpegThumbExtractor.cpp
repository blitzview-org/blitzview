#include "FfmpegThumbExtractor.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
}

#include <QTransform>
#include <cmath>

namespace {

// Display rotation from the stream's display-matrix side data (phone videos
// store portrait orientation this way). Returns the clockwise angle the
// decoded frame must be rotated by for upright display (0/90/180/270).
//
// Two ways to reach the side data, because we must not be tied to one FFmpeg
// version: on Windows the FFmpeg that Qt Multimedia ships is what we link
// against, and its major is whatever the Qt release picked.
int displayRotationCW(const AVStream* stream)
{
#if LIBAVCODEC_VERSION_MAJOR >= 61   // FFmpeg 7.0+
    const AVCodecParameters* codecPar = stream->codecpar;
    const AVPacketSideData* sd = av_packet_side_data_get(
        codecPar->coded_side_data, codecPar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    const uint8_t* data = sd ? sd->data : nullptr;
    const size_t   size = sd ? sd->size : 0;
#else                                // deprecated in 7.0, the only way before
    size_t size = 0;
    const uint8_t* data = av_stream_get_side_data(
        stream, AV_PKT_DATA_DISPLAYMATRIX, &size);
#endif
    if (!data || size < 9 * sizeof(int32_t))
        return 0;

    // av_display_rotation_get returns counterclockwise degrees; players
    // rotate by the negative to display upright
    const double theta = av_display_rotation_get(
        reinterpret_cast<const int32_t*>(data));
    if (std::isnan(theta))
        return 0;

    int cw = static_cast<int>(std::lround(-theta)) % 360;
    if (cw < 0)
        cw += 360;
    return cw;
}

} // namespace

QImage extractVideoThumbnail(const QString& filePath, const QSize& targetSize)
{
    const QByteArray pathUtf8 = filePath.toUtf8();

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) < 0)
        return {};

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return {};
    }

    // Check for attached_pic (cover art) first — instant extraction
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            const AVPacket& pkt = fmtCtx->streams[i]->attached_pic;
            if (pkt.size > 0) {
                QImage img;
                if (img.loadFromData(pkt.data, pkt.size)) {
                    avformat_close_input(&fmtCtx);
                    if (img.size() != targetSize)
                        img = img.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    return img;
                }
            }
        }
    }

    // Find video stream
    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }

    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return {};
    }

    AVCodecParameters* codecPar = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return {};
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        avformat_close_input(&fmtCtx);
        return {};
    }

    avcodec_parameters_to_context(codecCtx, codecPar);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return {};
    }

    const int rotationCW = displayRotationCW(fmtCtx->streams[videoIdx]);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    QImage result;

    // Read packets until we decode a frame
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codecCtx, pkt) >= 0) {
            if (avcodec_receive_frame(codecCtx, frame) >= 0) {
                // Got a frame — convert to RGB
                const int srcW = frame->width;
                const int srcH = frame->height;
                const int dstW = targetSize.width();
                const int dstH = targetSize.height();

                // Compute aspect-correct target size
                double scale = qMin(static_cast<double>(dstW) / srcW,
                                    static_cast<double>(dstH) / srcH);
                int outW = qMax(1, static_cast<int>(srcW * scale));
                int outH = qMax(1, static_cast<int>(srcH * scale));

                SwsContext* swsCtx = sws_getContext(
                    srcW, srcH, static_cast<AVPixelFormat>(frame->format),
                    outW, outH, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);

                if (swsCtx) {
                    QImage img(outW, outH, QImage::Format_RGB888);
                    uint8_t* dstData[1] = { img.bits() };
                    int dstLinesize[1] = { static_cast<int>(img.bytesPerLine()) };
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, srcH,
                              dstData, dstLinesize);
                    sws_freeContext(swsCtx);
                    result = img;
                }

                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (!result.isNull() && rotationCW != 0)
        result = result.transformed(QTransform().rotate(rotationCW));

    return result;
}
