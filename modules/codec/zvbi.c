/*****************************************************************************
 * zvbi.c : VBI and Teletext PES demux and decoder using libzvbi
 *****************************************************************************
 * Copyright (C) 2007, M2X
 * $Id$
 *
 * Authors: Derk-Jan Hartman <djhartman at m2x dot nl>
 *          Jean-Paul Saman <jpsaman at m2x dot nl>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 *
 * information on teletext format can be found here :
 * http://pdc.ro.nu/teletext.html
 *
 *****************************************************************************/

/* This module implements:
 * ETSI EN 301 775: VBI data in PES
 * ETSI EN 300 472: EBU Teletext data in PES
 * ETSI EN 300 706: Enhanced Teletext (libzvbi)
 * ETSI EN 300 231: Video Programme System [VPS] (libzvbi)
 * ETSI EN 300 294: 625-line Wide Screen Signaling [WSS] (libzvbi)
 * EIA-608 Revision A: Closed Captioning [CC] (libzvbi)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <assert.h>
#include <libzvbi.h>

#include <vlc_codec.h>
#include "substext.h"

/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define PAGE_TEXT N_("Teletext page")
#define PAGE_LONGTEXT N_("Open the indicated Teletext page." \
        "Default page is index 100")

#define OPAQUE_TEXT N_("Teletext transparency")
#define OPAQUE_LONGTEXT N_("Setting vbi-opaque to true " \
        "makes the text to be boxed and maybe easier to read." )

#define POS_TEXT N_("Teletext alignment")
#define POS_LONGTEXT N_( \
  "You can enforce the teletext position on the video " \
  "(0=center, 1=left, 2=right, 4=top, 8=bottom, you can " \
  "also use combinations of these values, eg. 6 = top-right).")

#define TELX_TEXT N_("Teletext text subtitles")
#define TELX_LONGTEXT N_( "Output teletext subtitles as text " \
  "instead of as RGBA" )

static const int pi_pos_values[] = { 0, 1, 2, 4, 8, 5, 6, 9, 10 };
static const char *const ppsz_pos_descriptions[] =
{ N_("Center"), N_("Left"), N_("Right"), N_("Top"), N_("Bottom"),
  N_("Top-Left"), N_("Top-Right"), N_("Bottom-Left"), N_("Bottom-Right") };

vlc_module_begin ()
    set_description( N_("VBI and Teletext decoder") )
    set_shortname( N_("VBI & Teletext") )
    set_capability( "decoder", 51 )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_SCODEC )
    set_callbacks( Open, Close )

    add_integer( "vbi-page", 100,
                 PAGE_TEXT, PAGE_LONGTEXT, false )
    add_bool( "vbi-opaque", false,
                 OPAQUE_TEXT, OPAQUE_LONGTEXT, false )
    add_integer( "vbi-position", 8, POS_TEXT, POS_LONGTEXT, false )
        change_integer_list( pi_pos_values, ppsz_pos_descriptions );
    add_bool( "vbi-text", false,
              TELX_TEXT, TELX_LONGTEXT, false )
vlc_module_end ()

/****************************************************************************
 * Local structures
 ****************************************************************************/

// #define ZVBI_DEBUG

//Guessing table for missing "default region triplet"
static const int pi_default_triplet[] = {
 0, 0, 0, 0,     // slo slk cze ces
 8,              // pol
 24,24,24,24,24, //scc scr srp hrv slv
 24,24,          //rum ron
 32,32,32,32,32, //est lit rus bul ukr
 48,48,          //gre ell
 64,             //ara
 88,             //heb
 16 };           //default
static const char *const ppsz_default_triplet[] = {
 "slo", "slk", "cze", "ces",
 "pol",
 "scc", "scr", "srp", "hrv", "slv",
 "rum", "ron",
 "est", "lit", "rus", "bul", "ukr",
 "gre", "ell",
 "ara",
 "heb",
 NULL
};

typedef enum {
    ZVBI_KEY_RED    = 'r' << 16,
    ZVBI_KEY_GREEN  = 'g' << 16,
    ZVBI_KEY_YELLOW = 'y' << 16,
    ZVBI_KEY_BLUE   = 'b' << 16,
    ZVBI_KEY_INDEX  = 'i' << 16,
} ttxt_key_id;

#define MAX_SLICES 32

struct decoder_sys_t
{
    vbi_decoder *     p_vbi_dec;
    vbi_sliced        p_vbi_sliced[MAX_SLICES];
    unsigned int      i_last_page;
    bool              b_update;
    bool              b_text;   /* Subtitles as text */

    vlc_mutex_t       lock; /* Lock to protect the following variables */
    /* Positioning of Teletext images */
    int               i_align;
    /* */
    unsigned int      i_wanted_page;
    unsigned int      i_wanted_subpage;
    /* */
    bool              b_opaque;
    struct {
        int pgno, subno;
    }                 nav_link[6];
    int               i_key[3];
};

static subpicture_t *Decode( decoder_t *, block_t ** );

static subpicture_t *Subpicture( decoder_t *p_dec, video_format_t *p_fmt,
                                 bool b_text,
                                 int i_columns, int i_rows,
                                 int i_align, mtime_t i_pts );

static void EventHandler( vbi_event *ev, void *user_data );
static int OpaquePage( picture_t *p_src, const vbi_page *p_page,
                       const video_format_t fmt, bool b_opaque, const int text_offset );
static int get_first_visible_row( vbi_char *p_text, int rows, int columns);
static int get_last_visible_row( vbi_char *p_text, int rows, int columns);

/* Properties callbacks */
static int RequestPage( vlc_object_t *p_this, char const *psz_cmd,
                        vlc_value_t oldval, vlc_value_t newval, void *p_data );
static int Opaque( vlc_object_t *p_this, char const *psz_cmd,
                   vlc_value_t oldval, vlc_value_t newval, void *p_data );
static int Position( vlc_object_t *p_this, char const *psz_cmd,
                     vlc_value_t oldval, vlc_value_t newval, void *p_data );
static int EventKey( vlc_object_t *p_this, char const *psz_cmd,
                     vlc_value_t oldval, vlc_value_t newval, void *p_data );

/*****************************************************************************
 * Open: probe the decoder and return score
 *****************************************************************************
 * Tries to launch a decoder and return score so that the interface is able
 * to chose.
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t *) p_this;
    decoder_sys_t *p_sys = NULL;

    if( p_dec->fmt_in.i_codec != VLC_CODEC_TELETEXT )
        return VLC_EGENERIC;

    p_sys = p_dec->p_sys = calloc( 1, sizeof(decoder_sys_t) );
    if( p_sys == NULL )
        return VLC_ENOMEM;

    p_sys->i_key[0] = p_sys->i_key[1] = p_sys->i_key[2] = '*' - '0';
    p_sys->b_update = false;
    p_sys->p_vbi_dec = vbi_decoder_new();
    vlc_mutex_init( &p_sys->lock );

    if( p_sys->p_vbi_dec == NULL )
    {
        msg_Err( p_dec, "VBI decoder could not be created." );
        Close( p_this );
        return VLC_ENOMEM;
    }

    /* Some broadcasters in countries with level 1 and level 1.5 still not send a G0 to do 
     * matches against table 32 of ETSI 300 706. We try to do some best effort guessing
     * This is not perfect, but might handle some cases where we know the vbi language 
     * is known. It would be better if people started sending G0 */
    for( int i = 0; ppsz_default_triplet[i] != NULL; i++ )
    {
        if( p_dec->fmt_in.psz_language && !strcasecmp( p_dec->fmt_in.psz_language, ppsz_default_triplet[i] ) )
        {
            vbi_teletext_set_default_region( p_sys->p_vbi_dec, pi_default_triplet[i]);
            msg_Dbg( p_dec, "overwriting default zvbi region: %d", pi_default_triplet[i] );
        }
    }

    vbi_event_handler_register( p_sys->p_vbi_dec, VBI_EVENT_TTX_PAGE | VBI_EVENT_NETWORK |
#ifdef ZVBI_DEBUG
                                VBI_EVENT_CAPTION | VBI_EVENT_TRIGGER |
                                VBI_EVENT_ASPECT | VBI_EVENT_PROG_INFO | VBI_EVENT_NETWORK_ID |
#endif
                                0 , EventHandler, p_dec );

    /* Create the var on vlc_global. */
    p_sys->i_wanted_page = var_CreateGetInteger( p_dec, "vbi-page" );
    var_AddCallback( p_dec, "vbi-page", RequestPage, p_sys );

    /* Check if the Teletext track has a known "initial page". */
    if( p_sys->i_wanted_page == 100 && p_dec->fmt_in.subs.teletext.i_magazine != -1 )
    {
        p_sys->i_wanted_page = 100 * p_dec->fmt_in.subs.teletext.i_magazine +
                               vbi_bcd2dec( p_dec->fmt_in.subs.teletext.i_page );
        var_SetInteger( p_dec, "vbi-page", p_sys->i_wanted_page );
    }
    p_sys->i_wanted_subpage = VBI_ANY_SUBNO;

    p_sys->b_opaque = var_CreateGetBool( p_dec, "vbi-opaque" );
    var_AddCallback( p_dec, "vbi-opaque", Opaque, p_sys );

    p_sys->i_align = var_CreateGetInteger( p_dec, "vbi-position" );
    var_AddCallback( p_dec, "vbi-position", Position, p_sys );

    p_sys->b_text = var_CreateGetBool( p_dec, "vbi-text" );
//    var_AddCallback( p_dec, "vbi-text", Text, p_sys );

    /* Listen for keys */
    var_AddCallback( p_dec->p_libvlc, "key-pressed", EventKey, p_dec );

    es_format_Init( &p_dec->fmt_out, SPU_ES, VLC_CODEC_SPU );
    if( p_sys->b_text )
        p_dec->fmt_out.video.i_chroma = VLC_CODEC_TEXT;
    else
        p_dec->fmt_out.video.i_chroma = VLC_CODEC_RGBA;

    p_dec->pf_decode_sub = Decode;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*) p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    var_DelCallback( p_dec, "vbi-position", Position, p_sys );
    var_DelCallback( p_dec, "vbi-opaque", Opaque, p_sys );
    var_DelCallback( p_dec, "vbi-page", RequestPage, p_sys );
    var_DelCallback( p_dec->p_libvlc, "key-pressed", EventKey, p_dec );

    vlc_mutex_destroy( &p_sys->lock );

    if( p_sys->p_vbi_dec )
        vbi_decoder_delete( p_sys->p_vbi_dec );
    free( p_sys );
}

#ifdef WORDS_BIGENDIAN
# define ZVBI_PIXFMT_RGBA32 VBI_PIXFMT_RGBA32_BE
#else
# define ZVBI_PIXFMT_RGBA32 VBI_PIXFMT_RGBA32_LE
#endif

/*****************************************************************************
 * Decode:
 *****************************************************************************/
static subpicture_t *Decode( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t   *p_sys = p_dec->p_sys;
    block_t         *p_block;
    subpicture_t    *p_spu = NULL;
    video_format_t  fmt;
    bool            b_cached = false;
    vbi_page        p_page;

    if( (pp_block == NULL) || (*pp_block == NULL) )
        return NULL;

    p_block = *pp_block;
    *pp_block = NULL;

    if( p_block->i_buffer > 0 &&
        ( ( p_block->p_buffer[0] >= 0x10 && p_block->p_buffer[0] <= 0x1f ) ||
          ( p_block->p_buffer[0] >= 0x99 && p_block->p_buffer[0] <= 0x9b ) ) )
    {
        vbi_sliced   *p_sliced = p_sys->p_vbi_sliced;
        unsigned int i_lines = 0;

        p_block->i_buffer--;
        p_block->p_buffer++;
        while( p_block->i_buffer >= 2 )
        {
            int      i_id   = p_block->p_buffer[0];
            unsigned i_size = p_block->p_buffer[1];

            if( 2 + i_size > p_block->i_buffer )
                break;

            if( ( i_id == 0x02 || i_id == 0x03 ) && i_size >= 44 && i_lines < MAX_SLICES )
            {
                unsigned line_offset  = p_block->p_buffer[2] & 0x1f;
                unsigned field_parity = p_block->p_buffer[2] & 0x20;

                p_sliced[i_lines].id = VBI_SLICED_TELETEXT_B;
                if( line_offset > 0 )
                    p_sliced[i_lines].line = line_offset + (field_parity ? 0 : 313);
                else
                    p_sliced[i_lines].line = 0;
                for( int i = 0; i < 42; i++ )
                    p_sliced[i_lines].data[i] = vbi_rev8( p_block->p_buffer[4 + i] );
                i_lines++;
            }

            p_block->i_buffer -= 2 + i_size;
            p_block->p_buffer += 2 + i_size;
        }

        if( i_lines > 0 )
            vbi_decode( p_sys->p_vbi_dec, p_sliced, i_lines, 0 );
    }

    /* */
    vlc_mutex_lock( &p_sys->lock );
    const int i_align = p_sys->i_align;
    const unsigned int i_wanted_page = p_sys->i_wanted_page;
    const unsigned int i_wanted_subpage = p_sys->i_wanted_subpage;
    const bool b_opaque = p_sys->b_opaque;
    vlc_mutex_unlock( &p_sys->lock );

    /* Try to see if the page we want is in the cache yet */
    memset( &p_page, 0, sizeof(vbi_page) );
    b_cached = vbi_fetch_vt_page( p_sys->p_vbi_dec, &p_page,
                                  vbi_dec2bcd( i_wanted_page ),
                                  i_wanted_subpage, VBI_WST_LEVEL_3p5,
                                  25, true );

    if( i_wanted_page == p_sys->i_last_page && !p_sys->b_update )
        goto error;

    if( !b_cached )
    {
        if( p_sys->b_text && p_sys->i_last_page != i_wanted_page )
        {
            /* We need to reset the subtitle */
            p_spu = Subpicture( p_dec, &fmt, true,
                                p_page.columns, p_page.rows,
                                i_align, p_block->i_pts );
            if( !p_spu )
                goto error;
            subpicture_updater_sys_t *p_spu_sys = p_spu->updater.p_sys;
            p_spu_sys->text = strdup("");

            p_sys->b_update = true;
            p_sys->i_last_page = i_wanted_page;
            goto exit;
        }
        goto error;
    }

    p_sys->b_update = false;
    p_sys->i_last_page = i_wanted_page;
#ifdef ZVBI_DEBUG
    msg_Dbg( p_dec, "we now have page: %d ready for display",
             i_wanted_page );
#endif

    /* Ignore transparent rows at the beginning and end */
    int i_first_row = get_first_visible_row( p_page.text, p_page.rows, p_page.columns );
    int i_num_rows;
    if ( i_first_row < 0 ) {
        i_first_row = p_page.rows - 1;
        i_num_rows = 0;
    } else {
        i_num_rows = get_last_visible_row( p_page.text, p_page.rows, p_page.columns ) - i_first_row + 1;
    }
#ifdef ZVBI_DEBUG
    msg_Dbg( p_dec, "After top and tail of page we have rows %i-%i of %i",
             i_first_row + 1, i_first_row + i_num_rows, p_page.rows );
#endif

    /* If there is a page or sub to render, then we do that here */
    /* Create the subpicture unit */
    p_spu = Subpicture( p_dec, &fmt, p_sys->b_text,
                        p_page.columns, i_num_rows,
                        i_align, p_block->i_pts );
    if( !p_spu )
        goto error;

    if( p_sys->b_text )
    {
        unsigned int i_textsize = 7000;
        int i_total,offset;
        char p_text[i_textsize+1];

        i_total = vbi_print_page_region( &p_page, p_text, i_textsize,
                        "UTF-8", 0, 0, 0, i_first_row, p_page.columns, i_num_rows );

        for( offset=1; offset<i_total && isspace( p_text[i_total-offset ] ); offset++)
           p_text[i_total-offset] = '\0';

        i_total -= offset;

        offset=0;
        while( offset < i_total && isspace( p_text[offset] ) )
           offset++;

        subpicture_updater_sys_t *p_spu_sys = p_spu->updater.p_sys;
        p_spu_sys->text = strdup( &p_text[offset] );

        p_spu_sys->align = i_align;
        p_spu_sys->i_font_height_percent = 5;
        p_spu_sys->renderbg = b_opaque;

#ifdef ZVBI_DEBUG
        msg_Info( p_dec, "page %x-%x(%d)\n\"%s\"", p_page.pgno, p_page.subno, i_total, &p_text[offset] );
#endif
    }
    else
    {
        picture_t *p_pic = p_spu->p_region->p_picture;

        /* ZVBI is stupid enough to assume pitch == width */
        p_pic->p->i_pitch = 4 * fmt.i_width;

        /* Maintain subtitle postion */
        p_spu->p_region->i_y = i_first_row*10;
        p_spu->i_original_picture_width = p_page.columns*12;
        p_spu->i_original_picture_height = p_page.rows*10;

        vbi_draw_vt_page_region( &p_page, ZVBI_PIXFMT_RGBA32,
                          p_spu->p_region->p_picture->p->p_pixels, -1,
                          0, i_first_row, p_page.columns, i_num_rows,
                          1, 1);

        vlc_mutex_lock( &p_sys->lock );
        memcpy( p_sys->nav_link, &p_page.nav_link, sizeof( p_sys->nav_link )) ;
        vlc_mutex_unlock( &p_sys->lock );

        OpaquePage( p_pic, &p_page, fmt, b_opaque, i_first_row * p_page.columns );
    }

exit:
    vbi_unref_page( &p_page );
    block_Release( p_block );
    return p_spu;

error:
    vbi_unref_page( &p_page );
    block_Release( p_block );
    return NULL;
}

static subpicture_t *Subpicture( decoder_t *p_dec, video_format_t *p_fmt,
                                 bool b_text,
                                 int i_columns, int i_rows, int i_align,
                                 mtime_t i_pts )
{
    video_format_t fmt;
    subpicture_t *p_spu=NULL;

    /* If there is a page or sub to render, then we do that here */
    /* Create the subpicture unit */
    if( b_text )
        p_spu = decoder_NewSubpictureText( p_dec );
    else
        p_spu = decoder_NewSubpicture( p_dec, NULL );
    if( !p_spu )
    {
        msg_Warn( p_dec, "can't get spu buffer" );
        return NULL;
    }

    memset( &fmt, 0, sizeof(video_format_t) );
    fmt.i_chroma = b_text ? VLC_CODEC_TEXT : VLC_CODEC_RGBA;
    fmt.i_sar_num = 0;
    fmt.i_sar_den = 1;
    if( b_text )
    {
        fmt.i_bits_per_pixel = 0;
    }
    else
    {
        fmt.i_width = fmt.i_visible_width = i_columns * 12;
        fmt.i_height = fmt.i_visible_height = i_rows * 10;
        fmt.i_bits_per_pixel = 32;
    }
    fmt.i_x_offset = fmt.i_y_offset = 0;

    p_spu->p_region = subpicture_region_New( &fmt );
    if( p_spu->p_region == NULL )
    {
        msg_Err( p_dec, "cannot allocate SPU region" );
        decoder_DeleteSubpicture( p_dec, p_spu );
        return NULL;
    }

    p_spu->p_region->i_x = 0;
    p_spu->p_region->i_y = 0;

    p_spu->i_start = i_pts;
    p_spu->i_stop = b_text ? i_pts + (10*CLOCK_FREQ): 0;
    p_spu->b_ephemer = true;
    p_spu->b_absolute = b_text ? false : true;

    if( !b_text )
        p_spu->p_region->i_align = i_align;
    p_spu->i_original_picture_width = fmt.i_width;
    p_spu->i_original_picture_height = fmt.i_height;

    /* */
    *p_fmt = fmt;
    return p_spu;
}

static void EventHandler( vbi_event *ev, void *user_data )
{
    decoder_t *p_dec        = (decoder_t *)user_data;
    decoder_sys_t *p_sys    = p_dec->p_sys;

    if( ev->type == VBI_EVENT_TTX_PAGE )
    {
#ifdef ZVBI_DEBUG
        msg_Info( p_dec, "Page %03x.%02x ",
                    ev->ev.ttx_page.pgno,
                    ev->ev.ttx_page.subno & 0xFF);
#endif
        if( p_sys->i_last_page == vbi_bcd2dec( ev->ev.ttx_page.pgno ) )
            p_sys->b_update = true;
#ifdef ZVBI_DEBUG
        if( ev->ev.ttx_page.clock_update )
            msg_Dbg( p_dec, "clock" );
        if( ev->ev.ttx_page.header_update )
            msg_Dbg( p_dec, "header" );
#endif
    }
    else if( ev->type == VBI_EVENT_CLOSE )
        msg_Dbg( p_dec, "Close event" );
    else if( ev->type == VBI_EVENT_CAPTION )
        msg_Dbg( p_dec, "Caption line: %x", ev->ev.caption.pgno );
    else if( ev->type == VBI_EVENT_NETWORK )
    {
        msg_Dbg( p_dec, "Network change");
        vbi_network n = ev->ev.network;
        msg_Dbg( p_dec, "Network id:%d name: %s, call: %s ", n.nuid, n.name, n.call );
    }
    else if( ev->type == VBI_EVENT_TRIGGER )
        msg_Dbg( p_dec, "Trigger event" );
    else if( ev->type == VBI_EVENT_ASPECT )
        msg_Dbg( p_dec, "Aspect update" );
    else if( ev->type == VBI_EVENT_PROG_INFO )
        msg_Dbg( p_dec, "Program info received" );
    else if( ev->type == VBI_EVENT_NETWORK_ID )
        msg_Dbg( p_dec, "Network ID changed" );
}

static int get_first_visible_row( vbi_char *p_text, int rows, int columns)
{
    for ( int i = 0; i < rows * columns; i++ )
    {
        if ( p_text[i].opacity != VBI_TRANSPARENT_SPACE )
        {
            return i / columns;
        }
    }

    return -1;
}

static int get_last_visible_row( vbi_char *p_text, int rows, int columns)
{
    for ( int i = rows * columns - 1; i >= 0; i-- )
    {
        if (p_text[i].opacity != VBI_TRANSPARENT_SPACE)
        {
            return i / columns;
        }
    }

    return -1;
}

static int OpaquePage( picture_t *p_src, const vbi_page *p_page,
                       const video_format_t fmt, bool b_opaque, const int text_offset )
{
    unsigned int    x, y;

    assert( fmt.i_chroma == VLC_CODEC_RGBA );

    /* Kludge since zvbi doesn't provide an option to specify opacity. */
    for( y = 0; y < fmt.i_height; y++ )
    {
        for( x = 0; x < fmt.i_width; x++ )
        {
            const vbi_opacity opacity = p_page->text[ text_offset + y/10 * p_page->columns + x/12 ].opacity;
            const int background = p_page->text[ text_offset + y/10 * p_page->columns + x/12 ].background;
            uint32_t *p_pixel = (uint32_t*)&p_src->p->p_pixels[y * p_src->p->i_pitch + 4*x];

            switch( opacity )
            {
            /* Show video instead of this character */
            case VBI_TRANSPARENT_SPACE:
                *p_pixel = 0;
                break;
            /* Display foreground and background color */
            /* To make the boxed text "closed captioning" transparent
             * change true to false.
             */
            case VBI_OPAQUE:
            /* alpha blend video into background color */
            case VBI_SEMI_TRANSPARENT:
                if( b_opaque )
                    break;
            /* Full text transparency. only foreground color is show */
            case VBI_TRANSPARENT_FULL:
                if( (*p_pixel) == (0xff000000 | p_page->color_map[background] ) )
                    *p_pixel = 0;
                break;
            }
        }
    }
    /* end of kludge */
    return VLC_SUCCESS;
}

/* Callbacks */
static int RequestPage( vlc_object_t *p_this, char const *psz_cmd,
                        vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    decoder_sys_t *p_sys = p_data;
    VLC_UNUSED(p_this); VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval);
    int want_navlink = -1;

    vlc_mutex_lock( &p_sys->lock );
    switch( newval.i_int )
    {
        case ZVBI_KEY_RED:
            want_navlink = 0;
            break;
        case ZVBI_KEY_GREEN:
            want_navlink = 1;
            break;
        case ZVBI_KEY_YELLOW:
            want_navlink = 2;
            break;
        case ZVBI_KEY_BLUE:
            want_navlink = 3;
            break;
        case ZVBI_KEY_INDEX:
            want_navlink = 5; /* #4 is SKIPPED */
            break;
    }

    if (want_navlink > -1)
    {
        int page = vbi_bcd2dec( p_sys->nav_link[want_navlink].pgno );
        if (page > 0 && page < 999) {
            p_sys->i_wanted_page = page;
            p_sys->i_wanted_subpage = p_sys->nav_link[want_navlink].subno;
        }
    }
    else if( newval.i_int > 0 && newval.i_int < 999 )
    {
        p_sys->i_wanted_page = newval.i_int;
        p_sys->i_wanted_subpage = VBI_ANY_SUBNO;
    }
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}

static int Opaque( vlc_object_t *p_this, char const *psz_cmd,
                   vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    decoder_sys_t *p_sys = p_data;
    VLC_UNUSED(p_this); VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval);

    vlc_mutex_lock( &p_sys->lock );
    p_sys->b_opaque = newval.b_bool;
    p_sys->b_update = true;
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}

static int Position( vlc_object_t *p_this, char const *psz_cmd,
                     vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    decoder_sys_t *p_sys = p_data;
    VLC_UNUSED(p_this); VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval);

    vlc_mutex_lock( &p_sys->lock );
    p_sys->i_align = newval.i_int;
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}

static int EventKey( vlc_object_t *p_this, char const *psz_cmd,
                        vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    decoder_t *p_dec = p_data;
    decoder_sys_t *p_sys = p_dec->p_sys;

    VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval); VLC_UNUSED( p_this );

    /* FIXME: Capture + and - key for subpage browsing */
    if( newval.i_int == '-' || newval.i_int == '+' )
    {
        vlc_mutex_lock( &p_sys->lock );
        if( p_sys->i_wanted_subpage == VBI_ANY_SUBNO && newval.i_int == '+' )
            p_sys->i_wanted_subpage = vbi_dec2bcd(1);
        else if ( newval.i_int == '+' )
            p_sys->i_wanted_subpage = vbi_add_bcd( p_sys->i_wanted_subpage, 1);
        else if( newval.i_int == '-')
            p_sys->i_wanted_subpage = vbi_add_bcd( p_sys->i_wanted_subpage, 0xF9999999); /* BCD complement - 1 */

        if ( !vbi_bcd_digits_greater( p_sys->i_wanted_subpage, 0x00 ) || vbi_bcd_digits_greater( p_sys->i_wanted_subpage, 0x99 ) )
                p_sys->i_wanted_subpage = VBI_ANY_SUBNO;
        else
            msg_Info( p_dec, "subpage: %d",
                      vbi_bcd2dec( p_sys->i_wanted_subpage) );

        p_sys->b_update = true;
        vlc_mutex_unlock( &p_sys->lock );
    }

    /* Capture 0-9 for page selection */
    if( newval.i_int < '0' || newval.i_int > '9' )
        return VLC_SUCCESS;

    vlc_mutex_lock( &p_sys->lock );
    p_sys->i_key[0] = p_sys->i_key[1];
    p_sys->i_key[1] = p_sys->i_key[2];
    p_sys->i_key[2] = (int)(newval.i_int - '0');
    msg_Info( p_dec, "page: %c%c%c", (char)(p_sys->i_key[0]+'0'),
              (char)(p_sys->i_key[1]+'0'), (char)(p_sys->i_key[2]+'0') );

    int i_new_page = 0;

    if( p_sys->i_key[0] > 0 && p_sys->i_key[0] <= 8 &&
        p_sys->i_key[1] >= 0 && p_sys->i_key[1] <= 9 &&
        p_sys->i_key[2] >= 0 && p_sys->i_key[2] <= 9 )
    {
        i_new_page = p_sys->i_key[0]*100 + p_sys->i_key[1]*10 + p_sys->i_key[2];
        p_sys->i_key[0] = p_sys->i_key[1] = p_sys->i_key[2] = '*' - '0';
    }
    vlc_mutex_unlock( &p_sys->lock );

    if( i_new_page > 0 )
        var_SetInteger( p_dec, "vbi-page", i_new_page );

    return VLC_SUCCESS;
}
