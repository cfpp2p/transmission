/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: webseed.c 14427 2015-03-04 17:11:04Z jordan $
 */

#include <string.h> /* strlen() */

#include <event2/buffer.h>
#include <event2/event.h>

#include "transmission.h"
#include "bandwidth.h"
#include "cache.h"
#include "inout.h" /* tr_ioFindFileLocation() */
#include "list.h"
#include "net.h" /* tr_address */
#include "session.h"
#include "peer-mgr.h"
#include "torrent.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"
#include "web.h"
#include "webseed.h"

struct tr_webseed_task
{
    struct evbuffer    * content;
    struct tr_webseed  * webseed;
    tr_block_index_t     block;
    tr_piece_index_t     piece_index;
    uint32_t             piece_offset;
    uint32_t             length;
    tr_block_index_t     blocks_done;
    uint32_t             block_size;
    struct tr_web_task * web_task;
    long                 response_code;
};

struct tr_webseed
{
    tr_peer              parent;
    tr_bandwidth         bandwidth;
    tr_session         * session;
    tr_peer_callback   * callback;
    void               * callback_data;
    tr_list            * tasks;
    struct event       * timer;
    char               * base_url;
    size_t               base_url_len;
    int                  torrent_id;
    bool                 is_stopping;
    int                  consecutive_failures;
    int                  wait_factor;
    int                  retry_tickcount;
    int                  retry_challenge;
    int                  idle_connections;
    int                  active_transfers;
    char              ** file_urls;
    struct tr_bitfield   blame;
    int                  strike_count;
};

struct tr_web_task
{
    int torrentId;
    int is_blocklisted;
    long code;
    long timeout_secs;
    bool did_connect;
    bool did_timeout;
    struct evbuffer * response;
    struct evbuffer * freebuf;
    char * tracker_addr;
    char * url;
    char * range;
    char * cookies;
    tr_session * session;
    tr_web_done_func * done_func;
    void * done_func_user_data;
    CURL * curl_easy;
    struct tr_web_task * next;
};

enum
{
    TR_IDLE_TIMER_MSEC = 2000,

    FAILURE_RETRY_INTERVAL = 150,

    MAX_CONSECUTIVE_FAILURES = 5,

    MAX_WEBSEED_CONNECTIONS = 4,

    MAX_BAD_PIECES_PER_WEBSEED = 5,

    MAX_WAIT_FACTOR = 288   // -- 24 hours -- 150 * 2000ms * 288
};

static void
webseed_free( struct tr_webseed * w )
{
    tr_dbg( "freeing webseed task" );
    if( !w )
    {
        tr_dbg( "???aborted freeing webseed - task is already empty" );
        return;
    }
    tr_torrent * tor = tr_torrentFindFromId( w->session, w->torrent_id );
    /* tr_isTorrent() is ALREADY called by path of tr_torrentHasMetadata()
    so really a redundant useless, but leave anyway SRS 11-01-12 */
    if( tr_isTorrent( tor ) && tr_torrentHasMetadata( tor ) ) {

        if( tor->isRunning )
        {
            tr_tordbg( tor, "free webseed error - should not be running - run flag:%d - stop flag:%d -",
                       tor->isRunning, tor->isStopping );
            return;
        }

        const tr_info * inf = tr_torrentInfo( tor );
        tr_file_index_t i;

        /* if we have an array of file URLs, free it */
        if( w->file_urls != NULL ) {
            for( i = 0; i < inf->fileCount; i++ ) {
               /* revert 12859 by SRS 10-14-2012 */
               if( w->file_urls[i] )
                    tr_free( w->file_urls[i] );
            }
            tr_free( w->file_urls );
        }
        tr_tordbg( tor, "free webseed URLs completed - run flag:%d - stop flag:%d -",
                   tor->isRunning, tor->isStopping );
    }
    else
        tr_dbg( "free webseed - torrent is incomplete magnet or deleted - continuing with destruct" );

    if( tr_isTorrent( tor ) && !tr_torrentHasMetadata( tor ) ) {
        tr_tordbg( tor, "free webseed incomplete webseed magnet -nothing to be done - run flag:%d - stop flag:%d -",
                   tor->isRunning, tor->isStopping );
        return;
    }

    /* webseed destruct */
    event_free( w->timer );
    tr_bandwidthDestruct( &w->bandwidth );
    tr_bitfieldDestruct (&w->blame);
    tr_free( w->base_url );

    /* parent class destruct */
    tr_peerDestruct( tor, &w->parent );

    tr_free( w );

    if( tor )
        tr_tordbg( tor, "free webseed completed - run flag:%d - stop flag:%d -",
                  tor->isRunning, tor->isStopping );
    else
        tr_dbg( "free webseed completed - torrent is empty" );
}

/***
****
***/

static void
publish( tr_webseed * w, tr_peer_event * e )
{
    if( w->callback != NULL )
        w->callback( &w->parent, e, w->callback_data );
}

static void
fire_client_got_rejs( tr_torrent * tor, tr_webseed * w,
                      tr_block_index_t block, tr_block_index_t count )
{
    tr_block_index_t i;
    tr_peer_event e = TR_PEER_EVENT_INIT;
    e.eventType = TR_PEER_CLIENT_GOT_REJ;
    tr_torrentGetBlockLocation( tor, block, &e.pieceIndex, &e.offset, &e.length );
    for( i = 1; i <= count; i++ ) {
        if( i == count )
            e.length = tr_torBlockCountBytes( tor, block + count - 1 );
        publish( w, &e );
        e.offset += e.length;
    }
}

static void
fire_client_got_blocks( tr_torrent * tor, tr_webseed * w,
                        tr_block_index_t block, tr_block_index_t count )
{
    tr_block_index_t i;
    tr_peer_event e = TR_PEER_EVENT_INIT;
    e.eventType = TR_PEER_CLIENT_GOT_BLOCK;
    tr_torrentGetBlockLocation( tor, block, &e.pieceIndex, &e.offset, &e.length );
    for( i = 1; i <= count; i++ ) {
        if( i == count )
            e.length = tr_torBlockCountBytes( tor, block + count - 1 );
        publish( w, &e );
        e.offset += e.length;
    }
}

static void
fire_client_got_data( tr_webseed * w, uint32_t length )
{
    tr_peer_event e = TR_PEER_EVENT_INIT;
    e.eventType = TR_PEER_CLIENT_GOT_DATA;
    e.length = length;
    e.wasPieceData = true;
    publish( w, &e );
}

/***
****
***/

struct write_block_data
{
    struct tr_webseed  * webseed;
    struct evbuffer    * content;
    tr_piece_index_t     piece_index;
    tr_block_index_t     block_index;
    tr_block_index_t     count;
    uint32_t             block_offset;
};

static void
write_block_func( void * vdata )
{
    struct write_block_data * data = vdata;
    struct tr_webseed * w = data->webseed;
    struct evbuffer * buf = data->content;
    struct tr_torrent * tor;

    if( !tr_isSession( w->session ) ) {
        tr_dbg( "write block aborted - no session for webseed torrent" );
        // fix possibility of crash when callback comes after webseed_free from the torrent remove
        evbuffer_free( buf );
        tr_free( data );
        return;
    }

    tor = tr_torrentFindFromId( w->session, w->torrent_id );

    if( w->wait_factor >= MAX_WAIT_FACTOR ) {
        if( !tor )
           tr_dbg( "?unknown? webseed deleted torrent ID %d write block aborted by wait factor %d ",
                    w->torrent_id, w->wait_factor );
        else
        {
            tr_tordbg( tor, "webseed paused - write block aborted by wait factor %d ",
                        w->wait_factor );
            fire_client_got_rejs( tor, w, data->block_index, data->count );
        }
        evbuffer_free( buf );
        tr_free( data );
        return;
    }

    if( !tor || !tor->isRunning || tor->isStopping || tr_torrentIsSeed( tor ) || w->is_stopping )
    {
        if( !tor )
           tr_dbg( "?unknown? webseed deleted torrent ID %d write block aborted - w stop flag is %d ",
                    w->torrent_id, w->is_stopping );
        else
        {
            tr_tordbg( tor, "webseed paused - write block aborted - run flag:%d - stop flag:%d - w stop flag is %d ",
                       tor->isRunning, tor->isStopping, w->is_stopping );
            fire_client_got_rejs( tor, w, data->block_index, data->count );
        }
        w->wait_factor = MAX_WAIT_FACTOR;
    }
    else
    {
        const uint32_t block_size = tor->blockSize;
        uint32_t len = evbuffer_get_length( buf );
        const uint32_t offset_end = data->block_offset + len;
        tr_cache * cache = w->session->cache;
        const tr_piece_index_t piece = data->piece_index;

        if( !tr_cpPieceIsComplete( &tor->completion, piece ) )
        // do not write the block if it is already complete - from libevent thread
        // also prevents a bug where a corrupt block overwrites a good block
        {
            while( len > 0 )
            {
                const uint32_t bytes_this_pass = MIN( len, block_size );
                tr_cacheWriteBlock( cache, tor, piece, offset_end - len, bytes_this_pass, buf );
                len -= bytes_this_pass;
            }

            tr_bitfieldAdd (&w->blame, piece);
            fire_client_got_blocks( tor, w, data->block_index, data->count );
        }
        else
        {
            tr_tordbg( tor, "we did ask for this piece, but piece %d is already complete...", piece );
            fire_client_got_rejs( tor, w, data->block_index, data->count );
        }
    }

    evbuffer_free( buf );
    tr_free( data );
}

/***
****
***/

struct connection_succeeded_data
{
    struct tr_webseed  * webseed;
    char               * real_url;
    tr_piece_index_t     piece_index;
    uint32_t             piece_offset;
};

static void
connection_succeeded( void * vdata )
{
    tr_torrent * tor;
    struct connection_succeeded_data * data = vdata;
    struct tr_webseed * w = data->webseed;

    tor = tr_torrentFindFromId( w->session, w->torrent_id );

    if( !tor || !tor->isRunning || tor->isStopping || tr_torrentIsSeed( tor ) || w->is_stopping )
    {
        if( !tor )
           tr_dbg( "?unknown? ID %d webseed deleted torrent connection aborted - w stop flag is %d ",
                    w->torrent_id, w->is_stopping );
        else
            tr_tordbg( tor, "webseed paused - connection aborted - run flag:%d - stop flag:%d - w stop flag is %d ",
                       tor->isRunning, tor->isStopping, w->is_stopping );
        w->wait_factor = MAX_WAIT_FACTOR;
        return;
    }

    if( ( data->piece_index >= tor->info.pieceCount )
        || ( tr_pieceOffset( tor, data->piece_index, data->piece_offset, 0 ) >= tor->info.totalSize ) )
    {
        tr_tordbg( tor, "webseed paused - connection aborted - piece index:%d - piece offset:%d - total size: %"PRIu64" bytes",
                   data->piece_index, data->piece_offset, tor->info.totalSize );
        w->wait_factor = MAX_WAIT_FACTOR;
        return;
    }

    if( ++w->active_transfers >= w->retry_challenge && w->retry_challenge )
        /* the server seems to be accepting more connections now */
        w->consecutive_failures = w->retry_tickcount = w->retry_challenge = 0;

    if( data->real_url && tor )
    {
        uint64_t file_offset;
        tr_file_index_t file_index;

        tr_ioFindFileLocation( tor, data->piece_index, data->piece_offset,
                               &file_index, &file_offset );
        tr_free( w->file_urls[file_index] );
        w->file_urls[file_index] = data->real_url;
    }
}

static void
connection_blocklisted( void * vdata )
{
    tr_torrent * tor;
    struct connection_succeeded_data * data = vdata;
    struct tr_webseed * w = data->webseed;

    tor = tr_torrentFindFromId( w->session, w->torrent_id );

    if( !tor || !tor->isRunning || tor->isStopping || tr_torrentIsSeed( tor ) || w->is_stopping )
    {
        if( !tor )
           tr_dbg( "?unknown? ID %d webseed deleted torrent connection aborted in blocklist set - w stop flag is %d ",
                     w->torrent_id, w->is_stopping );
        else
            tr_tordbg( tor, "webseed paused - connection aborted in blocklist set - run flag:%d - stop flag:%d - w stop flag is %d ",
                       tor->isRunning, tor->isStopping, w->is_stopping );
        return;
    }

    if( ( data->piece_index >= tor->info.pieceCount )
        || ( tr_pieceOffset( tor, data->piece_index, data->piece_offset, 0 ) >= tor->info.totalSize ) )
    {
        tr_tordbg( tor, "webseed paused - connection aborted in blocklist set - piece index:%d - piece offset:%d - total size: %"PRIu64" bytes",
                   data->piece_index, data->piece_offset, tor->info.totalSize );
        return;
    }

    if( tor )
    {
        uint64_t file_offset;
        tr_file_index_t file_index;

        tr_ioFindFileLocation( tor, data->piece_index, data->piece_offset,
                               &file_index, &file_offset );
        tr_free( w->file_urls[file_index] );
        w->file_urls[file_index] = data->real_url;
    }
}

/***
****
***/

static void
on_content_changed( struct evbuffer                * buf,
                    const struct evbuffer_cb_info  * info,
                    void                           * vtask )
{
    uint32_t len;
    const size_t n_added = info->n_added;
    struct tr_webseed_task * task = vtask;
    struct tr_webseed * w = task->webseed;
    struct tr_web_task * web_task = task->web_task;

    if( ( web_task->is_blocklisted != 0 )
        && ( web_task->is_blocklisted != 1 )
        && ( web_task->is_blocklisted != 99 ) )
        tr_dbg( "web-task-is-blocklisted at occ: bad value %d ", web_task->is_blocklisted );

    bool is_blocklisted = false;

    if( n_added <= 0 )
        return;

    struct tr_torrent * wtor;
    wtor = tr_torrentFindFromId( w->session, w->torrent_id );

    if( web_task->is_blocklisted == 99 )
    {
        if( !wtor )
           tr_dbg( "?unknown? ID %d webseed task response code %d content changed BL 99 aborted - wait factor %d ",
                   w->torrent_id, (int)task->response_code, w->wait_factor );
        else
            tr_tordbg( wtor, "webseed task response code %d content changed BL 99 aborted - wait factor %d ",
                       (int)task->response_code, w->wait_factor );
        w->wait_factor = MAX_WAIT_FACTOR;
        return;
    }

    if( w->wait_factor >= MAX_WAIT_FACTOR )
    {
        if( !wtor )
           tr_dbg( "?unknown? ID %d webseed task response code %d content changed aborted - wait factor %d ",
                   w->torrent_id, (int)task->response_code, w->wait_factor );
        else
            tr_tordbg( wtor, "webseed task response code %d content changed aborted - wait factor %d ",
                       (int)task->response_code, w->wait_factor );
        web_task->is_blocklisted = 99;
        return;
    }

    if( !wtor || !wtor->isRunning || wtor->isStopping || tr_torrentIsSeed( wtor ) || w->is_stopping )
    {
        if( !wtor )
           tr_dbg( "?unknown? ID %d webseed deleted torrent content changed - aborted - w stop flag is %d ",
                    w->torrent_id, w->is_stopping );
        else
            tr_tordbg( wtor, "webseed paused - content changed aborted - run flag:%d - stop flag:%d - w stop flag is %d ",
                       wtor->isRunning, wtor->isStopping, w->is_stopping );
        task->response_code = 997L;
        w->wait_factor = MAX_WAIT_FACTOR;
        web_task->is_blocklisted = 99;
        return;
    }

    if( !w->is_stopping )
    {
        tr_bandwidthUsed( &w->bandwidth, TR_DOWN, n_added, true, tr_time_msec( ) );
        fire_client_got_data( w, n_added );
    }

    len = evbuffer_get_length( buf );

    if( !task->response_code )
    {
        tr_webGetTaskInfo( task->web_task, TR_WEB_GET_CODE, &task->response_code );

        if( task->response_code == 206 )
        {
            const char * url;
            struct connection_succeeded_data * data;
            const char * server_ip;
            const char * effective_ip;
            struct tr_address addr;

            url = NULL;
            tr_webGetTaskInfo( task->web_task, TR_WEB_GET_REAL_URL, &url );

/////////////////////////////////////////////////////////////////////////////////

            if( w->session->blockListWebseeds ) {
                tr_address_from_string( &addr, "0.0.0.0" );

                // blocklist check
                server_ip = NULL;
                tr_webGetTaskInfo( task->web_task, TR_WEB_GET_PRIMARY_IP, &server_ip );

                if( tr_address_from_string( &addr, server_ip ) )
                {
                    if( tr_sessionIsAddressBlocked( w->session, &addr ) )
                    is_blocklisted = true;
                    w->wait_factor = MAX_WAIT_FACTOR;
                    web_task->is_blocklisted = 99;
                }

                const tr_address * send_address = &addr;
                effective_ip = tr_strdup( tr_address_to_string( send_address ) );

            // http://curl.haxx.se/libcurl/c/curl_easy_getinfo.html
            // You should not free the memory returned by this function unless it is explicitly mentioned below.
            //                tr_free( server_ip );
				
//////////////////////////////////////////////////////////////////////////////////

                if( is_blocklisted ) {
                    struct tr_torrent * tor;
                    tor = tr_torrentFindFromId( w->session, w->torrent_id );
                    if( !effective_ip ) effective_ip = tr_strdup( "??unknown??" );
                    if( !url ) url = tr_strdup( "??unknown??" );
                    if( tor )
                        tr_tordbg( tor, "Blocklisted - Webseeder IP:%s - Real URL:%s -", effective_ip, url );
                    else
                        tr_dbg( "?unknown? torrent Blocklisted - Webseeder IP:%s - Real URL:%s -", effective_ip, url );
                    // invalidate
                    w->wait_factor = MAX_WAIT_FACTOR;
                    web_task->is_blocklisted = 99;
                    data = tr_new( struct connection_succeeded_data, 1 );
                    data->webseed = w;
                    data->real_url = tr_strdup( " " );
                    data->piece_index = task->piece_index;
                    data->piece_offset = task->piece_offset
                                       + (task->blocks_done * task->block_size)
                                       + (len - 1);
                    /* processing this uses a tr_torrent pointer,
                      so push the work to the libevent thread... */
                    tr_runInEventThread( w->session, connection_blocklisted, data );
                }
            }
            else {
                data = tr_new( struct connection_succeeded_data, 1 );
                data->webseed = w;
                data->real_url = tr_strdup( url );
                data->piece_index = task->piece_index;
                data->piece_offset = task->piece_offset
                                   + (task->blocks_done * task->block_size)
                                   + (len - 1);

                /* processing this uses a tr_torrent pointer,
                  so push the work to the libevent thread... */
                tr_runInEventThread( w->session, connection_succeeded, data );
            }
        }
    }

    if( ( task->response_code == 206 ) && ( len >= task->block_size ) && !is_blocklisted )
    {
        /* once we've got at least one full block, save it */

        struct write_block_data * data;
        const uint32_t block_size = task->block_size;
        const tr_block_index_t completed = len / block_size;

        data = tr_new( struct write_block_data, 1 );
        data->webseed = task->webseed;
        data->piece_index = task->piece_index;
        data->block_index = task->block + task->blocks_done;
        data->count = completed;
        data->block_offset = task->piece_offset + task->blocks_done * block_size;
        data->content = evbuffer_new( );

        /* we don't use locking on this evbuffer so we must copy out the data
        that will be needed when writing the block in a different thread */
        evbuffer_remove_buffer( task->content, data->content,
                                block_size * completed );

        tr_runInEventThread( w->session, write_block_func, data );
        task->blocks_done += completed;
    }
}

static void task_request_next_chunk( struct tr_webseed_task * task );

static bool
webseed_has_tasks( const tr_webseed * w )
{
    return w->tasks != NULL;
}


static void
on_idle( tr_webseed * w )
{
    tr_torrent * tor = tr_torrentFindFromId( w->session, w->torrent_id );
    int want, running_tasks = tr_list_size( w->tasks );

    if( w->consecutive_failures >= MAX_CONSECUTIVE_FAILURES ) {
        want = w->idle_connections;

        if( w->retry_tickcount >= ( FAILURE_RETRY_INTERVAL * w->wait_factor ) ) {
            /* some time has passed since our connection attempts failed. try again */
            ++want;
            /* if this challenge is fulfilled we will reset consecutive_failures */
            w->retry_challenge = running_tasks + want;
        }
    }
    else {
        want = MAX_WEBSEED_CONNECTIONS - running_tasks;
        w->retry_challenge = running_tasks + w->idle_connections + 1;
    }

    if( !tor || !tor->isRunning || tor->isStopping || tr_torrentIsSeed( tor ) )
    {
        w->wait_factor = MAX_WAIT_FACTOR;
    }

    if( w->wait_factor >= MAX_WAIT_FACTOR ) want = 0; //blocklisted webseeder or very very unresponsive server

    // if( want < 1 ) // hmmm - now what 
    if( want < 0 )
    {
        if( !tor )
          tr_dbg( "?unknown? torrent ID %d webseed WANT less than zero!! %d ", w->torrent_id, want );
        else
            tr_tordbg( tor, "webseed WANT less than zero!! %d ", want );
    }

    // we should only get here originally from torrentFree
    if( w->is_stopping && !webseed_has_tasks( w ) )
    {
        webseed_free( w );
        if( !tor )
          tr_dbg( "?unknown? torrent ID %d webseed freed by idle timer hit", w->torrent_id );
        else
            tr_tordbg( tor, "webseed freed by idle timer hit - run flag:%d - stop flag:%d -",
                       tor->isRunning, tor->isStopping );
    }
    else if( !w->is_stopping && ( want > 0 ) )
    {
        int i;
        int got = 0;
        tr_block_index_t * blocks = NULL;

        blocks = tr_new( tr_block_index_t, want*2 );
        tr_peerMgrGetNextRequests( tor, &w->parent, want, blocks, &got, true );

        w->idle_connections -= MIN( w->idle_connections, got );
        if( w->retry_tickcount >= ( FAILURE_RETRY_INTERVAL * w->wait_factor ) && got == want )
            w->retry_tickcount = 0;

        for( i=0; i<got; ++i )
        {
            const tr_block_index_t b = blocks[i*2];
            const tr_block_index_t be = blocks[i*2+1];
            struct tr_webseed_task * task = tr_new( struct tr_webseed_task, 1 );
            task->webseed = w;
            task->block = b;
            task->piece_index = tr_torBlockPiece( tor, b );
            task->piece_offset = ( tor->blockSize * b )
                                - ( tor->info.pieceSize * task->piece_index );
            task->length = (be - b) * tor->blockSize + tr_torBlockCountBytes( tor, be );
            task->blocks_done = 0;
            task->response_code = 0;
            task->block_size = tor->blockSize;
            task->content = evbuffer_new( );
            evbuffer_add_cb( task->content, on_content_changed, task );
            tr_list_append( &w->tasks, task );
            task_request_next_chunk( task );
        }

        tr_free( blocks );
    }
}


static void
web_response_func( tr_session    * session,
                   bool            did_connect UNUSED,
                   bool            did_timeout UNUSED,
                   int             is_blocklisted,
                   const char    * tracker_addr,
                   long            response_code,
                   const void    * response UNUSED,
                   size_t          response_byte_count UNUSED,
                   void          * vtask )
{
    struct tr_webseed_task * t = vtask;
    tr_webseed * w = t->webseed;
    struct tr_web_task * web_task = t->web_task;
    tr_torrent * tor = tr_torrentFindFromId( session, w->torrent_id );
    int success = ( response_code == 206 );

    if( ( web_task->is_blocklisted != 0 )
        && ( web_task->is_blocklisted != 1 )
        && ( web_task->is_blocklisted != 99 ) )
        tr_dbg( "web-task-is-blocklisted at wrf: bad value %d ", web_task->is_blocklisted );

    if( ( is_blocklisted == 1 ) && w->session->blockListWebseeds )
    {
        if( !tracker_addr ) tracker_addr = tr_strdup( "??unknown??" );
        if( tor )
            tr_tordbg( tor, "Blocklisted webseeder - validated on web response - IP:%s -", tracker_addr );
        else if( w->torrent_id )
		    tr_dbg( "??unknown?? webseeder blocklisted - old torrent ID was %d - validated IP:%s -", w->torrent_id, tracker_addr );
        else
            tr_dbg( "??unknown?? webseeder blocklisted - validated IP:%s -", tracker_addr );

        w->wait_factor = MAX_WAIT_FACTOR;
    }
    else if( is_blocklisted == 99 )
    {
        if( !tracker_addr ) tracker_addr = tr_strdup( "??unknown??" );
        if( tor )
            tr_tordbg( tor, "Blocklisted webseeder - MAX WAIT FACTOR - IP:%s -", tracker_addr );
        else if( w->torrent_id )
		    tr_dbg( "??unknown?? webseeder blocklisted - old torrent ID was %d - MAX WAIT FACTOR IP:%s -", w->torrent_id, tracker_addr );
        else
            tr_dbg( "??unknown?? webseeder blocklisted - MAX WAIT FACTOR IP:%s -", tracker_addr );

        w->wait_factor = MAX_WAIT_FACTOR;
    }
    else if( web_task->is_blocklisted == 99 )
    {
        if( !tracker_addr ) tracker_addr = tr_strdup( "??unknown??" );
        if( tor )
            tr_tordbg( tor, "Blocklisted BP 99 webseeder - MAX WAIT FACTOR - IP:%s - %d",
                       tracker_addr, web_task->is_blocklisted );
        else if( w->torrent_id )
		    tr_dbg( "??unknown?? webseeder blocklisted BP 99 - old torrent ID was %d - MAX WAIT FACTOR IP:%s - %d",
                       w->torrent_id, tracker_addr, web_task->is_blocklisted );
        else
            tr_dbg( "??unknown?? webseeder blocklisted BP 99 - MAX WAIT FACTOR IP:%s - %d",
                       tracker_addr, web_task->is_blocklisted );

        is_blocklisted = 99;
        w->wait_factor = MAX_WAIT_FACTOR;
    }
    else
        is_blocklisted = 0;

    if( response_code == 999 )
    {
        w->wait_factor = MAX_WAIT_FACTOR;
        if( tor )
            tr_tordbg( tor, "pausing webseed code 999 Too many redirects" );
        else if( w->torrent_id )
            tr_dbg( "??unknown?? webseed torrent pausing %d - code 999 Too many redirects", w->torrent_id );
        else
            tr_dbg( "detected response code 999 Too many redirects" );
    }

    else if( response_code == 998 )
    {
        w->wait_factor = MAX_WAIT_FACTOR;
        if( tor )
            tr_tordbg( tor, "pausing webseed code 998 Webseed server compression error" );
        else if( w->torrent_id )
            tr_dbg( "??unknown?? webseed torrent pausing %d - code 998 Webseed server compression error", w->torrent_id );
        else
            tr_dbg( "detected response code 998 Webseed server compression error" );
    }

    else if( response_code == 997 )
    {
        w->wait_factor = MAX_WAIT_FACTOR;
        if( tor )
            tr_tordbg( tor, "pausing webseed code 997 Webseed torrent halt" );
        else if( w->torrent_id )
            tr_dbg( "??unknown?? webseed torrent pausing %d - code 997 Webseed torrent halt", w->torrent_id );
        else
            tr_dbg( "detected response code 997 Webseed torrent halt" );
    }

    else if( response_code == 996 )
    {
        w->wait_factor = MAX_WAIT_FACTOR;
        if( tor )
            tr_tordbg( tor, "pausing webseed code 996 CURLcode error" );
        else if( w->torrent_id )
            tr_dbg( "??unknown?? webseed torrent pausing %d - code 996 CURLcode error", w->torrent_id );
        else
            tr_dbg( "detected response code 996 CURLcode error" );
    }

    if( w->is_stopping )
    {
        success = 0;
        w->wait_factor = MAX_WAIT_FACTOR;
        t->response_code = 997L;
        if( !tor )
           tr_dbg( "?unknown? ID %d webseed deleted torrent content changed - aborted - w stop flag is %d ",
                    w->torrent_id, w->is_stopping );
        else
            tr_tordbg( tor, "webseed paused - content changed aborted - run flag:%d - stop flag:%d - w stop flag is %d ",
                       tor->isRunning, tor->isStopping, w->is_stopping );
    }

    if( tor )
    {
        if( !tor->isRunning || tor->isStopping || tr_torrentIsSeed( tor ) )
        {
            tr_tordbg( tor, "webseed paused - web response aborted - run flag:%d - stop flag:%d -",
                       tor->isRunning, tor->isStopping );
            success = 0;  // if we have a paused state then drop the task so we can eventually free everything
            w->wait_factor = MAX_WAIT_FACTOR;
            t->response_code = 997L; // lets fail this one because we are paused
        }
    }

    if( w->wait_factor >= MAX_WAIT_FACTOR )
        web_task->is_blocklisted = 99;

    const tr_block_index_t blocks_remain = (t->length + tor->blockSize - 1)
                                                   / tor->blockSize - t->blocks_done;

    if( tor && ( w->wait_factor < MAX_WAIT_FACTOR ) )
    {
        /* active_transfers was only increased if the connection was successful */
        if( t->response_code == 206 && !is_blocklisted )
            --w->active_transfers;

        if( !success )
        {
            if( blocks_remain )
                fire_client_got_rejs( tor, w, t->block + t->blocks_done, blocks_remain );

            if( t->blocks_done )
                ++w->idle_connections;
            else if( ++w->consecutive_failures >= MAX_CONSECUTIVE_FAILURES && !w->retry_tickcount )
                     {
                         /* now wait a while until retrying to establish a connection */
                         ++w->retry_tickcount;
                         if( w->session->maxWebseedConnectFails >= 999999999 )
                         {
                             if( ++w->wait_factor >= 5 ) w->wait_factor = 28800;
                         }
                         else
                         {
                             if( ++w->wait_factor >= w->session->maxWebseedConnectFails ) w->wait_factor = 28800;
                         }
                                                                                // 28800 - wait 100 days -- arbitrary
                         if( w->session->maxWebseedConnectFails == 0 ) w->wait_factor = 1; // yikes! - bypass ALL blocks !!
                     }

            tr_list_remove_data( &w->tasks, t );
            evbuffer_free( t->content );
            tr_free( t );
        }
        else
        {
            const uint32_t bytes_done = t->blocks_done * tor->blockSize;
            const uint32_t buf_len = evbuffer_get_length( t->content );

            if( bytes_done + buf_len < t->length )
            {
                if( !is_blocklisted )
                {
                    /* request finished successfully but there's still data missing. that
                    means we've reached the end of a file and need to request the next one */
                    t->response_code = 0;
                    task_request_next_chunk( t );
                }
            }
            else
            {
                if( buf_len ) {
                    /* on_content_changed() will not write a block if it is smaller than
                    the torrent's block size, i.e. the torrent's very last block */
                    if( !tr_cpPieceIsComplete( &tor->completion, t->piece_index ) )
                    {
                        tr_cacheWriteBlock( session->cache, tor,
                                            t->piece_index, t->piece_offset + bytes_done,
                                            buf_len, t->content );

                        tr_bitfieldAdd (&w->blame, t->piece_index);
                        fire_client_got_blocks( tor, t->webseed,
                                                t->block + t->blocks_done, 1 );
                    }
                    else
                    {
                        tr_tordbg( tor, "we did ask for this piece, but piece %d is already complete...",
                                                                               t->piece_index );
                        fire_client_got_rejs( tor, t->webseed,
                                                t->block + t->blocks_done, 1 );
                    }
                }

                ++w->idle_connections;

                tr_list_remove_data( &w->tasks, t );
                evbuffer_free( t->content );
                tr_free( t );

                on_idle( w );
            }
        }
        if( !success && !t->blocks_done && ( w->consecutive_failures >= MAX_CONSECUTIVE_FAILURES )
            && !w->retry_tickcount && ( w->wait_factor >= MAX_WAIT_FACTOR ) )
        {
            web_task->is_blocklisted = 99;
            w->wait_factor = 28800;
        }
    }
    else
    {
        if( tor && ( w->wait_factor >= MAX_WAIT_FACTOR ) )
        {
            if( blocks_remain )
                fire_client_got_rejs( tor, w, t->block + t->blocks_done, blocks_remain );
            else
            {
                tr_tordbg( tor, "last block rejs - torrent ID %d",
                            w->torrent_id );
                fire_client_got_rejs( tor, t->webseed,
                                        t->block + t->blocks_done, 1 );
            }

            web_task->is_blocklisted = 99;
            tr_tordbg( tor, "web_response_function - too big a wait factor %d",
                             w->wait_factor );
        }
        if( !tor )
        {
            tr_dbg( "?unknown? ID %d webseed deleted torrent web response aborted", w->torrent_id );
            web_task->is_blocklisted = 99;
            w->wait_factor = MAX_WAIT_FACTOR;
            t->response_code = 997L;
        }
        tr_list_remove_data( &w->tasks, t );
        evbuffer_free( t->content );
        tr_free( t );
    }
}

void
increment_webseed_strike_count (tr_webseed     * w,
            tr_torrent     * tor,
            tr_piece_index_t    pieceIndex)
{
  if (tr_bitfieldHas (&w->blame, pieceIndex))
    {
      ++w->strike_count;
     
      if (w->strike_count < MAX_BAD_PIECES_PER_WEBSEED)
         tr_tordbg (tor, "Webseed URL:%s contributed to corrupt piece %d, strike_count is %d.",
              w->base_url, pieceIndex, w->strike_count);
      else
      {
         w->wait_factor = MAX_WAIT_FACTOR;
         tr_tordbg (tor, "Webseed URL:%s has sent %d bad pieces and has been disabled.",
              w->base_url, MAX_BAD_PIECES_PER_WEBSEED);
    
      }
    }
}

static struct evbuffer *
make_url( tr_webseed * w, const tr_file * file )
{
    struct evbuffer * buf = evbuffer_new( );

    evbuffer_add( buf, w->base_url, w->base_url_len );

    /* if url ends with a '/', add the torrent name */
    if( w->base_url[w->base_url_len - 1] == '/' && file->name )
        tr_http_escape( buf, file->name, strlen(file->name), false );

    return buf;
}

static void
task_request_next_chunk( struct tr_webseed_task * t )
{
    tr_webseed * w = t->webseed;
    tr_torrent * tor = tr_torrentFindFromId( w->session, w->torrent_id );
    if( tor && tor->isRunning && !tor->isStopping && !tr_torrentIsSeed( tor ) && !w->is_stopping )
    {
        char range[64];
        char ** urls = t->webseed->file_urls;

        const tr_info * inf = tr_torrentInfo( tor );
        const uint64_t remain = t->length - t->blocks_done * tor->blockSize
                                - evbuffer_get_length( t->content );

        const uint64_t total_offset = tr_pieceOffset( tor, t->piece_index,
                                                           t->piece_offset,
                                                           t->length - remain );
        const tr_piece_index_t step_piece = total_offset / inf->pieceSize;
        const uint64_t step_piece_offset
                               = total_offset - ( inf->pieceSize * step_piece );

        tr_file_index_t file_index;
        const tr_file * file;
        uint64_t file_offset;
        uint64_t this_pass;

        if( ( t->piece_index >= tor->info.pieceCount )
            || ( total_offset >= tor->info.totalSize )
            || ( step_piece >= tor->info.pieceCount )
            || ( tr_pieceOffset( tor, step_piece, step_piece_offset, 0 ) >= tor->info.totalSize ) )
        {
            tr_tordbg( tor, "webseed paused - next chunk aborted - piece index:%d - piece offset:%d - total size: %"PRIu64" bytes",
                       t->piece_index, t->piece_offset, tor->info.totalSize );
            w->wait_factor = MAX_WAIT_FACTOR;
            return;
        }

        if( w->wait_factor >= MAX_WAIT_FACTOR )
        {
            if( !tor )
               tr_dbg( "?unknown? ID %d webseed MAX WAIT FACTOR next chunk aborted", w->torrent_id );
            else
                tr_tordbg( tor, "MAX WAIT FACTOR - next chunk aborted - run flag:%d - stop flag:%d -",
                           tor->isRunning, tor->isStopping );
            return;
        }

        tr_ioFindFileLocation( tor, step_piece, step_piece_offset,
                                    &file_index, &file_offset );
        file = &inf->files[file_index];
        this_pass = MIN( remain, file->length - file_offset );

        if( !urls[file_index] )
            urls[file_index] = evbuffer_free_to_str( make_url( t->webseed, file ) );

        tr_snprintf( range, sizeof range, "%"PRIu64"-%"PRIu64,
                     file_offset, file_offset + this_pass - 1 );
        t->web_task = tr_webRunWithBuffer( w->session, w->torrent_id, urls[file_index],
                                           range, NULL, web_response_func, t, t->content );
    }
    else
    {
        if( !tor )
           tr_dbg( "?unknown? ID %d webseed deleted torrent next chunk aborted", w->torrent_id );
        else
            tr_tordbg( tor, "webseed paused - next chunk aborted - run flag:%d - stop flag:%d -",
                       tor->isRunning, tor->isStopping );
        w->wait_factor = MAX_WAIT_FACTOR;
    }
}

bool
tr_webseedGetSpeed_Bps( const tr_webseed * w, uint64_t now, int * setme_Bps )
{
    const bool is_active = webseed_has_tasks( w );
    *setme_Bps = is_active ? tr_bandwidthGetPieceSpeed_Bps( &w->bandwidth, now, TR_DOWN ) : 0;
    return is_active;
}

bool
tr_webseedIsActive( const tr_webseed * w )
{
    int Bps = 0;
    return tr_webseedGetSpeed_Bps( w, tr_time_msec(), &Bps ) && ( Bps > 0 );
}

/***
****
***/

static void
webseed_timer_func( evutil_socket_t foo UNUSED, short bar UNUSED, void * vw )
{
    tr_webseed * w = vw;
    if( w->retry_tickcount )
        ++w->retry_tickcount;
    on_idle( w );
    tr_timerAddMsec( w->timer, TR_IDLE_TIMER_MSEC );
}

tr_webseed*
tr_webseedNew( struct tr_torrent  * tor,
               const char         * url,
               tr_peer_callback   * callback,
               void               * callback_data )
{
    tr_webseed * w = tr_new0( tr_webseed, 1 );
    tr_peer * peer = &w->parent;
    const tr_info * inf = tr_torrentInfo( tor );

    /* construct parent class */
    tr_peerConstruct( peer );
    peer->peerIsChoked = true;
    peer->clientIsInterested = !tr_torrentIsSeed( tor );
    peer->client = tr_strdup( "webseed" );
    tr_bitfieldSetHasAll( &peer->have );
    tr_peerUpdateProgress( tor, peer );

    w->wait_factor = 1;
    w->torrent_id = tr_torrentId( tor );
    w->session = tor->session;
    w->base_url_len = strlen( url );
    w->base_url = tr_strndup( url, w->base_url_len );
    w->callback = callback;
    w->callback_data = callback_data;
    w->file_urls = tr_new0( char *, inf->fileCount );
    //tr_rcConstruct( &w->download_rate );
    tr_bandwidthConstruct( &w->bandwidth, tor->session, &tor->bandwidth );
    tr_bitfieldConstruct (&w->blame, tor->blockCount);
    w->timer = evtimer_new( w->session->event_base, webseed_timer_func, w );
    tr_timerAddMsec( w->timer, TR_IDLE_TIMER_MSEC );
    return w;
}

void
tr_webseedFree( tr_webseed * w )
{
    if( w )
    {

    struct tr_torrent * tor;
    tor = tr_torrentFindFromId( w->session, w->torrent_id );

    if( tor && tor->isRunning )
        {
        tr_tordbg( tor, "should not be running - skipping free webseeds - run flag:%d - stop flag:%d - w stop flag is %d ",
                   tor->isRunning, tor->isStopping, w->is_stopping );
        w->wait_factor = MAX_WAIT_FACTOR;
        return;
        }

    if( !tor )
        {
        tr_dbg( "??free webseed - torrent is empty! - w stop flag is %d ", w->is_stopping );
        // torrent should not be empty right now
        w->wait_factor = MAX_WAIT_FACTOR;
        return;
        }

    if( webseed_has_tasks( w ) )
        w->is_stopping = true; // just wait for idle timer to hit then free it
    else
        webseed_free( w );
    }
    else tr_dbg( "??free webseed - webseed is empty!" );
}
