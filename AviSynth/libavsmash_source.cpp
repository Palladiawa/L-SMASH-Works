/*****************************************************************************
 * libavsmash_source.cpp
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
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

#include "lsmashsource.h"

extern "C"
{
/* L-SMASH */
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "video_output.h"
#include "audio_output.h"
#include "libavsmash_source.h"

static const char func_name_video_source[] = "LSMASHVideoSource";
static const char func_name_audio_source[] = "LSMASHAudioSource";

uint32_t LSMASHVideoSource::open_file
(
    const char                        *source,
    IScriptEnvironment                *env
)
{
    libavsmash_video_decode_handler_t *vdhp = this->vdhp.get();
    lw_log_handler_t *lhp = libavsmash_video_get_log_handler( vdhp );
    lhp->name     = func_name_video_source;
    lhp->level    = LW_LOG_FATAL;
    lhp->show_log = throw_error;
    lsmash_movie_parameters_t movie_param;
    AVFormatContext *format_ctx = nullptr;
    lsmash_root_t *root = libavsmash_open_file( &format_ctx, source, &file_param, &movie_param, lhp );
    this->format_ctx.reset( format_ctx );
    libavsmash_video_set_root( vdhp, root );
    return movie_param.number_of_tracks;
}

void LSMASHVideoSource::get_video_track
(
    const char                        *source,
    uint32_t                           track_number,
    int                                threads,
    IScriptEnvironment                *env
)
{
    libavsmash_video_decode_handler_t *vdhp = this->vdhp.get();
    libavsmash_video_output_handler_t *vohp = this->vohp.get();
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    lsmash_root_t *root = libavsmash_video_get_root( vdhp );
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
                env->ThrowError( "LSMASHVideoSource: failed to find video track." );
            libavsmash_video_set_track_id( vdhp, track_id );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( root, track_id, &media_param ) )
                env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
    }
    else
    {
        /* Get the desired video track. */
        track_id = lsmash_get_track_ID( root, track_number );
        if( track_id == 0 )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
        libavsmash_video_set_track_id( vdhp, track_id );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( root, track_id, &media_param ) )
            env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            env->ThrowError( "LSMASHVideoSource: the track you specified is not a video track." );
    }
    if( lsmash_construct_timeline( root, track_id ) )
        env->ThrowError( "LSMASHVideoSource: failed to get construct timeline." );
    if( libavsmash_video_get_summaries( vdhp ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to get summaries." );
    /* libavformat */
    AVFormatContext *format_ctx = this->format_ctx.get();
    for( i = 0; i < format_ctx->nb_streams && format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == format_ctx->nb_streams )
        env->ThrowError( "LSMASHVideoSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream       *stream = format_ctx->streams[i];
    AVCodecContext *ctx    = stream->codec;
    libavsmash_video_set_codec_context( vdhp, ctx );
    AVCodec *codec = libavsmash_video_find_decoder( vdhp );
    if( !codec )
        env->ThrowError( "LSMASHVideoSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, nullptr ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avcodec_open2." );
}

static void prepare_video_decoding
(
    libavsmash_video_decode_handler_t *vdhp,
    libavsmash_video_output_handler_t *vohp,
    int                                direct_rendering,
    int                                stacked_format,
    enum AVPixelFormat                 pixel_format,
    VideoInfo                         &vi,
    IScriptEnvironment                *env
)
{
    /* Initialize the video decoder configuration. */
    lw_log_handler_t *lhp = libavsmash_video_get_log_handler( vdhp );
    lhp->priv = env;
    uint32_t sample_count    = libavsmash_video_fetch_sample_count   ( vdhp );
    uint64_t media_duration  = libavsmash_video_fetch_media_duration ( vdhp );
    uint32_t media_timescale = libavsmash_video_fetch_media_timescale( vdhp );
    if( libavsmash_video_initialize_decoder_configuration( vdhp ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to initialize the decoder configuration." );
    /* Set up output format. */
    AVCodecContext *ctx = libavsmash_video_get_codec_context( vdhp );
    int max_width  = libavsmash_video_get_max_width ( vdhp );
    int max_height = libavsmash_video_get_max_height( vdhp );
    int (*get_buffer_func)( struct AVCodecContext *, AVFrame *, int ) =
        as_setup_video_rendering( vohp, ctx, "LSMASHVideoSource",
                                  direct_rendering, stacked_format, pixel_format,
                                  max_width, max_height );
    libavsmash_video_set_get_buffer_func( vdhp, get_buffer_func );
    /* Calculate average framerate. */
    int64_t fps_num = 25;
    int64_t fps_den = 1;
    libavsmash_video_setup_timestamp_info( vdhp, vohp, &fps_num, &fps_den );
    if( vohp->vfr2cfr )
    {
        if( libavsmash_video_get_error( vdhp ) )
            env->ThrowError( "LSMASHVideoSource: failed to get the minimum CTS of video stream." );
    }
    else
        libavsmash_video_clear_error( vdhp );
    /* Find the first valid video sample. */
    if( libavsmash_video_find_first_valid_frame( vdhp ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to find the first valid video frame." );
    /* Setup filter specific info. */
    vi.fps_numerator   = (unsigned int)fps_num;
    vi.fps_denominator = (unsigned int)fps_den;
    vi.num_frames      = vohp->frame_count;
    /* Force seeking at the first reading. */
    libavsmash_video_force_seek( vdhp );
}

LSMASHVideoSource::LSMASHVideoSource
(
    const char         *source,
    uint32_t            track_number,
    int                 threads,
    int                 seek_mode,
    uint32_t            forward_seek_threshold,
    int                 direct_rendering,
    int                 fps_num,
    int                 fps_den,
    int                 stacked_format,
    enum AVPixelFormat  pixel_format,
    const char         *preferred_decoder_names,
    IScriptEnvironment *env
) : LSMASHVideoSource{}
{
    memset( &vi,  0, sizeof(VideoInfo) );
    libavsmash_video_decode_handler_t *vdhp = this->vdhp.get();
    libavsmash_video_output_handler_t *vohp = this->vohp.get();
    set_preferred_decoder_names( preferred_decoder_names );
    libavsmash_video_set_seek_mode              ( vdhp, seek_mode );
    libavsmash_video_set_forward_seek_threshold ( vdhp, forward_seek_threshold );
    libavsmash_video_set_preferred_decoder_names( vdhp, tokenize_preferred_decoder_names() );
    vohp->vfr2cfr = (fps_num > 0 && fps_den > 0);
    vohp->cfr_num = (uint32_t)fps_num;
    vohp->cfr_den = (uint32_t)fps_den;
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_malloc_zero( sizeof(as_video_output_handler_t) );
    if( as_vohp == nullptr )
        env->ThrowError( "LSMASHVideoSource: failed to allocate the AviSynth video output handler." );
    as_vohp->vi  = &vi;
    as_vohp->env = env;
    vohp->private_handler      = as_vohp;
    vohp->free_private_handler = as_free_video_output_handler;
    get_video_track( source, track_number, threads, env );
    prepare_video_decoding( vdhp, vohp, direct_rendering, stacked_format, pixel_format, vi, env );
    lsmash_discard_boxes( libavsmash_video_get_root( vdhp ) );
}

LSMASHVideoSource::~LSMASHVideoSource()
{
    libavsmash_video_decode_handler_t *vdhp = this->vdhp.get();
    lsmash_root_t *root = libavsmash_video_get_root( vdhp );
    lw_free( libavsmash_video_get_preferred_decoder_names( vdhp ) );
    lsmash_close_file( &file_param );
    lsmash_destroy_root( root );
}

PVideoFrame __stdcall LSMASHVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    libavsmash_video_decode_handler_t *vdhp = this->vdhp.get();
    libavsmash_video_output_handler_t *vohp = this->vohp.get();
    lw_log_handler_t *lhp = libavsmash_video_get_log_handler( vdhp );
    lhp->priv = env;
    if( libavsmash_video_get_error( vdhp )
     || libavsmash_video_get_frame( vdhp, vohp, sample_number ) < 0 )
        return env->NewVideoFrame( vi );
    AVCodecContext *ctx      = libavsmash_video_get_codec_context( vdhp );
    AVFrame        *av_frame = libavsmash_video_get_frame_buffer ( vdhp );
    PVideoFrame     as_frame;
    if( make_frame( vohp, ctx, av_frame, as_frame, env ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to make a frame." );
    return as_frame;
}

uint32_t LSMASHAudioSource::open_file( const char *source, IScriptEnvironment *env )
{
    libavsmash_audio_decode_handler_t *adhp = this->adhp.get();
    lw_log_handler_t *lhp = libavsmash_audio_get_log_handler( adhp );
    lhp->name     = func_name_audio_source;
    lhp->level    = LW_LOG_FATAL;
    lhp->show_log = throw_error;
    lsmash_movie_parameters_t movie_param;
    AVFormatContext *format_ctx = nullptr;
    lsmash_root_t *root = libavsmash_open_file( &format_ctx, source, &file_param, &movie_param, lhp );
    this->format_ctx.reset( format_ctx );
    libavsmash_audio_set_root( adhp, root );
    return movie_param.number_of_tracks;
}

static int64_t get_start_time( lsmash_root_t *root, uint32_t track_ID )
{
    /* Consider start time of this media if any non-empty edit is present. */
    uint32_t edit_count = lsmash_count_explicit_timeline_map( root, track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( root, track_ID, edit_number, &edit ) )
            return 0;
        if( edit.duration == 0 )
            return 0;   /* no edits */
        if( edit.start_time >= 0 )
            return edit.start_time;
    }
    return 0;
}

static char *duplicate_as_string( void *src, size_t length )
{
    char *dst = new char[length + 1];
    if( !dst )
        return nullptr;
    memcpy( dst, src, length );
    dst[length] = '\0';
    return dst;
}

void LSMASHAudioSource::get_audio_track( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env )
{
    libavsmash_audio_decode_handler_t *adhp = this->adhp.get();
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    lsmash_root_t *root = libavsmash_audio_get_root( adhp );
    uint32_t i;
    uint32_t track_id;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first audio track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            track_id = lsmash_get_track_ID( root, i );
            if( track_id == 0 )
                env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
            libavsmash_audio_set_track_id( adhp, track_id );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( root, track_id, &media_param ) )
                env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
    }
    else
    {
        /* Get the desired audio track. */
        track_id = lsmash_get_track_ID( root, track_number );
        if( track_id == 0 )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
            libavsmash_audio_set_track_id( adhp, track_id );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( root, track_id, &media_param ) )
            env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            env->ThrowError( "LSMASHAudioSource: the track you specified is not an audio track." );
    }
    if( lsmash_construct_timeline( root, track_id ) )
        env->ThrowError( "LSMASHAudioSource: failed to get construct timeline." );
     if( libavsmash_audio_get_summaries( adhp ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to get summaries." );
    (void)libavsmash_audio_fetch_sample_count( adhp );
    vi.num_audio_samples = lsmash_get_media_duration_from_media_timeline( root, track_id );
    if( skip_priming )
    {
        libavsmash_audio_output_handler_t *aohp = this->aohp.get();
        uint32_t itunes_metadata_count = lsmash_count_itunes_metadata( root );
        for( i = 1; i <= itunes_metadata_count; i++ )
        {
            lsmash_itunes_metadata_t metadata;
            if( lsmash_get_itunes_metadata( root, i, &metadata ) < 0 )
                continue;
            if( metadata.item != ITUNES_METADATA_ITEM_CUSTOM
             || (metadata.type != ITUNES_METADATA_TYPE_STRING && metadata.type != ITUNES_METADATA_TYPE_BINARY)
             || !metadata.meaning || !metadata.name
             || memcmp( "com.apple.iTunes", metadata.meaning, strlen( metadata.meaning ) )
             || memcmp( "iTunSMPB", metadata.name, strlen( metadata.name ) ) )
            {
                lsmash_cleanup_itunes_metadata( &metadata );
                continue;
            }
            char *value = nullptr;
            if( metadata.type == ITUNES_METADATA_TYPE_STRING )
            {
                int length = strlen( metadata.value.string );
                if( length >= 116 )
                    value = duplicate_as_string( metadata.value.string, length );
            }
            else    /* metadata.type == ITUNES_METADATA_TYPE_BINARY */
            {
                if( metadata.value.binary.size >= 116 )
                    value = duplicate_as_string( metadata.value.binary.data, metadata.value.binary.size );
            }
            lsmash_cleanup_itunes_metadata( &metadata );
            if( !value )
                continue;
            uint32_t dummy[9];
            uint32_t priming_samples;
            uint32_t padding;
            uint64_t duration;
            if( 12 != sscanf( value, " %I32x %I32x %I32x %I64x %I32x %I32x %I32x %I32x %I32x %I32x %I32x %I32x",
                              &dummy[0], &priming_samples, &padding, &duration, &dummy[1], &dummy[2],
                              &dummy[3], &dummy[4], &dummy[5], &dummy[6], &dummy[7], &dummy[8] ) )
            {
                delete [] value;
                continue;
            }
            delete [] value;
            libavsmash_audio_set_implicit_preroll( adhp );
            aohp->skip_decoded_samples = priming_samples;
            vi.num_audio_samples = duration + priming_samples;
            break;
        }
        if( aohp->skip_decoded_samples == 0 )
        {
            uint32_t ctd_shift;
            if( lsmash_get_composition_to_decode_shift_from_media_timeline( root, track_id, &ctd_shift ) )
                env->ThrowError( "LSMASHAudioSource: failed to get the timeline shift." );
            aohp->skip_decoded_samples = ctd_shift + get_start_time( root, track_id );
        }
    }
    /* libavformat */
    AVFormatContext *format_ctx = this->format_ctx.get();
    for( i = 0; i < format_ctx->nb_streams && format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO; i++ );
    if( i == format_ctx->nb_streams )
        env->ThrowError( "LSMASHAudioSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream       *stream = format_ctx->streams[i];
    AVCodecContext *ctx    = stream->codec;
    libavsmash_audio_set_codec_context( adhp, ctx );
    AVCodec *codec = libavsmash_audio_find_decoder( adhp );
    if( !codec )
        env->ThrowError( "LSMASHAudioSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = 0;
    if( avcodec_open2( ctx, codec, nullptr ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avcodec_open2." );
}

static void prepare_audio_decoding
(
    libavsmash_audio_decode_handler_t *adhp,
    libavsmash_audio_output_handler_t *aohp,
    uint64_t                           channel_layout,
    int                                sample_rate,
    VideoInfo                         &vi,
    IScriptEnvironment                *env
)
{
    /* Initialize the audio decoder configuration. */
    lw_log_handler_t *lhp = libavsmash_audio_get_log_handler( adhp );
    lhp->priv = env;
    if( libavsmash_audio_initialize_decoder_configuration( adhp ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to initialize the decoder configuration." );
    aohp->output_channel_layout  = libavsmash_audio_get_best_used_channel_layout ( adhp );
    aohp->output_sample_format   = libavsmash_audio_get_best_used_sample_format  ( adhp );
    aohp->output_sample_rate     = libavsmash_audio_get_best_used_sample_rate    ( adhp );
    aohp->output_bits_per_sample = libavsmash_audio_get_best_used_bits_per_sample( adhp );
    AVCodecContext *ctx = libavsmash_audio_get_codec_context( adhp );
    as_setup_audio_rendering( aohp, ctx, &vi, env, "LSMASHAudioSource", channel_layout, sample_rate );
    /* Count the number of PCM audio samples. */
    vi.num_audio_samples = libavsmash_audio_count_overall_pcm_samples( adhp, aohp->output_sample_rate, &aohp->skip_decoded_samples );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LSMASHAudioSource: no valid audio frame." );
    /* Force seeking at the first reading. */
    libavsmash_audio_force_seek( adhp );
}

LSMASHAudioSource::LSMASHAudioSource
(
    const char         *source,
    uint32_t            track_number,
    bool                skip_priming,
    uint64_t            channel_layout,
    int                 sample_rate,
    const char         *preferred_decoder_names,
    IScriptEnvironment *env
) : LSMASHAudioSource{}
{
    memset( &vi,  0, sizeof(VideoInfo) );
    libavsmash_audio_decode_handler_t *adhp = this->adhp.get();
    libavsmash_audio_output_handler_t *aohp = this->aohp.get();
    set_preferred_decoder_names( preferred_decoder_names );
    libavsmash_audio_set_preferred_decoder_names( adhp, tokenize_preferred_decoder_names() );
    get_audio_track( source, track_number, skip_priming, env );
    lsmash_discard_boxes( libavsmash_audio_get_root( adhp ) );
    prepare_audio_decoding( adhp, aohp, channel_layout, sample_rate, vi, env );
}

LSMASHAudioSource::~LSMASHAudioSource()
{
    libavsmash_audio_decode_handler_t *adhp = this->adhp.get();
    lsmash_root_t *root = libavsmash_audio_get_root( adhp );
    lw_freep( libavsmash_audio_get_preferred_decoder_names( adhp ) );
    lsmash_close_file( &file_param );
    lsmash_destroy_root( root );
}

void __stdcall LSMASHAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    libavsmash_audio_decode_handler_t *adhp = this->adhp.get();
    libavsmash_audio_output_handler_t *aohp = this->aohp.get();
    lw_log_handler_t *lhp = libavsmash_audio_get_log_handler( adhp );
    lhp->priv = env;
    return (void)libavsmash_audio_get_pcm_samples( adhp, aohp, buf, start, wanted_length );
}

AVSValue __cdecl CreateLSMASHVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
#ifdef NDEBUG
    av_log_set_level( AV_LOG_QUIET );
#endif
    const char *source                  = args[0].AsString();
    uint32_t    track_number            = args[1].AsInt( 0 );
    int         threads                 = args[2].AsInt( 0 );
    int         seek_mode               = args[3].AsInt( 0 );
    uint32_t    forward_seek_threshold  = args[4].AsInt( 10 );
    int         direct_rendering        = args[5].AsBool( false ) ? 1 : 0;
    int         fps_num                 = args[6].AsInt( 0 );
    int         fps_den                 = args[7].AsInt( 1 );
    int         stacked_format          = args[8].AsBool( false ) ? 1 : 0;
    enum AVPixelFormat pixel_format     = get_av_output_pixel_format( args[9].AsString( nullptr ) );
    const char *preferred_decoder_names = args[10].AsString( nullptr );
    threads                = threads >= 0 ? threads : 0;
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    direct_rendering      &= (pixel_format == AV_PIX_FMT_NONE);
    return new LSMASHVideoSource( source, track_number, threads, seek_mode, forward_seek_threshold,
                                  direct_rendering, fps_num, fps_den, stacked_format, pixel_format, preferred_decoder_names, env );
}

AVSValue __cdecl CreateLSMASHAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
#ifdef NDEBUG
    av_log_set_level( AV_LOG_QUIET );
#endif
    const char *source                  = args[0].AsString();
    uint32_t    track_number            = args[1].AsInt( 0 );
    bool        skip_priming            = args[2].AsBool( true );
    const char *layout_string           = args[3].AsString( nullptr );
    int         sample_rate             = args[4].AsInt( 0 );
    const char *preferred_decoder_names = args[5].AsString( nullptr );
    uint64_t channel_layout = layout_string ? av_get_channel_layout( layout_string ) : 0;
    return new LSMASHAudioSource( source, track_number, skip_priming,
                                  channel_layout, sample_rate, preferred_decoder_names, env );
}
