/*****************************************************************************
 * video_output.c
 *****************************************************************************
 * Copyright (C) 2013 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include <string.h>

/* Libav */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "VapourSynth.h"
#include "video_output.h"

static void vs_bit_blt( uint8_t *dst_data, int dst_linesize, uint8_t *src_data, int src_linesize, int row_size, int height )
{
    if( src_linesize == dst_linesize && src_linesize == row_size )
    {
        memcpy( dst_data, src_data, src_linesize * height );
        return;
    }
    for( int i = 0; i < height; i++ )
    {
        memcpy( dst_data, src_data, row_size );
        dst_data += dst_linesize;
        src_data += src_linesize;
    }
}

static void make_black_background_planar_yuv8( VSFrameRef *frame, const VSAPI *vsapi )
{
    for( int i = 0; i < 3; i++ )
        memset( vsapi->getWritePtr( frame, i ), i ? 0x80 : 0x00, vsapi->getStride( frame, i ) * vsapi->getFrameHeight( frame, i ) );
}

static void make_black_background_planar_yuv16( VSFrameRef *frame, const VSAPI *vsapi )
{
    int shift = vsapi->getFrameFormat( frame )->bitsPerSample - 8;
    for( int i = 0; i < 3; i++ )
    {
        int v = i ? 0x00000080 << shift : 0x00000000;
        uint8_t *data = vsapi->getWritePtr( frame, i );
        uint8_t *end  = data + vsapi->getStride( frame, i ) * vsapi->getFrameHeight( frame, i );
        while( data < end )
        {
            /* Assume little endianess. */
            data[0] = v;
            data[1] = v >> 8;
            data += 2;
        }
    }
}

static void make_black_background_planar_rgb( VSFrameRef *frame, const VSAPI *vsapi )
{
    for( int i = 0; i < 3; i++ )
        memset( vsapi->getWritePtr( frame, i ), 0x00, vsapi->getStride( frame, i ) * vsapi->getFrameHeight( frame, i ) );
}

static inline int convert_av_pixel_format( struct SwsContext *sws_ctx, AVFrame *picture, uint8_t *dst_data[4], int dst_linesize[4] )
{
    int64_t dst_format;
    av_opt_get_int( sws_ctx, "dst_format", 0, &dst_format );
    if( av_image_alloc( dst_data, dst_linesize, picture->width, picture->height, dst_format, 16 ) < 0 )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, picture->height, dst_data, dst_linesize );
    return 0;
}

static int make_frame_planar_yuv( struct SwsContext *sws_ctx, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( convert_av_pixel_format( sws_ctx, picture, dst_data, dst_linesize ) < 0 )
    {
        if( frame_ctx )
            vsapi->setFilterError( "lsmas: failed to av_image_alloc.", frame_ctx );
        return -1;
    }
    const VSFormat *format = vsapi->getFrameFormat( frame );
    int row_size_0 = format->bytesPerSample * picture->width;
    for( int i = 0; i < 3; i++ )
    {
        int      frame_linesize = vsapi->getStride  ( frame, i );
        uint8_t *frame_data     = vsapi->getWritePtr( frame, i );
        int      row_size       = row_size_0      >> (i ? format->subSamplingW : 0);
        int      height         = picture->height >> (i ? format->subSamplingH : 0);
        vs_bit_blt( frame_data, frame_linesize, dst_data[i], dst_linesize[i], row_size, height );
    }
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_planar_rgb8( struct SwsContext *sws_ctx, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( convert_av_pixel_format( sws_ctx, picture, dst_data, dst_linesize ) < 0 )
    {
        if( frame_ctx )
            vsapi->setFilterError( "lsmas: failed to av_image_alloc.", frame_ctx );
        return -1;
    }
    uint8_t *frame_data[3] = { vsapi->getWritePtr( frame, 0 ), vsapi->getWritePtr( frame, 1 ), vsapi->getWritePtr( frame, 2 ) };
    int frame_linesize = vsapi->getStride( frame, 0 );
    int frame_offset   = 0;
    int dst_offset     = 0;
    const VSFormat *format = vsapi->getFrameFormat( frame );
    for( int i = 0; i < picture->height; i++ )
    {
        uint8_t *dst_b = dst_data[0] + dst_offset;
        uint8_t *dst_g = dst_b + 1;
        uint8_t *dst_r = dst_g + 1;
        uint8_t *frame_r = frame_data[0] + frame_offset;
        uint8_t *frame_g = frame_data[1] + frame_offset;
        uint8_t *frame_b = frame_data[2] + frame_offset;
        for( int j = 0; j < picture->width; j++ )
        {
            *(frame_r++) = *dst_r;
            *(frame_g++) = *dst_g;
            *(frame_b++) = *dst_b;
            dst_r += format->numPlanes;
            dst_g += format->numPlanes;
            dst_b += format->numPlanes;
        }
        dst_offset   += dst_linesize[0];
        frame_offset += frame_linesize;
    }
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_planar_rgb16( struct SwsContext *sws_ctx, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
{
    uint8_t *dst_data    [4];
    int      dst_linesize[4];
    if( convert_av_pixel_format( sws_ctx, picture, dst_data, dst_linesize ) < 0 )
    {
        if( frame_ctx )
            vsapi->setFilterError( "lsmas: failed to av_image_alloc.", frame_ctx );
        return -1;
    }
    uint8_t *frame_data[3] = { vsapi->getWritePtr( frame, 0 ), vsapi->getWritePtr( frame, 1 ), vsapi->getWritePtr( frame, 2 ) };
    int frame_linesize = vsapi->getStride( frame, 0 );
    int frame_offset   = 0;
    int dst_offset     = 0;
    const VSFormat *format = vsapi->getFrameFormat( frame );
    for( int i = 0; i < picture->height; i++ )
    {
        uint16_t *dst_b = (uint16_t *)(dst_data[0] + dst_offset);
        uint16_t *dst_g = dst_b + 1;
        uint16_t *dst_r = dst_g + 1;
        uint16_t *frame_r = (uint16_t *)(frame_data[0] + frame_offset);
        uint16_t *frame_g = (uint16_t *)(frame_data[1] + frame_offset);
        uint16_t *frame_b = (uint16_t *)(frame_data[2] + frame_offset);
        for( int j = 0; j < picture->width; j++ )
        {
            *(frame_r++) = *dst_r;
            *(frame_g++) = *dst_g;
            *(frame_b++) = *dst_b;
            dst_r += format->numPlanes;
            dst_g += format->numPlanes;
            dst_b += format->numPlanes;
        }
        dst_offset   += dst_linesize[0];
        frame_offset += frame_linesize;
    }
    av_free( dst_data[0] );
    return 0;
}

static void avoid_yuv_scale_conversion( enum AVPixelFormat *input_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat full;
        enum AVPixelFormat limited;
    } range_hack_table[] =
        {
            { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
            { AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P },
            { AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUV440P },
            { AV_PIX_FMT_NONE,     AV_PIX_FMT_NONE    }
        };
    for( int i = 0; range_hack_table[i].full != AV_PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

VSPresetFormat get_vs_output_pixel_format( const char *format_name )
{
    if( !format_name )
        return pfNone;
    static const struct
    {
        const char     *format_name;
        VSPresetFormat  vs_output_pixel_format;
    } format_table[] =
        {
            { "YUV420P8",  pfYUV420P8  },
            { "YUV422P8",  pfYUV422P8  },
            { "YUV444P8",  pfYUV444P8  },
            { "YUV410P8",  pfYUV410P8  },
            { "YUV411P8",  pfYUV411P8  },
            { "YUV440P8",  pfYUV440P8  },
            { "YUV420P9",  pfYUV420P9  },
            { "YUV422P9",  pfYUV422P9  },
            { "YUV444P9",  pfYUV444P9  },
            { "YUV420P10", pfYUV420P10 },
            { "YUV422P10", pfYUV422P10 },
            { "YUV444P10", pfYUV444P10 },
            { "YUV420P16", pfYUV420P16 },
            { "YUV422P16", pfYUV422P16 },
            { "YUV444P16", pfYUV444P16 },
            { "RGB24",     pfRGB24     },
            { "RGB48",     pfRGB48     },
            { NULL,        pfNone      }
        };
    for( int i = 0; format_table[i].format_name; i++ )
        if( strcasecmp( format_name, format_table[i].format_name ) == 0 )
            return format_table[i].vs_output_pixel_format;
    return pfNone;
}

static enum AVPixelFormat vs_to_av_output_pixel_format( VSPresetFormat vs_output_pixel_format )
{
    static const struct
    {
        VSPresetFormat     vs_output_pixel_format;
        enum AVPixelFormat av_output_pixel_format;
    } format_table[] =
        {
            { pfYUV420P8,  AV_PIX_FMT_YUV420P     },
            { pfYUV422P8,  AV_PIX_FMT_YUV422P     },
            { pfYUV444P8,  AV_PIX_FMT_YUV444P     },
            { pfYUV410P8,  AV_PIX_FMT_YUV410P     },
            { pfYUV411P8,  AV_PIX_FMT_YUV411P     },
            { pfYUV440P8,  AV_PIX_FMT_YUV440P     },
            { pfYUV420P9,  AV_PIX_FMT_YUV420P9LE  },
            { pfYUV422P9,  AV_PIX_FMT_YUV422P9LE  },
            { pfYUV444P9,  AV_PIX_FMT_YUV444P9LE  },
            { pfYUV420P10, AV_PIX_FMT_YUV420P10LE },
            { pfYUV422P10, AV_PIX_FMT_YUV422P10LE },
            { pfYUV444P10, AV_PIX_FMT_YUV444P10LE },
            { pfYUV420P16, AV_PIX_FMT_YUV420P16LE },
            { pfYUV422P16, AV_PIX_FMT_YUV422P16LE },
            { pfYUV444P16, AV_PIX_FMT_YUV444P16LE },
            { pfRGB24,     AV_PIX_FMT_BGR24       },
            { pfRGB48,     AV_PIX_FMT_BGR48LE     },
            { pfNone,      AV_PIX_FMT_NONE        }
        };
    for( int i = 0; format_table[i].vs_output_pixel_format != pfNone; i++ )
        if( vs_output_pixel_format == format_table[i].vs_output_pixel_format )
            return format_table[i].av_output_pixel_format;
    return AV_PIX_FMT_NONE;
}

static int set_frame_maker( video_output_handler_t *vohp )
{
    static const struct
    {
        VSPresetFormat              vs_output_pixel_format;
        func_make_black_background *func_make_black_background;
        func_make_frame            *func_make_frame;
    } frame_maker_table[] =
        {
            { pfYUV420P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV422P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV444P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV410P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV411P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV440P8,  make_black_background_planar_yuv8,  make_frame_planar_yuv   },
            { pfYUV420P9,  make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV422P9,  make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV444P9,  make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV420P10, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV422P10, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV444P10, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV420P16, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV422P16, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfYUV444P16, make_black_background_planar_yuv16, make_frame_planar_yuv   },
            { pfRGB24,     make_black_background_planar_rgb,   make_frame_planar_rgb8  },
            { pfRGB48,     make_black_background_planar_rgb,   make_frame_planar_rgb16 },
            { pfNone,      NULL,                               NULL                  }
        };
    for( int i = 0; frame_maker_table[i].vs_output_pixel_format != pfNone; i++ )
        if( vohp->vs_output_pixel_format == frame_maker_table[i].vs_output_pixel_format )
        {
            vohp->make_black_background = frame_maker_table[i].func_make_black_background;
            vohp->make_frame            = frame_maker_table[i].func_make_frame;
            return 0;
        }
    vohp->make_black_background = NULL;
    vohp->make_frame            = NULL;
    return -1;
}

int determine_colorspace_conversion( video_output_handler_t *vohp, enum AVPixelFormat *input_pixel_format )
{
    avoid_yuv_scale_conversion( input_pixel_format );
    if( vohp->variable_info || vohp->vs_output_pixel_format == pfNone )
    {
        /* Determine by input pixel format. */
        static const struct
        {
            enum AVPixelFormat av_input_pixel_format;
            VSPresetFormat     vs_output_pixel_format;
        } conversion_table[] =
            {
                { AV_PIX_FMT_YUV420P,     pfYUV420P8  },
                { AV_PIX_FMT_NV12,        pfYUV420P8  },
                { AV_PIX_FMT_NV21,        pfYUV420P8  },
                { AV_PIX_FMT_YUV422P,     pfYUV422P8  },
                { AV_PIX_FMT_UYVY422,     pfYUV422P8  },
                { AV_PIX_FMT_YUYV422,     pfYUV422P8  },
                { AV_PIX_FMT_YUV444P,     pfYUV444P8  },
                { AV_PIX_FMT_YUV410P,     pfYUV410P8  },
                { AV_PIX_FMT_YUV411P,     pfYUV411P8  },
                { AV_PIX_FMT_UYYVYY411,   pfYUV411P8  },
                { AV_PIX_FMT_YUV440P,     pfYUV440P8  },
                { AV_PIX_FMT_YUV420P9LE,  pfYUV420P9  },
                { AV_PIX_FMT_YUV420P9BE,  pfYUV420P9  },
                { AV_PIX_FMT_YUV422P9LE,  pfYUV422P9  },
                { AV_PIX_FMT_YUV422P9BE,  pfYUV422P9  },
                { AV_PIX_FMT_YUV444P9LE,  pfYUV444P9  },
                { AV_PIX_FMT_YUV444P9BE,  pfYUV444P9  },
                { AV_PIX_FMT_YUV420P10LE, pfYUV420P10 },
                { AV_PIX_FMT_YUV420P10BE, pfYUV420P10 },
                { AV_PIX_FMT_YUV422P10LE, pfYUV422P10 },
                { AV_PIX_FMT_YUV422P10BE, pfYUV422P10 },
                { AV_PIX_FMT_YUV444P10LE, pfYUV444P10 },
                { AV_PIX_FMT_YUV444P10BE, pfYUV444P10 },
                { AV_PIX_FMT_YUV420P16LE, pfYUV420P16 },
                { AV_PIX_FMT_YUV420P16BE, pfYUV420P16 },
                { AV_PIX_FMT_YUV422P16LE, pfYUV422P16 },
                { AV_PIX_FMT_YUV422P16BE, pfYUV422P16 },
                { AV_PIX_FMT_YUV444P16LE, pfYUV444P16 },
                { AV_PIX_FMT_YUV444P16BE, pfYUV444P16 },
                { AV_PIX_FMT_BGR24,       pfRGB24     },
                { AV_PIX_FMT_RGB24,       pfRGB24     },
                { AV_PIX_FMT_BGR48LE,     pfRGB48     },
                { AV_PIX_FMT_BGR48BE,     pfRGB48     },
                { AV_PIX_FMT_NONE,        pfNone      }
            };
        for( int i = 0; conversion_table[i].vs_output_pixel_format != pfNone; i++ )
            if( *input_pixel_format == conversion_table[i].av_input_pixel_format )
            {
                vohp->vs_output_pixel_format = conversion_table[i].vs_output_pixel_format;
                break;
            }
    }
    vohp->av_output_pixel_format = vs_to_av_output_pixel_format( vohp->vs_output_pixel_format );
    return set_frame_maker( vohp );
}

int make_frame( video_output_handler_t *vohp, AVFrame *picture, VSFrameRef *frame, VSFrameContext *frame_ctx, const VSAPI *vsapi )
{
    /* Convert color space. We don't change the presentation resolution. */
    int64_t width;
    int64_t height;
    int64_t format;
    av_opt_get_int( vohp->sws_ctx, "srcw",       0, &width );
    av_opt_get_int( vohp->sws_ctx, "srch",       0, &height );
    av_opt_get_int( vohp->sws_ctx, "src_format", 0, &format );
    avoid_yuv_scale_conversion( (enum AVPixelFormat *)&picture->format );
    if( !vohp->sws_ctx || picture->width != width || picture->height != height || picture->format != format )
    {
        /* Update scaler. */
        vohp->sws_ctx = sws_getCachedContext( vohp->sws_ctx,
                                              picture->width, picture->height, (enum AVPixelFormat)picture->format,
                                              picture->width, picture->height, vohp->av_output_pixel_format,
                                              vohp->scaler_flags, NULL, NULL, NULL );
        if( !vohp->sws_ctx )
        {
            if( frame_ctx )
                vsapi->setFilterError( "lsmas: failed to update scaler settings.", frame_ctx );
            return -1;
        }
    }
    if( !vohp->make_frame )
        return -1;
    if( vohp->make_black_background && !vohp->variable_info )
        vohp->make_black_background( frame, vsapi );
    return vohp->make_frame( vohp->sws_ctx, picture, frame, frame_ctx, vsapi );
}