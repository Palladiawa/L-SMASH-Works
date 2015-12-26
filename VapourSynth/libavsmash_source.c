/*****************************************************************************
 * libavsmash_source.c
 *****************************************************************************
 * Copyright (C) 2013-2015 L-SMASH Works project
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

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL. */

/* L-SMASH (ISC) */
#include <lsmash.h>                 /* Demuxer */

/* Libav (LGPL or GPL) */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>

#include "lsmashsource.h"
#include "video_output.h"

#include "../common/libavsmash.h"
#include "../common/libavsmash_video.h"

typedef struct
{
    VSVideoInfo                        vi;
    libavsmash_video_decode_handler_t *vdhp;
    libavsmash_video_output_handler_t *vohp;
    lsmash_file_parameters_t           file_param;
    AVFormatContext                   *format_ctx;
    char preferred_decoder_names_buf[PREFERRED_DECODER_NAMES_BUFSIZE];
} lsmas_handler_t;

/* Deallocate the handler of this plugin. */
static void free_handler
(
    lsmas_handler_t **hpp
)
{
    if( !hpp || !*hpp )
        return;
    lsmas_handler_t *hp = *hpp;
    lsmash_root_t *root = libavsmash_video_get_root( hp->vdhp );
    lw_free( libavsmash_video_get_preferred_decoder_names( hp->vdhp ) );
    libavsmash_video_free_decode_handler( hp->vdhp );
    libavsmash_video_free_output_handler( hp->vohp );
    avformat_close_input( &hp->format_ctx );
    lsmash_close_file( &hp->file_param );
    lsmash_destroy_root( root );
    lw_free( hp );
}

/* Allocate the handler of this plugin. */
static lsmas_handler_t *alloc_handler
(
    void
)
{
    lsmas_handler_t *hp = lw_malloc_zero( sizeof(lsmas_handler_t) );
    if( !hp )
        return NULL;
    hp->vdhp = libavsmash_video_alloc_decode_handler();
    if( !hp->vdhp )
    {
        free_handler( &hp );
        return NULL;
    }
    hp->vohp = libavsmash_video_alloc_output_handler();
    if( !hp->vohp )
    {
        free_handler( &hp );
        return NULL;
    }
    return hp;
}

static void VS_CC vs_filter_init( VSMap *in, VSMap *out, void **instance_data, VSNode *node, VSCore *core, const VSAPI *vsapi )
{
    lsmas_handler_t *hp = (lsmas_handler_t *)*instance_data;
    vsapi->setVideoInfo( &hp->vi, 1, node );
}

static int get_composition_duration
(
    libavsmash_video_decode_handler_t *vdhp,
    uint32_t                           composition_sample_number,
    uint32_t                           last_sample_number
)
{
    uint32_t coded_sample_number = libavsmash_video_get_coded_sample_number( vdhp, composition_sample_number );
    if( composition_sample_number == last_sample_number )
        goto no_composition_duration;
    uint32_t next_coded_sample_number = libavsmash_video_get_coded_sample_number( vdhp, composition_sample_number + 1 );
    uint64_t      cts;
    uint64_t next_cts;
    if( libavsmash_video_get_cts( vdhp,      coded_sample_number,      &cts ) < 0
     || libavsmash_video_get_cts( vdhp, next_coded_sample_number, &next_cts ) < 0 )
        goto no_composition_duration;
    if( next_cts <= cts || (next_cts - cts) > INT_MAX )
        return 0;
    return (int)(next_cts - cts);
no_composition_duration:;
    uint32_t sample_duration;
    if( libavsmash_video_get_sample_duration( vdhp, coded_sample_number, &sample_duration ) < 0 )
        return 0;
    return sample_duration <= INT_MAX ? sample_duration : 0;
}

static void set_sample_duration
(
    libavsmash_video_decode_handler_t *vdhp,
    VSVideoInfo                       *vi,
    VSMap                             *props,
    uint32_t                           sample_number,
    const VSAPI                       *vsapi
)
{
    int sample_duration = get_composition_duration( vdhp, sample_number, vi->numFrames );
    if( sample_duration == 0 )
    {
        vsapi->propSetInt( props, "_DurationNum", vi->fpsDen,      paReplace );
        vsapi->propSetInt( props, "_DurationDen", vi->fpsNum,      paReplace );
    }
    else
    {
        uint32_t media_timescale = libavsmash_video_get_media_timescale( vdhp );
        vsapi->propSetInt( props, "_DurationNum", sample_duration, paReplace );
        vsapi->propSetInt( props, "_DurationDen", media_timescale, paReplace );
    }
}

static void set_frame_properties
(
    libavsmash_video_decode_handler_t *vdhp,
    VSVideoInfo                       *vi,
    AVFrame                           *av_frame,
    VSFrameRef                        *vs_frame,
    uint32_t                           sample_number,
    const VSAPI                       *vsapi
)
{
    AVCodecContext *ctx   = libavsmash_video_get_codec_context( vdhp );
    VSMap          *props = vsapi->getFramePropsRW( vs_frame );
    /* Sample duration */
    set_sample_duration( vdhp, vi, props, sample_number, vsapi );
    /* Sample aspect ratio */
    vsapi->propSetInt( props, "_SARNum", av_frame->sample_aspect_ratio.num, paReplace );
    vsapi->propSetInt( props, "_SARDen", av_frame->sample_aspect_ratio.den, paReplace );
    /* Color format */
    if( ctx )
    {
        vsapi->propSetInt( props, "_ColorRange",  ctx->color_range != AVCOL_RANGE_JPEG, paReplace );
        vsapi->propSetInt( props, "_Primaries",   ctx->color_primaries,                 paReplace );
        vsapi->propSetInt( props, "_Transfer",    ctx->color_trc,                       paReplace );
        vsapi->propSetInt( props, "_Matrix",      ctx->colorspace,                      paReplace );
        int chroma_loc;
        switch( ctx->chroma_sample_location )
        {
            case AVCHROMA_LOC_LEFT       : chroma_loc = 0;  break;
            case AVCHROMA_LOC_CENTER     : chroma_loc = 1;  break;
            case AVCHROMA_LOC_TOPLEFT    : chroma_loc = 2;  break;
            case AVCHROMA_LOC_TOP        : chroma_loc = 3;  break;
            case AVCHROMA_LOC_BOTTOMLEFT : chroma_loc = 4;  break;
            case AVCHROMA_LOC_BOTTOM     : chroma_loc = 5;  break;
            default                      : chroma_loc = -1; break;
        }
        if( chroma_loc != -1 )
            vsapi->propSetInt( props, "_ChromaLocation", chroma_loc, paReplace );
    }
    /* Picture type */
    char pict_type = av_get_picture_type_char( av_frame->pict_type );
    vsapi->propSetData( props, "_PictType", &pict_type, 1, paReplace );
    /* BFF or TFF */
    if( av_frame->interlaced_frame )
        vsapi->propSetInt( props, "_FieldBased", av_frame->top_field_first ? 2 : 1, paReplace );
}

static int prepare_video_decoding
(
    lsmas_handler_t *hp,
    VSCore          *core,
    const VSAPI     *vsapi
)
{
    libavsmash_video_decode_handler_t *vdhp = hp->vdhp;
    libavsmash_video_output_handler_t *vohp = hp->vohp;
    VSVideoInfo                       *vi   = &hp->vi;
    lw_log_handler_t                  *lhp  = libavsmash_video_get_log_handler( vdhp );
    (void)libavsmash_video_fetch_sample_count   ( vdhp );
    (void)libavsmash_video_fetch_media_duration ( vdhp );
    (void)libavsmash_video_fetch_media_timescale( vdhp );
    /* Initialize the video decoder configuration. */
    if( libavsmash_video_initialize_decoder_configuration( vdhp ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to initialize the decoder configuration." );
        return -1;
    }
    /* Set up output format. */
    AVCodecContext *ctx = libavsmash_video_get_codec_context( vdhp );
    if( determine_colorspace_conversion( vohp, ctx->pix_fmt ) )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: %s is not supported", av_get_pix_fmt_name( ctx->pix_fmt ) );
        return -1;
    }
    if( initialize_scaler_handler( &vohp->scaler, ctx, vohp->scaler.enabled, SWS_FAST_BILINEAR, vohp->scaler.output_pixel_format ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to initialize scaler handler." );
        return -1;
    }
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = NULL;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    int max_width  = libavsmash_video_get_max_width ( vdhp );
    int max_height = libavsmash_video_get_max_height( vdhp );
    int (*get_buffer_func)( struct AVCodecContext *, AVFrame *, int ) = setup_video_rendering( vohp, ctx, vi, max_width, max_height );
    if( !get_buffer_func )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate memory for the background black frame data." );
        return -1;
    }
    libavsmash_video_set_get_buffer_func( vdhp, get_buffer_func );
    /* Calculate average framerate. */
    int64_t fps_num = 25;
    int64_t fps_den = 1;
    libavsmash_video_setup_timestamp_info( vdhp, vohp, &fps_num, &fps_den );
    if( vohp->vfr2cfr )
    {
        if( libavsmash_video_get_error( vdhp ) )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get the minimum CTS of video stream." );
            return -1;
        }
    }
    else
        libavsmash_video_clear_error( vdhp );
    /* Find the first valid video sample. */
    if( libavsmash_video_find_first_valid_frame( vdhp ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate the first valid video frame." );
        return -1;
    }
    /* Setup filter specific info. */
    hp->vi.fpsNum    = fps_num;
    hp->vi.fpsDen    = fps_den;
    hp->vi.numFrames = vohp->frame_count;
    /* Force seeking at the first reading. */
    libavsmash_video_force_seek( vdhp );
    return 0;
}

static const VSFrameRef *VS_CC vs_filter_get_frame( int n, int activation_reason, void **instance_data, void **frame_data, VSFrameContext *frame_ctx, VSCore *core, const VSAPI *vsapi )
{
    if( activation_reason != arInitial )
        return NULL;
    lsmas_handler_t *hp = (lsmas_handler_t *)*instance_data;
    VSVideoInfo     *vi = &hp->vi;
    uint32_t sample_number = MIN( n + 1, vi->numFrames );   /* For L-SMASH, sample_number is 1-origin. */
    libavsmash_video_decode_handler_t *vdhp = hp->vdhp;
    libavsmash_video_output_handler_t *vohp = hp->vohp;
    if( libavsmash_video_get_error( vdhp ) )
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = NULL;
    vsbh.frame_ctx = frame_ctx;
    vsbh.vsapi     = vsapi;
    lw_log_handler_t *lhp = libavsmash_video_get_log_handler( vdhp );
    lhp->priv     = &vsbh;
    lhp->show_log = set_error;
    /* Get and decode the desired video frame. */
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = frame_ctx;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    if( libavsmash_video_get_frame( vdhp, vohp, sample_number ) < 0 )
        return NULL;
    /* Output video frame. */
    AVFrame    *av_frame = libavsmash_video_get_frame_buffer( vdhp );
    VSFrameRef *vs_frame = make_frame( vohp, libavsmash_video_get_codec_context( vdhp ), av_frame );
    if( !vs_frame )
    {
        vsapi->setFilterError( "lsmas: failed to output a video frame.", frame_ctx );
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    }
    set_frame_properties( vdhp, vi, av_frame, vs_frame, sample_number, vsapi );
    return vs_frame;
}

static void VS_CC vs_filter_free( void *instance_data, VSCore *core, const VSAPI *vsapi )
{
    free_handler( (lsmas_handler_t **)&instance_data );
}

static uint32_t open_file
(
    lsmas_handler_t  *hp,
    const char       *source,
    lw_log_handler_t *lhp
)
{
    lsmash_movie_parameters_t movie_param;
    lsmash_root_t *root = libavsmash_open_file( &hp->format_ctx, source, &hp->file_param, &movie_param, lhp );
    if( !root )
        return 0;
    libavsmash_video_set_root( hp->vdhp, root );
    return movie_param.number_of_tracks;
}

static int get_video_track
(
    lsmas_handler_t *hp,
    uint32_t         track_number,
    int              threads,
    uint32_t         number_of_tracks
)
{
    libavsmash_video_decode_handler_t *vdhp = hp->vdhp;
    lw_log_handler_t                  *lhp  = libavsmash_video_get_log_handler( vdhp );
    lsmash_root_t                     *root = libavsmash_video_get_root( vdhp );
    /* L-SMASH */
    uint32_t i;
    uint32_t track_id;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first video track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            track_id = lsmash_get_track_ID( root, i );
            if( track_id == 0 )
            {
                set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find video track." );
                return -1;
            }
            libavsmash_video_set_track_id( vdhp, track_id );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( root, track_id, &media_param ) < 0 )
            {
                set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get media parameters." );
                return -1;
            }
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find video track." );
            return -1;
        }
    }
    else
    {
        /* Get the desired video track. */
        track_id = lsmash_get_track_ID( root, track_number );
        if( track_id == 0 )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find video track %"PRIu32".", track_number );
            return -1;
        }
        libavsmash_video_set_track_id( vdhp, track_id );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( root, track_id, &media_param ) < 0 )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get media parameters." );
            return -1;
        }
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
        {
            set_error( lhp, LW_LOG_FATAL, "lsmas: the track you specified is not a video track." );
            return -1;
        }
    }
    if( lsmash_construct_timeline( root, track_id ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to get construct timeline." );
        return -1;
    }
    if( libavsmash_video_get_summaries( vdhp ) < 0 )
        return -1;
    /* libavformat */
    for( i = 0; i < hp->format_ctx->nb_streams && hp->format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == hp->format_ctx->nb_streams )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find stream by libavformat." );
        return -1;
    }
    /* libavcodec */
    AVStream       *stream = hp->format_ctx->streams[i];
    AVCodecContext *ctx    = stream->codec;
    libavsmash_video_set_codec_context( vdhp, ctx );
    AVCodec *codec = libavsmash_video_find_decoder( vdhp );
    if( !codec )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to find %s decoder.", codec->name );
        return -1;
    }
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to avcodec_open2." );
        return -1;
    }
    return 0;
}

void VS_CC vs_libavsmashsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi )
{
    /* Get file name. */
    const char *file_name = vsapi->propGetData( in, "source", 0, NULL );
    if( !file_name )
    {
        vsapi->setError( out, "lsmas: failed to get source file name." );
        return;
    }
    /* Allocate the handler of this plugin. */
    lsmas_handler_t *hp = alloc_handler();
    if( !hp )
    {
        vsapi->setError( out, "lsmas: failed to allocate the handler." );
        return;
    }
    libavsmash_video_decode_handler_t *vdhp = hp->vdhp;
    libavsmash_video_output_handler_t *vohp = hp->vohp;
    vs_video_output_handler_t *vs_vohp = vs_allocate_video_output_handler( vohp );
    if( !vs_vohp )
    {
        free_handler( &hp );
        vsapi->setError( out, "lsmas: failed to allocate the VapourSynth video output handler." );
        return;
    }
    vohp->private_handler      = vs_vohp;
    vohp->free_private_handler = lw_free;
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = out;
    vsbh.frame_ctx = NULL;
    vsbh.vsapi     = vsapi;
    /* Set up log handler. */
    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_FATAL;
    lh.priv     = &vsbh;
    lh.show_log = set_error;
    /* Open source file. */
    uint32_t number_of_tracks = open_file( hp, file_name, &lh );
    if( number_of_tracks == 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Get options. */
    int64_t track_number;
    int64_t threads;
    int64_t seek_mode;
    int64_t seek_threshold;
    int64_t variable_info;
    int64_t direct_rendering;
    int64_t fps_num;
    int64_t fps_den;
    const char *format;
    const char *preferred_decoder_names;
    set_option_int64 ( &track_number,            0,    "track",          in, vsapi );
    set_option_int64 ( &threads,                 0,    "threads",        in, vsapi );
    set_option_int64 ( &seek_mode,               0,    "seek_mode",      in, vsapi );
    set_option_int64 ( &seek_threshold,          10,   "seek_threshold", in, vsapi );
    set_option_int64 ( &variable_info,           0,    "variable",       in, vsapi );
    set_option_int64 ( &direct_rendering,        0,    "dr",             in, vsapi );
    set_option_int64 ( &fps_num,                 0,    "fpsnum",         in, vsapi );
    set_option_int64 ( &fps_den,                 1,    "fpsden",         in, vsapi );
    set_option_string( &format,                  NULL, "format",         in, vsapi );
    set_option_string( &preferred_decoder_names, NULL, "decoder",        in, vsapi );
    set_preferred_decoder_names_on_buf( hp->preferred_decoder_names_buf, preferred_decoder_names );
    libavsmash_video_set_seek_mode              ( vdhp, CLIP_VALUE( seek_mode,      0, 2 ) );
    libavsmash_video_set_forward_seek_threshold ( vdhp, CLIP_VALUE( seek_threshold, 1, 999 ) );
    libavsmash_video_set_preferred_decoder_names( vdhp, tokenize_preferred_decoder_names( hp->preferred_decoder_names_buf ) );
    vohp->vfr2cfr = (fps_num > 0 && fps_den > 0);
    vohp->cfr_num = (uint32_t)fps_num;
    vohp->cfr_den = (uint32_t)fps_den;
    vs_vohp->variable_info               = CLIP_VALUE( variable_info,  0, 1 );
    vs_vohp->direct_rendering            = CLIP_VALUE( direct_rendering,  0, 1 ) && !format;
    vs_vohp->vs_output_pixel_format = vs_vohp->variable_info ? pfNone : get_vs_output_pixel_format( format );
    if( track_number && track_number > number_of_tracks )
    {
        vs_filter_free( hp, core, vsapi );
        set_error( &lh, LW_LOG_FATAL, "lsmas: the number of tracks equals %"PRIu32".", number_of_tracks );
        return;
    }
    /* Get video track. */
    threads = threads >= 0 ? threads : 0;
    if( get_video_track( hp, track_number, threads, number_of_tracks ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Set up decoders for this track. */
    libavsmash_video_set_log_handler( vdhp, &lh );
    if( prepare_video_decoding( hp, core, vsapi ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    lsmash_discard_boxes( libavsmash_video_get_root( vdhp ) );
    vsapi->createFilter( in, out, "LibavSMASHSource", vs_filter_init, vs_filter_get_frame, vs_filter_free, fmSerial, 0, hp, core );
    return;
}
