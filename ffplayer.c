#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include <libavutil/avstring.h>
#include <libavutil/eval.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/dict.h>
#include <libavutil/parseutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avassert.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/avfft.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <assert.h>

const char program_name[] = "ffplayer";
const int program_birth_year = 2018;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

static unsigned sws_flags = SWS_BICUBIC;

typedef struct MyAVPacketList
{
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue
{
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams
{
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock
{
    double pts;       /* clock base */
    double pts_drift; /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial; /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial; /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame
{
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;      /* presentation timestamp for the frame */
    double duration; /* estimated duration of the frame */
    int64_t pos;     /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue
{
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;

enum
{
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder
{
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState
{
    SDL_Thread *read_tid;
    AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    double max_frame_duration; // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
    int width, height, xleft, ytop;
    int step;

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;
} VideoState;

/*原版ffplay中的控制选项，这里直接写默认值*/
static AVInputFormat *file_iformat = NULL;
static const char *input_filename = NULL;
static const char *window_title = "ffplayer";
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int seek_by_bytes = -1;
static int borderless = 0;
static int startup_volume = 100;
static int show_status = 1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static const char *audio_codec_name = NULL;
static const char *subtitle_codec_name = NULL;
static const char *video_codec_name = NULL;
double rdftspeed = 0.02;
static int64_t cursor_last_shown = 0;
static int cursor_hidden = 0;
static int autorotate = 1;
static int find_stream_info = 1;

/* current context */
static int is_full_screen = 0;
static int64_t audio_callback_time = 0;

static AVPacket flush_pkt;

#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;

static const struct TextureFormatEntry
{
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
    {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
};

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                                 enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
    {
        return -1;
    }

    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
    {
        return -1;
    }

    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    if (pkt == &flush_pkt)
    {
        q->serial++;
    }

    pkt1->serial = q->serial;

    if (!q->last_pkt)
    {
        q->first_pkt = pkt1;
    }
    else
    {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;

    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);

    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    SDL_LockMutex(q->mutex);
    int ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
    {
        av_packet_unref(pkt);
    }

    return ret;
}
// null packet用于榨干decoder
static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    // av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;

    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));

    q->mutex = SDL_CreateMutex();
    if (!q->mutex)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    q->cond = SDL_CreateCond();
    if (!q->cond)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    q->abort_request = 1;

    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);

    for (pkt = q->first_pkt; pkt; pkt = pkt1)
    {
        pkt1 = pkt->next;

        if (pkt->pkt.size)
        {
            av_packet_unref(&pkt->pkt);
        }

        av_freep(&pkt);
    }

    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);

    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;)
    {
        if (q->abort_request)
        {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1)
        {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
            {
                q->last_pkt = NULL;
            }

            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            *pkt = pkt1->pkt;

            if (serial)
            {
                *serial = pkt1->serial;
            }

            av_free(pkt1);
            ret = 1;
            break;
        }
        else if (!block)
        {
            ret = 0;
            break;
        }
        else
        {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);

    return ret;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond)
{
    memset(d, 0, sizeof(Decoder));

    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

// 参考：https://zhuanlan.zhihu.com/p/43948483
static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
    int ret = AVERROR(EAGAIN);

    // 循环直到出错，或解出一帧，分3个主要步骤
    for (;;)
    {
        AVPacket pkt;
        // 1. 流连续的情况下，不断调用avcodec_receive_frame获取解码后的frame
        if (d->queue->serial == d->pkt_serial)
        {
            // serial相等，则decoder中的帧有效
            do
            {
                if (d->queue->abort_request)
                {
                    return -1;
                }

                switch (d->avctx->codec_type)
                {
                case AVMEDIA_TYPE_VIDEO:

                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0)
                    {
                        if (decoder_reorder_pts == -1)
                        {
                            frame->pts = frame->best_effort_timestamp;
                        }
                        else if (!decoder_reorder_pts)
                        {
                            frame->pts = frame->pkt_dts;
                        }
                    }
                    break;

                case AVMEDIA_TYPE_AUDIO:

                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0)
                    {
                        AVRational tb = (AVRational){1, frame->sample_rate};
                        if (frame->pts != AV_NOPTS_VALUE)
                        {
                            frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                        }
                        else if (d->next_pts != AV_NOPTS_VALUE)
                        {
                            frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                        }

                        if (frame->pts != AV_NOPTS_VALUE)
                        {
                            d->next_pts = frame->pts + frame->nb_samples;
                            d->next_pts_tb = tb;
                        }
                    }
                    break;

                default:
                    break;
                }

                if (ret == AVERROR_EOF)
                {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }

                if (ret >= 0)
                {
                    return 1;
                }

            } while (ret != AVERROR(EAGAIN));
        }

        // 2. 取一个packet，顺带过滤“过时”的packet
        do
        {
            if (d->queue->nb_packets == 0)
            {
                SDL_CondSignal(d->empty_queue_cond);
            }

            if (d->packet_pending)
            {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            }
            else
            {
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                {
                    return -1;
                }
            }

        } while (d->queue->serial != d->pkt_serial);

        if (pkt.data == flush_pkt.data)
        {
            // flush_pkt处理：avcodec_flush_buffers
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        }
        else
        {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                // subtitle的解码比较特别
                int got_frame = 0;

                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0)
                {
                    ret = AVERROR(EAGAIN);
                }
                else
                {
                    if (got_frame && !pkt.data)
                    {
                        // 参考avcodec_decode_subtitle2注释，使用pkt.data = NULL && pkt.size = 0触发flush，
                        // 这里判断如果有帧输出，且pkt.data==NULL，就一直flush(这就是个null_packet呀)
                        d->packet_pending = 1;
                        av_packet_move_ref(&d->pkt, &pkt);
                    }

                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            }
            else
            {
                // 3. 将packet送入解码器
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN))
                {
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }

            if (pkt.size)
            {
                av_packet_unref(&pkt);
            }
        }
    }
}

static void decoder_destroy(Decoder *d)
{
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    memset(f, 0, sizeof(FrameQueue));

    if (!(f->mutex = SDL_CreateMutex()))
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    if (!(f->cond = SDL_CreateCond()))
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;

    for (int i = 0; i < f->max_size; i++)
    {
        if (!(f->queue[i].frame = av_frame_alloc()))
        {
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static void frame_queue_destory(FrameQueue *f)
{
    for (int i = 0; i < f->max_size; i++)
    {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }

    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);

    while (f->size >= f->max_size && !f->pktq->abort_request)
    {
        SDL_CondWait(f->cond, f->mutex);
    }

    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
    {
        return NULL;
    }

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);

    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request)
    {
        SDL_CondWait(f->cond, f->mutex);
    }

    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
    {
        return NULL;
    }

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
    {
        f->windex = 0;
    }

    SDL_LockMutex(f->mutex);

    f->size++;

    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown)
    {
        f->rindex_shown = 1;
        return;
    }

    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
    {
        f->rindex = 0;
    }

    SDL_LockMutex(f->mutex);

    f->size--;

    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];

    if (f->rindex_shown && fp->serial == f->pktq->serial)
    {
        return fp->pos;
    }

    return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);

    SDL_WaitThread(d->decoder_tid, NULL);

    d->decoder_tid = NULL;

    packet_queue_flush(d->queue);
}

static inline void fill_rectangle(int x, int y, int w, int h)
{
    SDL_Rect rect;

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    if (w && h)
    {
        SDL_RenderFillRect(renderer, &rect);
    }
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;

    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format)
    {
        void *pixels;
        int pitch;

        if (*texture)
        {
            SDL_DestroyTexture(*texture);
        }

        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
        {
            return -1;
        }

        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
        {
            return -1;
        }

        if (init_texture)
        {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
            {
                return -1;
            }

            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }

        av_log(NULL, AV_LOG_VERBOSE,
               "Created %dx%d texture with %s.\n",
               new_width, new_height, SDL_GetPixelFormatName(new_format));
    }

    return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    float aspect_ratio;

    if (pic_sar.num == 0)
    {
        aspect_ratio = 0;
    }
    else
    {
        aspect_ratio = av_q2d(pic_sar);
    }

    if (aspect_ratio <= 0.0)
    {
        aspect_ratio = 1.0;
    }

    aspect_ratio *= (float)pic_width / (float)pic_height;

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    int height = scr_height;
    int width = lrint(height * aspect_ratio) & ~1;

    if (width > scr_width)
    {
        width = scr_width;
        height = lrint(width / aspect_ratio) & ~1;
    }

    int x = (scr_width - width) / 2;
    int y = (scr_height - height) / 2;

    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX(width, 1);
    rect->h = FFMAX(height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;

    if (format == AV_PIX_FMT_RGB32 ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32 ||
        format == AV_PIX_FMT_BGR32_1)
    {
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++)
    {
        if (format == sdl_texture_format_map[i].format)
        {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx)
{
    int ret = 0;

    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;

    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);

    if (realloc_texture(tex,
                        sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
                        frame->width,
                        frame->height,
                        sdl_blendmode,
                        0) < 0)
    {
        return -1;
    }

    switch (sdl_pix_fmt)
    {
    case SDL_PIXELFORMAT_UNKNOWN:

        /* This should only happen if we are not using avfilter... */
        *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                                frame->width,
                                                frame->height,
                                                frame->format,
                                                frame->width,
                                                frame->height,
                                                AV_PIX_FMT_BGRA,
                                                sws_flags,
                                                NULL,
                                                NULL,
                                                NULL);
        if (*img_convert_ctx != NULL)
        {
            uint8_t *pixels[4];
            int pitch[4];

            if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch))
            {
                sws_scale(*img_convert_ctx,
                          (const uint8_t *const *)frame->data,
                          frame->linesize,
                          0,
                          frame->height,
                          pixels,
                          pitch);

                SDL_UnlockTexture(*tex);
            }
        }
        else
        {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            ret = -1;
        }
        break;

    case SDL_PIXELFORMAT_IYUV:

        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0)
        {
            ret = SDL_UpdateYUVTexture(*tex, NULL,
                                       frame->data[0], frame->linesize[0],
                                       frame->data[1], frame->linesize[1],
                                       frame->data[2], frame->linesize[2]);
        }
        else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0)
        {
            ret = SDL_UpdateYUVTexture(*tex,
                                       NULL,
                                       frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                       -frame->linesize[0],
                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                       -frame->linesize[1],
                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                       -frame->linesize[2]);
        }
        else
        {
            av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
            return -1;
        }
        break;

    default:

        if (frame->linesize[0] < 0)
        {
            ret = SDL_UpdateTexture(*tex,
                                    NULL,
                                    frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                    -frame->linesize[0]);
        }
        else
        {
            ret = SDL_UpdateTexture(*tex,
                                    NULL,
                                    frame->data[0],
                                    frame->linesize[0]);
        }
        break;
    }

    return ret;
}

static Frame *subtitle_refresh_render(VideoState *is, Frame *vp)
{
    Frame *sp = NULL;

    if (frame_queue_nb_remaining(&is->subpq) > 0)
    {
        sp = frame_queue_peek(&is->subpq);

        if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000))
        {
            if (!sp->uploaded)
            {
                uint8_t *pixels[4];
                int pitch[4];

                if (!sp->width || !sp->height)
                {
                    sp->width = vp->width;
                    sp->height = vp->height;
                }

                if (realloc_texture(&is->sub_texture,
                                    SDL_PIXELFORMAT_ARGB8888,
                                    sp->width,
                                    sp->height,
                                    SDL_BLENDMODE_BLEND, 1) < 0)
                {
                    return NULL;
                }

                for (int i = 0; i < sp->sub.num_rects; i++)
                {
                    AVSubtitleRect *sub_rect = sp->sub.rects[i];

                    sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                    sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                    sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                    sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                    is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                               sub_rect->w,
                                                               sub_rect->h,
                                                               AV_PIX_FMT_PAL8,
                                                               sub_rect->w,
                                                               sub_rect->h,
                                                               AV_PIX_FMT_BGRA,
                                                               0,
                                                               NULL,
                                                               NULL,
                                                               NULL);

                    if (!is->sub_convert_ctx)
                    {
                        av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                        return NULL;
                    }

                    if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch))
                    {
                        sws_scale(is->sub_convert_ctx,
                                  (const uint8_t *const *)sub_rect->data,
                                  sub_rect->linesize,
                                  0,
                                  sub_rect->h,
                                  pixels,
                                  pitch);

                        SDL_UnlockTexture(is->sub_texture);
                    }
                }

                sp->uploaded = 1;
            }
        }
        else
        {
            sp = NULL;
        }
    }

    return sp;
}

static void video_image_display(VideoState *is)
{
    Frame *vp, *sp;
    SDL_Rect rect;

    vp = frame_queue_peek_last(&is->pictq);
    if (is->subtitle_st)
    {
        sp = subtitle_refresh_render(is, vp);
    }

    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

    if (!vp->uploaded)
    {
        if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx) < 0)
        {
            return;
        }

        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
    if (sp)
    {
        SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
    }
}

static inline int compute_mod(int a, int b)
{
    return a < 0 ? a % b + b : a % b;
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
    {
        return;
    }

    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:

        decoder_abort(&is->auddec, &is->sampq);

        SDL_CloseAudioDevice(audio_dev);

        decoder_destroy(&is->auddec);

        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);

        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft)
        {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;

    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;

    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;

    switch (codecpar->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;

    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;

    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream >= 0)
    {
        stream_component_close(is, is->audio_stream);
    }

    if (is->video_stream >= 0)
    {
        stream_component_close(is, is->video_stream);
    }

    if (is->subtitle_stream >= 0)
    {
        stream_component_close(is, is->subtitle_stream);
    }

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);

    SDL_DestroyCond(is->continue_read_thread);

    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);

    av_free(is->filename);

    if (is->vis_texture)
    {
        SDL_DestroyTexture(is->vis_texture);
    }

    if (is->vid_texture)
    {
        SDL_DestroyTexture(is->vid_texture);
    }

    if (is->sub_texture)
    {
        SDL_DestroyTexture(is->sub_texture);
    }

    av_free(is);
}

static void do_exit(VideoState *is)
{
    if (is)
    {
        stream_close(is);
    }

    if (renderer)
    {
        SDL_DestroyRenderer(renderer);
    }

    if (window)
    {
        SDL_DestroyWindow(window);
    }

    avformat_network_deinit();

    if (show_status)
    {
        printf("\n");
    }

    SDL_Quit();

    av_log(NULL, AV_LOG_QUIET, "%s", "");

    exit(0);
}

static void sigterm_handler(int sig)
{
    exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;

    calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);

    default_width = rect.w;
    default_height = rect.h;
}

static int video_open(VideoState *is)
{
    int w, h;

    if (screen_width)
    {
        w = screen_width;
        h = screen_height;
    }
    else
    {
        w = default_width;
        h = default_height;
    }

    if (!window_title)
    {
        window_title = input_filename;
    }

    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    if (is_full_screen)
    {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    SDL_ShowWindow(window);

    is->width = w;
    is->height = h;

    return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is)
{
    if (!is->width)
    {
        video_open(is);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (is->video_st)
    {
        video_image_display(is);
    }

    SDL_RenderPresent(renderer);
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
    {
        return NAN;
    }

    if (c->paused)
    {
        return c->pts;
    }
    else
    {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;

    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);

    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
    {
        set_clock(c, slave_clock, slave->serial);
    }
}

static int get_master_sync_type(VideoState *is)
{
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
    {
        if (is->video_st)
        {
            return AV_SYNC_VIDEO_MASTER;
        }

        return AV_SYNC_AUDIO_MASTER;
    }
    else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
    {
        if (is->audio_st)
        {
            return AV_SYNC_AUDIO_MASTER;
        }

        return AV_SYNC_EXTERNAL_CLOCK;
    }
    else
    {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is))
    {
    case AV_SYNC_VIDEO_MASTER:
        val = get_clock(&is->vidclk);
        break;

    case AV_SYNC_AUDIO_MASTER:
        val = get_clock(&is->audclk);
        break;

    default:
        val = get_clock(&is->extclk);
        break;
    }

    return val;
}

static void check_external_clock_speed(VideoState *is)
{
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
    {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES))
    {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else
    {
        double speed = is->extclk.speed;
        if (speed != 1.0)
        {
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req)
    {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;

        if (seek_by_bytes)
        {
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        }

        is->seek_req = 1;

        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused)
    {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS))
        {
            is->vidclk.paused = 0;
        }

        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }

    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

static void toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

static void update_volume(VideoState *is, int sign, double step)
{
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
    {
        stream_toggle_pause(is);
    }

    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
    {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));

        if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
        {
            if (diff <= -sync_threshold)
            {
                delay = FFMAX(0, delay + diff);
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
            {
                delay = delay + diff;
            }
            else if (diff >= sync_threshold)
            {
                delay = 2 * delay;
            }
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial)
    {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
        {
            return vp->duration;
        }

        return duration;
    }
    else
    {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

static void show_status_in_video_refresh(VideoState *is)
{
    static int64_t last_time;
    int64_t cur_time;
    int aqsize, vqsize, sqsize;
    double av_diff;
    cur_time = av_gettime_relative();

    if (!last_time || (cur_time - last_time) >= 30000)
    {
        aqsize = vqsize = sqsize = 0;

        if (is->audio_st)
        {
            aqsize = is->audioq.size;
        }

        if (is->video_st)
        {
            vqsize = is->videoq.size;
        }

        if (is->subtitle_st)
        {
            sqsize = is->subtitleq.size;
        }

        av_diff = 0;

        if (is->audio_st && is->video_st)
        {
            av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
        }
        else if (is->video_st)
        {
            av_diff = get_master_clock(is) - get_clock(&is->vidclk);
        }
        else if (is->audio_st)
        {
            av_diff = get_master_clock(is) - get_clock(&is->audclk);
        }

        av_log(NULL, AV_LOG_INFO,
               "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%" PRId64 "/%" PRId64 "   \r",
               get_master_clock(is),
               (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
               av_diff,
               is->frame_drops_early + is->frame_drops_late,
               aqsize / 1024,
               vqsize / 1024,
               sqsize,
               is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
               is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);

        fflush(stdout);

        last_time = cur_time;
    }
}

static void subtitle_refresh_hide_or_skip(VideoState *is)
{
    Frame *sp, *sp2;

    while (frame_queue_nb_remaining(&is->subpq) > 0)
    {
        sp = frame_queue_peek(&is->subpq);
        if (frame_queue_nb_remaining(&is->subpq) > 1)
        {
            sp2 = frame_queue_peek_next(&is->subpq);
        }
        else
        {
            sp2 = NULL;
        }

        if (sp->serial != is->subtitleq.serial ||
            (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000))) ||
            (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
        {
            if (sp->uploaded)
            {
                for (int i = 0; i < sp->sub.num_rects; i++)
                {
                    AVSubtitleRect *sub_rect = sp->sub.rects[i];

                    uint8_t *pixels;
                    int pitch;

                    if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch))
                    {
                        for (int j = 0; j < sub_rect->h; j++, pixels += pitch)
                        {
                            memset(pixels, 0, sub_rect->w << 2);
                        }

                        SDL_UnlockTexture(is->sub_texture);
                    }
                }
            }

            frame_queue_next(&is->subpq);
        }
        else
        {
            break;
        }
    }
}

/* called to display each frame */
// 参考：https://zhuanlan.zhihu.com/p/44122324
static void video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = opaque;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
    {
        check_external_clock_speed(is);
    }

    if (is->video_st)
    {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0)
        {
            // nothing to do, no picture to display in the queue
        }
        else
        {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial)
            {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
            {
                goto display;
            }

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            double time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay)
            {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
            {
                is->frame_timer = time;
            }

            SDL_LockMutex(is->pictq.mutex);

            if (!isnan(vp->pts))
            {
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            }

            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1)
            {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);

                if (!is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration)
                {
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st)
            {
                subtitle_refresh_hide_or_skip(is);
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
            {
                stream_toggle_pause(is);
            }
        }

    display:
        /* display picture */
        if (is->force_refresh && is->pictq.rindex_shown)
        {
            video_display(is);
        }
    }

    is->force_refresh = 0;

    if (show_status)
    {
        show_status_in_video_refresh(is);
    }
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp = frame_queue_peek_writable(&is->pictq);
    if (!vp)
    {
        return -1;
    }

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);

    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture = decoder_decode_frame(&is->viddec, frame, NULL);

    if (got_picture < 0)
    {
        return -1;
    }

    if (got_picture)
    {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
        {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
        {
            if (frame->pts != AV_NOPTS_VALUE)
            {
                double diff = dpts - get_master_clock(is);

                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 && // 如果diff < 0，说明已经时间已过，不能显示了，drop；frame_last_filter_delay用上次filter耗时估计本次耗时时间；dpts < (get_master_clock(is) + is->frame_last_filter_delay)，等效于认为在filter计算完后才拿到解码后的帧，此时，如果时间已过，则不能显示了。
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets)
                {
                    av_log(NULL, AV_LOG_ERROR, "drop early: %d\n", is->videoq.nb_packets);

                    is->frame_drops_early++;

                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int audio_thread(void *arg)
{
    VideoState *is = arg;

    AVFrame *frame = av_frame_alloc();

    if (!frame)
    {
        return AVERROR(ENOMEM);
    }

    while (1)
    {
        int got_frame = decoder_decode_frame(&is->auddec, frame, NULL);
        if (got_frame < 0)
        {
            goto the_end;
        }

        if (got_frame)
        {
            AVRational tb = (AVRational){1, frame->sample_rate};

            Frame *af = frame_queue_peek_writable(&is->sampq);
            if (!af)
            {
                goto the_end;
            }

            af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            af->pos = frame->pkt_pos;
            af->serial = is->auddec.pkt_serial;
            af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

            av_frame_move_ref(af->frame, frame);
            frame_queue_push(&is->sampq);
        }
    }

the_end:

    av_frame_free(&frame);

    return 0;
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);

    if (!d->decoder_tid)
    {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int video_thread(void *arg)
{
    VideoState *is = arg;

    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        return AVERROR(ENOMEM);
    }

    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    while (1)
    {
        int ret = get_video_frame(is, frame);
        if (ret < 0)
        {
            goto the_end;
        }

        if (!ret)
        {
            continue;
        }

        double duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
        double pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);

        ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);

        av_frame_unref(frame);

        if (ret < 0)
        {
            goto the_end;
        }
    }

the_end:

    av_frame_free(&frame);

    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = arg;

    while (1)
    {
        // 注意这里是先frame_queue_peek_writable再decoder_decode_frame
        Frame *sp = frame_queue_peek_writable(&is->subpq);
        if (!sp)
        {
            return 0;
        }

        int got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub);
        if (got_subtitle < 0)
        {
            break;
        }

        double pts = 0;

        if (got_subtitle && sp->sub.format == 0)
        {
            if (sp->sub.pts != AV_NOPTS_VALUE)
            {
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            }

            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        }
        else if (got_subtitle)
        {
            avsubtitle_free(&sp->sub);
        }
    }

    return 0;
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
// 参考：https://zhuanlan.zhihu.com/p/44680734
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER)
    {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD)
        {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
            {
                /* not enough measures to have a correct estimate */
                av_log(NULL, AV_LOG_ERROR, "\nadd cum: %d\n", is->audio_diff_avg_count);
                is->audio_diff_avg_count++;
            }
            else
            {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold)
                {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }

                av_log(NULL, AV_LOG_TRACE,
                       "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        }
        else
        {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

static int check_init_swr(VideoState *is, Frame *af, int data_size)
{
    int wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    // 两种情况需要“重采样”：
    // 1. 音频源格式与输出格式不同
    // 2. 样本数需要调整（发生在“音频同步到视频”的时候）
    if (af->frame->format != is->audio_src.fmt ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate != is->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx))
    {
        // 判断是否需要swresample，初始化swr_ctx
        swr_free(&is->swr_ctx);

        swr_alloc_set_opts2(&is->swr_ctx,
                            &is->audio_tgt.ch_layout,
                            is->audio_tgt.fmt,
                            is->audio_tgt.freq,
                            &af->frame->ch_layout,
                            af->frame->format,
                            af->frame->sample_rate,
                            0,
                            NULL);

        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0)
        {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);

            swr_free(&is->swr_ctx);

            return -1;
        }

        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
        {
            return -1;
        }

        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    return wanted_nb_samples;
}

static int do_resample(VideoState *is, Frame *af, int wanted_nb_samples)
{
    const uint8_t **in = (const uint8_t **)af->frame->extended_data;

    uint8_t **out = &is->audio_buf1;
    int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;

    int out_size = av_samples_get_buffer_size(NULL,
                                              is->audio_tgt.ch_layout.nb_channels,
                                              out_count,
                                              is->audio_tgt.fmt,
                                              0);
    if (out_size < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
        return -1;
    }

    if (wanted_nb_samples != af->frame->nb_samples)
    {
        if (swr_set_compensation(is->swr_ctx,
                                 (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                 wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
            return -1;
        }
    }

    av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);

    if (!is->audio_buf1)
    {
        return AVERROR(ENOMEM);
    }

    int len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
        return -1;
    }

    if (len2 == out_count)
    {
        av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
        if (swr_init(is->swr_ctx) < 0)
            swr_free(&is->swr_ctx);
    }

    is->audio_buf = is->audio_buf1;

    return len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
}

static void update_audio_pts(VideoState *is, Frame *af)
{
    if (!isnan(af->pts))
    {
        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    }
    else
    {
        is->audio_clock = NAN;
    }

    is->audio_clock_serial = af->serial;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is)
{
    int wanted_nb_samples, resampled_data_size;

    if (is->paused)
    {
        return -1;
    }

    Frame *af;

    do
    {
        af = frame_queue_peek_readable(&is->sampq);
        if (!af)
        {
            return -1;
        }

        frame_queue_next(&is->sampq);

    } while (af->serial != is->audioq.serial);

    int data_size = av_samples_get_buffer_size(NULL,
                                               af->frame->ch_layout.nb_channels,
                                               af->frame->nb_samples,
                                               af->frame->format,
                                               1);

    if ((wanted_nb_samples = check_init_swr(is, af, data_size)) < 0)
    {
        return -1;
    }

    if (is->swr_ctx)
    {
        if ((resampled_data_size = do_resample(is, af, wanted_nb_samples)) < 0)
        {
            return -1;
        }
    }
    else
    {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    /* update the audio clock with the pts */
    update_audio_pts(is, af);

    return resampled_data_size;
}

/* prepare a new audio buffer */
// 参考：https://zhuanlan.zhihu.com/p/44139512
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;

    audio_callback_time = av_gettime_relative();

    while (len > 0)
    {
        if (is->audio_buf_index >= is->audio_buf_size)
        {
            int audio_size = audio_decode_frame(is);
            if (audio_size < 0)
            {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            }
            else
            {
                is->audio_buf_size = audio_size;
            }

            is->audio_buf_index = 0;
        }

        int len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
        {
            len1 = len;
        }

        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
        {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        }
        else
        {
            memset(stream, 0, len1);

            if (!is->muted && is->audio_buf)
            {
                SDL_MixAudioFormat(stream,
                                   (uint8_t *)is->audio_buf + is->audio_buf_index,
                                   AUDIO_S16SYS,
                                   len1,
                                   is->audio_volume);
            }
        }

        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }

    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;

    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock))
    {
        set_clock_at(&is->audclk,
                     is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec,
                     is->audio_clock_serial,
                     audio_callback_time / 1000000.0);

        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};

    SDL_AudioSpec wanted_spec, spec;

    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;

    const char *env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env)
    {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE)
    {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }

    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;

    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }

    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
    {
        next_sample_rate_idx--;
    }

    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;

    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE)))
    {
        av_log(NULL, AV_LOG_WARNING,
               "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());

        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels)
        {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;

            if (!wanted_spec.freq)
            {
                av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
                return -1;
            }
        }

        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }

    if (spec.format != AUDIO_S16SYS)
    {
        av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }

    if (spec.channels != wanted_spec.channels)
    {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);

        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE)
        {
            av_log(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;

    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
    {
        return -1;
    }

    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL,
                                                             audio_hw_params->ch_layout.nb_channels,
                                                             1,
                                                             audio_hw_params->fmt,
                                                             1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL,
                                                                audio_hw_params->ch_layout.nb_channels,
                                                                audio_hw_params->freq,
                                                                audio_hw_params->fmt,
                                                                1);

    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    return spec.size;
}

static int open_decoder(VideoState *is, AVCodec *codec, AVCodecContext *avctx, int stream_index)
{
    AVFormatContext *ic = is->ic;

    int stream_lowres = lowres;

    avctx->pkt_timebase = ic->streams[stream_index]->time_base;
    avctx->codec_id = codec->id;

    if (stream_lowres > codec->max_lowres)
    {
        av_log(avctx, AV_LOG_WARNING,
               "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }

    avctx->lowres = stream_lowres;

    if (fast)
    {
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    int ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0)
    {
        return ret;
    }

    return 0;
}

static int find_decoder(VideoState *is, AVCodecContext *avctx, int stream_index, AVCodec **ret)
{
    AVCodec *codec = (AVCodec *)avcodec_find_decoder(avctx->codec_id);

    const char *forced_codec_name = NULL;

    switch (avctx->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        is->last_audio_stream = stream_index;
        forced_codec_name = audio_codec_name;
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        is->last_subtitle_stream = stream_index;
        forced_codec_name = subtitle_codec_name;
        break;

    case AVMEDIA_TYPE_VIDEO:
        is->last_video_stream = stream_index;
        forced_codec_name = video_codec_name;
        break;

    default:
        break;
    }

    if (forced_codec_name)
    {
        codec = (AVCodec *)avcodec_find_decoder_by_name(forced_codec_name);
    }

    if (!codec)
    {
        if (forced_codec_name)
        {
            av_log(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
        }
        else
        {
            av_log(NULL, AV_LOG_WARNING, "No codec could be found with id %d\n", avctx->codec_id);
        }

        return AVERROR(EINVAL);
    }

    *ret = codec;

    return 0;
}

static int stream_component_open_audio(VideoState *is, AVCodecContext *avctx, int stream_index)
{
    AVFormatContext *ic = is->ic;

    int sample_rate = avctx->sample_rate;
    AVChannelLayout channel_layout = avctx->ch_layout;

    /* prepare audio output */
    int ret = audio_open(is, &channel_layout, sample_rate, &is->audio_tgt);
    if (ret < 0)
    {
        avcodec_free_context(&avctx);
        return ret;
    }

    is->audio_hw_buf_size = ret;
    is->audio_src = is->audio_tgt;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;

    /* init averaging filter */
    is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    is->audio_diff_avg_count = 0;

    /* since we do not have a precise anough audio FIFO fullness,
       we correct audio sync only if larger than this threshold */
    is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

    is->audio_stream = stream_index;
    is->audio_st = ic->streams[stream_index];

    decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);

    if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek)
    {
        is->auddec.start_pts = is->audio_st->start_time;
        is->auddec.start_pts_tb = is->audio_st->time_base;
    }

    if ((ret = decoder_start(&is->auddec, audio_thread, is)) < 0)
    {
        return ret;
    }

    SDL_PauseAudioDevice(audio_dev, 0);

    return ret;
}

static int stream_component_open_video(VideoState *is, AVCodecContext *avctx, int stream_index)
{
    AVFormatContext *ic = is->ic;

    is->video_stream = stream_index;
    is->video_st = ic->streams[stream_index];

    decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);

    int ret = decoder_start(&is->viddec, video_thread, is);
    if (ret < 0)
    {
        return ret;
    }

    is->queue_attachments_req = 1;

    return ret;
}

static int stream_component_open_subtitle(VideoState *is, AVCodecContext *avctx, int stream_index)
{
    AVFormatContext *ic = is->ic;

    is->subtitle_stream = stream_index;
    is->subtitle_st = ic->streams[stream_index];

    decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);

    int ret = decoder_start(&is->subdec, subtitle_thread, is);
    if (ret < 0)
    {
        return ret;
    }

    return ret;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
    {
        return -1;
    }

    AVCodecContext *avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
    {
        return AVERROR(ENOMEM);
    }

    int ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
    {
        goto fail;
    }

    AVCodec *codec;
    if ((ret = find_decoder(is, avctx, stream_index, &codec)) != 0)
    {
        goto fail;
    }

    if ((ret = open_decoder(is, codec, avctx, stream_index)) != 0)
    {
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    switch (avctx->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        ret = stream_component_open_audio(is, avctx, stream_index);
        break;

    case AVMEDIA_TYPE_VIDEO:
        ret = stream_component_open_video(is, avctx, stream_index);
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        ret = stream_component_open_subtitle(is, avctx, stream_index);
        break;

    default:
        break;
    }

    goto out;

fail:

    avcodec_free_context(&avctx);

out:

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s)
{
    if (!strcmp(s->iformat->name, "rtp") ||
        !strcmp(s->iformat->name, "rtsp") ||
        !strcmp(s->iformat->name, "sdp"))
    {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4) ||
                  !strncmp(s->url, "udp:", 4)))
    {
        return 1;
    }

    return 0;
}

static int open_input_file(AVFormatContext **ctx, VideoState *is)
{
    AVDictionaryEntry *t;

    int ret = 0;

    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    AVFormatContext *ic = avformat_alloc_context();
    if (!ic)
    {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;

    int err = avformat_open_input(&ic, is->filename, is->iformat, NULL);

    if (err < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "failed to open %s: %d", is->filename, err);
        ret = -1;
        goto fail;
    }

    is->ic = ic;

    if (genpts)
    {
        ic->flags |= AVFMT_FLAG_GENPTS;
    }

    av_format_inject_global_side_data(ic);

    if (find_stream_info)
    {
        err = avformat_find_stream_info(ic, NULL);

        if (err < 0)
        {
            av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
    {
        // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
        ic->pb->eof_reached = 0;
    }

    if (seek_by_bytes < 0)
    {
        seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
    }

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
    {
        window_title = av_asprintf("%s - %s", t->value, input_filename);
    }

    is->realtime = is_realtime(ic);
    *ctx = ic;

    goto out;

fail:

    if (ic && !is->ic)
    {
        avformat_close_input(&ic);
    }

out:

    return ret;
}

static void seek_to_start_time(AVFormatContext *ic, VideoState *is)
{
    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE)
    {
        int64_t timestamp = start_time;

        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
        {
            timestamp += ic->start_time;
        }

        int ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not seek to position %0.3f\n",
                   is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }
}

static void find_best_streams(AVFormatContext *ic, int st_index[AVMEDIA_TYPE_NB])
{
    for (int i = 0; i < AVMEDIA_TYPE_NB; i++)
    {
        st_index[i] = -1;
    }

    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic,
                                                       AVMEDIA_TYPE_VIDEO,
                                                       st_index[AVMEDIA_TYPE_VIDEO],
                                                       -1,
                                                       NULL,
                                                       0);
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic,
                                                       AVMEDIA_TYPE_AUDIO,
                                                       st_index[AVMEDIA_TYPE_AUDIO],
                                                       st_index[AVMEDIA_TYPE_VIDEO],
                                                       NULL,
                                                       0);
    st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic,
                                                          AVMEDIA_TYPE_SUBTITLE,
                                                          st_index[AVMEDIA_TYPE_SUBTITLE],
                                                          (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
                                                          NULL,
                                                          0);
}

static void update_window_size(AVFormatContext *ic, int video_index)
{
    if (video_index >= 0)
    {
        AVStream *st = ic->streams[video_index];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);

        if (codecpar->width)
        {
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }
}

static int open_the_streams(VideoState *is, int st_index[AVMEDIA_TYPE_NB])
{
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
    {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    int ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
    {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
    {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", is->filename);
        ret = -1;
    }

    return ret;
}

static int read_thread_loop_handle_pause(AVFormatContext *ic, VideoState *is)
{
    int ret = 0;

    if (is->paused != is->last_paused)
    {
        is->last_paused = is->paused;

        if (is->paused)
        {
            is->read_pause_return = av_read_pause(ic);
        }
        else
        {
            av_read_play(ic);
        }
    }

    if (is->paused && (!strcmp(ic->iformat->name, "rtsp") || (ic->pb && !strncmp(input_filename, "mmsh:", 5))))
    {
        /* wait 10 ms to avoid trying to get another packet */
        /* XXX: horrible */
        SDL_Delay(10);
        ret = 1;
    }

    return ret;
}

static int read_thread_loop_handle_seek(AVFormatContext *ic, VideoState *is)
{
    if (is->seek_req)
    {
        int64_t seek_target = is->seek_pos;
        int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
        int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;

        // FIXME the +-2 is due to rounding being not done in the correct direction in generation of the seek_pos/seek_rel variables
        int ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
        }
        else
        {
            if (is->audio_stream >= 0)
            {
                packet_queue_flush(&is->audioq);
                packet_queue_put(&is->audioq, &flush_pkt);
            }

            if (is->subtitle_stream >= 0)
            {
                packet_queue_flush(&is->subtitleq);
                packet_queue_put(&is->subtitleq, &flush_pkt);
            }

            if (is->video_stream >= 0)
            {
                packet_queue_flush(&is->videoq);
                packet_queue_put(&is->videoq, &flush_pkt);
            }

            if (is->seek_flags & AVSEEK_FLAG_BYTE)
            {
                set_clock(&is->extclk, NAN, 0);
            }
            else
            {
                set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
            }
        }

        is->seek_req = 0;
        is->queue_attachments_req = 1;
        is->eof = 0;

        if (is->paused)
        {
            step_to_next_frame(is);
        }
    }

    return 0;
}

static int read_thread_loop_handle_queue_attachments_req(AVFormatContext *ic, VideoState *is)
{
    if (is->queue_attachments_req)
    {
        if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        {
            AVPacket copy = {0};

            int ret = av_packet_ref(&copy, &is->video_st->attached_pic);
            if (ret < 0)
            {
                return ret;
            }

            packet_queue_put(&is->videoq, &copy);
            packet_queue_put_nullpacket(&is->videoq, is->video_stream);
        }

        is->queue_attachments_req = 0;
    }

    return 0;
}

static int read_thread_loop_handle_queue_full(VideoState *is, SDL_mutex *wait_mutex)
{
    /* if the queue are full, no need to read more */
    if (infinite_buffer < 1 &&
        (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE ||
         (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
          stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
          stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq))))
    {
        /* wait 10 ms */
        SDL_LockMutex(wait_mutex);
        SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
        SDL_UnlockMutex(wait_mutex);
        return 1;
    }

    return 0;
}

static int read_thread_loop_handle_loop(VideoState *is)
{
    int ret = 0;

    if (!is->paused &&
        (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
        (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0)))
    {
        if (loop != 1 && (!loop || --loop))
        {
            stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
        }
        else if (autoexit)
        {
            ret = AVERROR_EOF;
        }
    }

    return ret;
}

/**
 * 一个宏方便在for循环内调用子函数
 * 子函数返回值：
 * 0  继续往下执行
 * <0 goto fail
 * >0 continue循环
 */
#define READ_THREAD_LOOP_CALL(func) \
    {                               \
        ret = func;                 \
        if (ret < 0)                \
            goto fail;              \
        else if (ret > 0)           \
            continue;               \
    }

static int read_thread_loop(AVFormatContext *ic, VideoState *is)
{
    int ret = 0;
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    int64_t pkt_ts;

    SDL_mutex *wait_mutex = SDL_CreateMutex();
    if (!wait_mutex)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
    {
        infinite_buffer = 1;
    }

    while (1)
    {
        if (is->abort_request)
        {
            break;
        }

        READ_THREAD_LOOP_CALL(read_thread_loop_handle_pause(ic, is));
        READ_THREAD_LOOP_CALL(read_thread_loop_handle_seek(ic, is));
        READ_THREAD_LOOP_CALL(read_thread_loop_handle_queue_attachments_req(ic, is));
        READ_THREAD_LOOP_CALL(read_thread_loop_handle_queue_full(is, wait_mutex));
        READ_THREAD_LOOP_CALL(read_thread_loop_handle_loop(is));

        // READ_THREAD_LOOP_CALL(read_thread_loop_handle_read());
        ret = av_read_frame(ic, pkt);
        if (ret < 0)
        {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof)
            {
                if (is->video_stream >= 0)
                {
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                }

                if (is->audio_stream >= 0)
                {
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                }

                if (is->subtitle_stream >= 0)
                {
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                }

                is->eof = 1;
            }

            if (ic->pb && ic->pb->error)
            {
                break;
            }

            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);

            continue;
        }
        else
        {
            is->eof = 0;
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                                        av_q2d(ic->streams[pkt->stream_index]->time_base) -
                                    (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000 <=
                                ((double)duration / 1000000);

        if (pkt->stream_index == is->audio_stream && pkt_in_play_range)
        {
            packet_queue_put(&is->audioq, pkt);
        }
        else if (pkt->stream_index == is->video_stream && pkt_in_play_range && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
        {
            packet_queue_put(&is->videoq, pkt);
        }
        else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range)
        {
            packet_queue_put(&is->subtitleq, pkt);
        }
        else
        {
            av_packet_unref(pkt);
        }
    }

    ret = 0;

fail:

    SDL_DestroyMutex(wait_mutex);

    return ret;
}

/* 读线程
 * 负责从网络或磁盘中读取数据后放入PacketQueue中
 */
static int read_thread(void *arg)
{
    VideoState *is = arg;

    AVFormatContext *ic = NULL;
    if (open_input_file(&ic, is) != 0)
    {
        goto fail;
    }

    seek_to_start_time(ic, is);

    if (show_status)
    {
        av_dump_format(ic, 0, is->filename, 0);
    }

    int st_index[AVMEDIA_TYPE_NB];

    find_best_streams(ic, st_index);

    update_window_size(ic, st_index[AVMEDIA_TYPE_VIDEO]);

    if (open_the_streams(is, st_index) != 0)
    {
        goto fail;
    }

    int ret = read_thread_loop(ic, is);

fail:

    if (ic && !is->ic)
    {
        avformat_close_input(&ic);
    }

    if (ret != 0)
    {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }

    return 0;
}

static VideoState *stream_open(const char *filename, AVInputFormat *iformat)
{
    VideoState *is = av_mallocz(sizeof(VideoState));
    if (!is)
    {
        return NULL;
    }

    is->filename = av_strdup(filename);
    if (!is->filename)
    {
        goto fail;
    }

    is->iformat = iformat;
    is->ytop = 0;
    is->xleft = 0;

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
    {
        goto fail;
    }

    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
    {
        goto fail;
    }

    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
    {
        goto fail;
    }

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
    {
        goto fail;
    }

    if (!(is->continue_read_thread = SDL_CreateCond()))
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);

    is->audio_clock_serial = -1;
    if (startup_volume < 0)
    {
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    }

    if (startup_volume > 100)
    {
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    }

    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);

    is->audio_volume = startup_volume;
    is->muted = 0;
    is->av_sync_type = av_sync_type;
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());

    fail:
        stream_close(is);
        return NULL;
    }

    return is;
}

static void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic = is->ic;

    int start_index, old_index;

    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO)
    {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    }
    else if (codec_type == AVMEDIA_TYPE_AUDIO)
    {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    }
    else
    {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }

    int stream_index = start_index;

    AVProgram *p;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1)
    {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p)
        {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
            {
                if (p->stream_index[start_index] == stream_index)
                {
                    break;
                }
            }

            if (start_index == nb_streams)
            {
                start_index = -1;
            }

            stream_index = start_index;
        }
    }

    while (1)
    {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }

            if (start_index == -1)
            {
                return;
            }

            stream_index = 0;
        }

        if (stream_index == start_index)
        {
            return;
        }

        AVStream *st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];

        if (st->codecpar->codec_type == codec_type)
        {
            /* check that parameters are OK */
            switch (codec_type)
            {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 && st->codecpar->ch_layout.nb_channels != 0)
                {
                    goto the_end;
                }
                break;

            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;

            default:
                break;
            }
        }
    }

the_end:

    if (p && stream_index != -1)
    {
        stream_index = p->stream_index[stream_index];
    }

    av_log(NULL, AV_LOG_INFO,
           "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(codec_type), old_index, stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}

static void toggle_full_screen(VideoState *is)
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event)
{
    double remaining_time = 0.0;

    SDL_PumpEvents();

    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
    {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY)
        {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }

        if (remaining_time > 0.0)
        {
            av_usleep((int64_t)(remaining_time * 1000000.0));
        }

        remaining_time = REFRESH_RATE;

        if (!is->paused || is->force_refresh)
        {
            video_refresh(is, &remaining_time);
        }

        SDL_PumpEvents();
    }
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;

    if (!is->ic->nb_chapters)
    {
        return;
    }

    int i;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++)
    {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0)
        {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);

    if (i >= is->ic->nb_chapters)
    {
        return;
    }

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base, AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)
{
    SDL_Event event;
    double incr, pos;

    while (1)
    {
        double x;
        refresh_loop_wait_event(cur_stream, &event); // 这里显示画面

        switch (event.type)
        {
        case SDL_KEYDOWN:

            if (exit_on_keydown)
            {
                do_exit(cur_stream);
                break;
            }

            switch (event.key.keysym.sym)
            {
            case SDLK_ESCAPE:
            case SDLK_q:
                do_exit(cur_stream);
                break;

            case SDLK_f:
                toggle_full_screen(cur_stream);
                cur_stream->force_refresh = 1;
                break;

            case SDLK_p:
            case SDLK_SPACE:
                toggle_pause(cur_stream);
                break;

            case SDLK_m:
                toggle_mute(cur_stream);
                break;

            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                break;

            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                break;

            case SDLK_s: // S: Step to next frame
                step_to_next_frame(cur_stream);
                break;

            case SDLK_a:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;

            case SDLK_v:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;

            case SDLK_c:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;

            case SDLK_t:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;

            case SDLK_PAGEUP:

                if (cur_stream->ic->nb_chapters <= 1)
                {
                    incr = 600.0;
                    goto do_seek;
                }

                seek_chapter(cur_stream, 1);
                break;

            case SDLK_PAGEDOWN:
                if (cur_stream->ic->nb_chapters <= 1)
                {
                    incr = -600.0;
                    goto do_seek;
                }

                seek_chapter(cur_stream, -1);
                break;

            case SDLK_LEFT:
                incr = -10.0;
                goto do_seek;

            case SDLK_RIGHT:
                incr = 10.0;
                goto do_seek;

            case SDLK_UP:
                incr = 60.0;
                goto do_seek;

            case SDLK_DOWN:
                incr = -60.0;

            do_seek:
                if (seek_by_bytes)
                {
                    pos = -1;

                    if (pos < 0 && cur_stream->video_stream >= 0)
                    {
                        pos = frame_queue_last_pos(&cur_stream->pictq);
                    }

                    if (pos < 0 && cur_stream->audio_stream >= 0)
                    {
                        pos = frame_queue_last_pos(&cur_stream->sampq);
                    }

                    if (pos < 0)
                    {
                        pos = avio_tell(cur_stream->ic->pb);
                    }

                    if (cur_stream->ic->bit_rate)
                    {
                        incr *= cur_stream->ic->bit_rate / 8.0;
                    }
                    else
                    {
                        incr *= 180000.0;
                    }

                    pos += incr;
                    stream_seek(cur_stream, pos, incr, 1);
                }
                else
                {
                    pos = get_master_clock(cur_stream);
                    if (isnan(pos))
                    {
                        pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                    }

                    pos += incr;
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                    {
                        pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
                    }

                    stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                }
                break;

            default:
                break;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:

            if (exit_on_mousedown)
            {
                do_exit(cur_stream);
                break;
            }

            if (event.button.button == SDL_BUTTON_LEFT)
            {
                static int64_t last_mouse_left_click = 0;

                if (av_gettime_relative() - last_mouse_left_click <= 500000)
                {
                    toggle_full_screen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                }
                else
                {
                    last_mouse_left_click = av_gettime_relative();
                }
            }

        case SDL_MOUSEMOTION:

            if (cursor_hidden)
            {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }

            cursor_last_shown = av_gettime_relative();

            if (event.type == SDL_MOUSEBUTTONDOWN)
            {
                if (event.button.button != SDL_BUTTON_RIGHT)
                {
                    break;
                }

                x = event.button.x;
            }
            else
            {
                if (!(event.motion.state & SDL_BUTTON_RMASK))
                {
                    break;
                }

                x = event.motion.x;
            }

            if (seek_by_bytes || cur_stream->ic->duration <= 0)
            {
                uint64_t size = avio_size(cur_stream->ic->pb);
                stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
            }
            else
            {
                int tns = cur_stream->ic->duration / 1000000LL;
                int thh = tns / 3600;
                int tmm = (tns % 3600) / 60;
                int tss = (tns % 60);

                double frac = x / cur_stream->width;

                int ns = frac * tns;
                int hh = ns / 3600;
                int mm = (ns % 3600) / 60;
                int ss = (ns % 60);

                av_log(NULL, AV_LOG_INFO,
                       "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)\n",
                       frac * 100, hh, mm, ss, thh, tmm, tss);

                int64_t ts = frac * cur_stream->ic->duration;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                {
                    ts += cur_stream->ic->start_time;
                }

                stream_seek(cur_stream, ts, 0, 0);
            }
            break;

        case SDL_WINDOWEVENT:

            switch (event.window.event)
            {
            case SDL_WINDOWEVENT_RESIZED:

                screen_width = cur_stream->width = event.window.data1;
                screen_height = cur_stream->height = event.window.data2;

                if (cur_stream->vis_texture)
                {
                    SDL_DestroyTexture(cur_stream->vis_texture);
                    cur_stream->vis_texture = NULL;
                }

            case SDL_WINDOWEVENT_EXPOSED:
                cur_stream->force_refresh = 1;
            }
            break;

        case SDL_QUIT:
        case FF_QUIT_EVENT:

            do_exit(cur_stream);
            break;

        default:
            break;
        }
    }
}

static void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Simple media player\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

static void show_help_default()
{
    show_usage();
    printf("\n");
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n");
}

static void prepare_sdl()
{
    int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
    {
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }

    if (SDL_Init(flags))
    {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    // av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    window = SDL_CreateWindow(program_name,
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              default_width,
                              default_height,
                              SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    if (window)
    {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer)
        {
            av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (renderer)
        {
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
            {
                av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
        }
    }

    if (!window || !renderer || !renderer_info.num_texture_formats)
    {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        do_exit(NULL);
    }
}

int main(int argc, char **argv)
{
    VideoState *is;

    if (argc < 2)
    {
        show_help_default();
        return -1;
    }

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_DEBUG);

    signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    input_filename = argv[1];

    prepare_sdl();

    is = stream_open(input_filename, file_iformat);
    if (!is)
    {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    event_loop(is);

    /* never returns */

    return 0;
}
