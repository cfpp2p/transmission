/******************************************************************************
 *
 * $Id$
 *
 * Copyright (c) Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <assert.h>
#include <limits.h>
#include <stdarg.h>

#include <event2/buffer.h>
#include <event2/http.h>

#include "transmission.h"
#include "announcer.h"
#include "crypto.h"
#include "list.h"
#include "ptrarray.h"
#include "resolver.h"
#include "session.h"
#include "torrent.h"
#include "utils.h"
#include "web.h"

#include "announcer-common.h"
#include "announcer-udp.h"

enum au_protocol_constants
{
    AUC_EVENT_NONE = 0,
    AUC_EVENT_COMPLETED = 1,
    AUC_EVENT_STARTED = 2,
    AUC_EVENT_STOPPED = 3,

    AUC_ACTION_CONNECT = 0,
    AUC_ACTION_ANNOUNCE = 1,
    AUC_ACTION_SCRAPE = 2,
    AUC_ACTION_ERROR = 3,

    AUC_CONNECTION_EXPIRE_TIME = 60,
    AUC_RESPONSE_TIMEOUT_INIT = 15, 
    AUC_MAXIMUM_RETRY_COUNT = 8 
};

#define AUC_PROTOCOL_ID 0x41727101980LL

/***
****
***/

typedef struct
{
    int64_t protocol_id; /* called "connection_id" in bep15 */
    int32_t action;
    int32_t transaction_id;
} TR_GNUC_PACKED
auP_connect_request;

typedef struct
{
    int64_t connection_id;
    int32_t action;
    int32_t transaction_id;
} TR_GNUC_PACKED
auP_request_header;

typedef struct
{
    auP_request_header hdr;
    int8_t info_hash[20];
    int8_t peer_id[20];
    int64_t downloaded;
    int64_t left;
    int64_t uploaded;
    int32_t event;
    uint32_t ip_address;
    uint32_t key;
    int32_t num_want;
    uint16_t port;
    uint16_t extensions;
} TR_GNUC_PACKED
auP_announce_request;

typedef struct
{
    auP_request_header hdr;
    /* (int8_t info_hash[20];)* */
} TR_GNUC_PACKED
auP_scrape_request;

/***
****
***/

typedef struct
{
    int32_t action;
    int32_t transaction_id;
} TR_GNUC_PACKED
auP_response_header;

typedef struct
{
    auP_response_header hdr;
    int64_t connection_id;
} TR_GNUC_PACKED
auP_connect_response;

typedef struct
{
    auP_response_header hdr;
    int32_t interval;
    int32_t leechers;
    int32_t seeders;
    /* (int32_t ip; uint16_t port;)* */
} TR_GNUC_PACKED
auP_announce_response;

typedef struct
{
    int32_t complete;
    int32_t downloaded;
    int32_t incomplete;
} TR_GNUC_PACKED
auP_scrape_item;

typedef struct
{
    auP_response_header hdr;
    /* (auP_scrape_item item;)* */
} TR_GNUC_PACKED
auP_scrape_response;

typedef struct
{
    auP_response_header hdr;
    /* char * error_string; */
} TR_GNUC_PACKED
auP_error_response;

/***
****
***/

typedef int32_t tnid_t;
typedef int64_t conid_t;
typedef uint32_t annkey_t;

typedef struct au_transaction au_transaction;
typedef struct au_state au_state;

static void au_transaction_log_full( au_transaction * t,
                                     tr_msg_level lev,
                                     int line,
                                     const char * fmt, ... )
                                     TR_GNUC_PRINTF( 4, 5 );
static void au_transaction_error_full( au_transaction * t,
                                       int line,
                                       const char * fmt, ... )
                                       TR_GNUC_PRINTF( 3, 4 );
static void au_state_error( au_state * s,
                            const char * fmt, ... )
                            TR_GNUC_PRINTF( 2, 3 );

#define au_transaction_log( t, lev, ... ) do { \
    if( tr_msgLoggingIsActive( lev ) ) \
        au_transaction_log_full( t, lev, __LINE__, __VA_ARGS__ ); \
} while( 0 )

#define au_transaction_error( t, ... ) do { \
    au_transaction_error_full( t, __LINE__, __VA_ARGS__ ); \
} while( 0 )

static void au_state_send( au_state * s, au_transaction * t );
static tr_bool au_state_connect( au_state * s );
static tr_session * au_state_get_session( const au_state * s );
static const char * au_state_get_endpoint( const au_state * s );

static au_transaction * au_context_get_transaction( au_context * c, tnid_t id );
static void au_context_add_transaction( au_context * c, au_transaction * t );
static void au_context_transmit( au_context * c, au_transaction * t );
static tr_session * au_context_get_session( const au_context * c );

#define ALLOC_PKT( bufptr, pktptr, pkttype ) do { \
    static const pkttype _PKT_init; \
    bufptr = evbuffer_new( ); \
    evbuffer_add( bufptr, &_PKT_init, sizeof( pkttype ) ); \
    pktptr = (pkttype *) evbuffer_pullup( bufptr, -1 ); \
    assert( evbuffer_get_length( bufptr ) == sizeof( pkttype ) ); \
} while( 0 )

/***
****
***/

struct au_transaction
{
     tnid_t id;
     au_state * state; /* not owned */
     struct evbuffer * pkt;
     time_t send_ts;
     int retries;
     tr_web_done_func * callback;
     void * cbdata;
     char * errstr;
};

static au_transaction *
au_transaction_new( au_state * s, struct evbuffer * pkt )
{
    au_transaction * t;
    auP_request_header * hdr;

    assert( evbuffer_get_length( pkt ) >= sizeof( *hdr ) );

    t = tr_new0( au_transaction, 1 );
    tr_cryptoRandBuf( &t->id, sizeof( t->id ) );
    t->state = s;
    t->pkt = pkt; /* transfer ownership */

    hdr = (auP_request_header *) evbuffer_pullup( pkt, -1 );
    hdr->transaction_id = htonl( t->id );

    return t;
}

static void
au_transaction_free( au_transaction * t )
{
    if( !t )
        return;
    if( t->pkt )
        evbuffer_free( t->pkt );
    tr_free( t->errstr );
    memset( t, 0, sizeof( *t ) );
    tr_free( t );
}

static int
au_transaction_cmp( const void * va, const void * vb )
{
    const au_transaction * a = va;
    const au_transaction * b = vb;
    if( a->id < b->id )
        return -1;
    if( a->id > b->id )
        return 1;
    return 0;
}

static tr_bool
au_transaction_is_valid( const au_transaction * t )
{
    return t && t->id != 0 && t->state != 0;
}

static int
au_transaction_get_action( au_transaction * t )
{
    const auP_request_header * hdr;
    if( !au_transaction_is_valid( t ) || !t->pkt
        || evbuffer_get_length( t->pkt ) < sizeof( *hdr ) )
    {
        return -1;
    }
    hdr = (const auP_request_header *) evbuffer_pullup( t->pkt, -1 );
    return ntohl( hdr->action );
}

static void
au_transaction_log_full( au_transaction * t, tr_msg_level lev,
                         int line, const char * fmt, ... )
{
    char buf[1024], loc[256];
    const char * endpoint, * mfmt;
    va_list ap;

    va_start( ap, fmt );
    evutil_vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );
    
    /*Testing*/
    if( t->state != NULL ) {
    endpoint = t ? au_state_get_endpoint( t->state ) : NULL;
    if( endpoint )
        {
        tr_snprintf( loc, sizeof( loc ),
                     _( "UDP Announcer (%s)" ), endpoint );
        }
    }
    else
    {
        tr_strlcpy( loc, _( "UDP Announcer" ), sizeof( loc ) );
    }

    switch( au_transaction_get_action( t ) )
    {
        case AUC_ACTION_CONNECT:
            mfmt = _( "Connect transaction (ID %08x): %s" );
            break;
        case AUC_ACTION_ANNOUNCE:
            mfmt = _( "Announce transaction (ID %08x): %s" );
            break;
        case AUC_ACTION_SCRAPE:
            mfmt = _( "Scrape transaction (ID %08x): %s" );
            break;
        default:
            mfmt = _( "Transaction (ID %08x): %s" );
            break;
    }

    tr_msg( __FILE__, line, lev, loc, mfmt, t ? t->id : 0, buf );
}

static void
au_transaction_error_full( au_transaction * t, int line,
                           const char * fmt, ... )
{
    char buf[1024];
    va_list ap;

    va_start( ap, fmt );
    evutil_vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    if( !t )
        return;
    if( !t->errstr )
        return;

    if( t != NULL ) 
    {
        if(t->errstr != NULL )
            tr_free( t->errstr );
        t->errstr = tr_strdup( buf );
        au_transaction_log_full( t, TR_MSG_ERR, line, "%s", t->errstr );
    }
}

static tr_bool
au_transaction_has_error( const au_transaction * t )
{
    return t->errstr != NULL;
}

static tr_bool
au_transaction_has_timeout( const au_transaction * t )
{
    return t->retries >= AUC_MAXIMUM_RETRY_COUNT;
}

static tr_bool
au_transaction_is_inactive( const au_transaction * t )
{
    return !au_transaction_is_valid( t )
        || au_transaction_has_error( t )
        || au_transaction_has_timeout( t );
}

static void
au_transaction_notify( au_transaction * t, const void * data, size_t len )
{
    tr_session * session;
    int code;

    if( !t->callback )
        return;

    if( au_transaction_has_error( t ) )
        code = HTTP_INTERNAL;
    else if( au_transaction_has_timeout( t ) )
        code = 0;
    else
        code = HTTP_OK;

    session = au_state_get_session( t->state );
    t->callback( session, code, data, len, t->cbdata );
    t->callback = NULL;
    t->cbdata = NULL;
}

static time_t
au_transaction_get_ttl( const au_transaction * t )
{
    return AUC_RESPONSE_TIMEOUT_INIT * ( 1 << t->retries );
}

static void
au_transaction_check_timeout( au_transaction * t, time_t now )
{
    if( !au_transaction_is_valid( t ) )
        return;

    if( au_transaction_has_timeout( t ) )
    {
        au_transaction_notify( t, NULL, 0 );
        return;
    }

    if( !t->send_ts )
        return;

    if( now - t->send_ts < au_transaction_get_ttl( t ) )
        return;

    t->retries++;

    if( au_transaction_has_timeout( t ) )
    {
        au_transaction_notify( t, NULL, 0 );
    }
    else
    {
        au_transaction_log( t, TR_MSG_DBG,
            _( "Retrying after timeout on attempt %d "
               "(next timeout in %d seconds)" ),
            t->retries, (int) au_transaction_get_ttl( t ) );
        au_state_send( t->state, t );
    }
}

static void
au_transaction_sent( au_transaction * t )
{
    t->send_ts = tr_time( );
}

static void
au_transaction_set_callback( au_transaction   * t,
                             tr_web_done_func   cb,
                             void             * cbdata )
{
    t->callback = cb;
    t->cbdata = cbdata;
}

/***
****
***/

struct au_state
{
    au_context * context; /* not owned */
    char * endpoint;
    tr_bool resolved;
    tr_bool resolving;
    tr_address addr;
    tr_port port;
    conid_t con_id;
    time_t con_ts;
    tnid_t con_tid;
    annkey_t key;
    tr_list * queue; /* au_transaction, not owned */
};

static au_state *
au_state_new( au_context * c, const char * tracker_host_port )
{
    au_state * s = tr_new0( au_state, 1 );
    s->context = c;
    s->endpoint = tr_strdup( tracker_host_port );
    tr_cryptoRandBuf( &s->key, sizeof( s->key ) );
    return s;
}

static void
au_state_free( au_state * s )
{
    if( !s )
        return;
    tr_free( s->endpoint );
    tr_list_free( &s->queue, NULL );
    memset( s, 0, sizeof( *s ) );
    tr_free( s );
}

static int
au_state_cmp( const void * va, const void * vb )
{
    const au_state * a = va;
    const au_state * b = vb;
    return strcmp( a->endpoint, b->endpoint );
}

static tr_session *
au_state_get_session( const au_state * s )
{
    return au_context_get_session( s->context );
}

static const char *
au_state_get_endpoint( const au_state * s )
{
    return s ? s->endpoint : NULL;
}

static tr_bool
au_state_is_connecting( const au_state * s )
{
    return s->con_tid != 0;
}

static tr_bool
au_state_is_connected( const au_state * s )
{
    return s->resolved && s->con_id != 0;
}

static void
au_state_flush( au_state * s )
{
    au_transaction * t;

    if( !s->queue )
        return;

    if( !au_state_connect( s ) )
        return;

    while( ( t = tr_list_pop_front( &s->queue ) ) )
		au_state_send( s, t );
}

static void
au_state_check_connection( au_state * s, time_t now )
{
    au_transaction * t;
    char * errstr = NULL;

    if( au_state_is_connected( s )
        && now - s->con_ts > AUC_CONNECTION_EXPIRE_TIME )
    {
        s->con_id = 0;
        return;
    }

    if( !au_state_is_connecting( s ) )
        return;

    t = au_context_get_transaction( s->context, s->con_tid );
    if( !au_transaction_is_inactive( t ) )
        return;

    if( au_transaction_is_valid( t ) )
    {
        if( au_transaction_has_error( t ) )
        {
            errstr = tr_strdup_printf(
                _( "Connection error: %s" ), t->errstr );
        }
        else if( au_transaction_has_timeout( t ) )
        {
            errstr = tr_strdup( _( "Connection timed out" ) );
        }
    }
    else
    {
        errstr = tr_strdup( _( "Connection failed" ) );
    }

    while( ( t = tr_list_pop_front( &s->queue ) ) )
        au_transaction_error( t, "%s", errstr );
		
	if ( errstr != NULL ) /* Added 2/22/12 */
        tr_free( errstr );
	
        s->con_tid = 0;
}

static void
au_state_error( au_state * s, const char * fmt, ... )
{
    au_transaction * t;
    char buf[1024];
    va_list ap;

    va_start( ap, fmt );
    evutil_vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    while( ( t = tr_list_pop_front( &s->queue ) ) )
        au_transaction_error( t, "%s", buf );
}

static void
resolver_callback( const char       * err,
                   const tr_address * addr,
                   void             * user_data )
{
    au_state * s = user_data;
    if( !s || !s->resolving )
        return;
    s->resolving = FALSE;

    if( err )
    {
        au_state_error( s, _( "DNS lookup for %1$s failed: %2$s" ),
                        s->endpoint, err );
        return;
    }

    if( !tr_isValidTrackerAddress( addr ) )
    {
        char astr[128];
        au_state_error( s,
            _( "DNS lookup for %1$s returned invalid address: %2$s" ),
            s->endpoint, tr_ntop( addr, astr, sizeof( astr ) ) );
        return;
    }

    s->addr = *addr;
    s->resolved = TRUE;
    au_state_flush( s );
}

static tr_bool
au_state_lookup( au_state * s )
{
    char buf[512], * portstr;
    tr_address * addr;
    int port;

    if( s->resolved )
        return TRUE;

    if( s->resolving )
        return FALSE;

    tr_strlcpy( buf, s->endpoint, sizeof( buf ) );
    portstr = strchr( buf, ':' );
    if( !portstr )
    {
        au_state_error( s, _( "Invalid UDP tracker address \"%s\" "
                              "(expecting \"hostname:port\")" ),
                        s->endpoint );
        return FALSE;
    }

    *portstr++ = '\0';
    port = atoi( portstr );
    if( !( 0 < port && port <= USHRT_MAX ) )
    {
        au_state_error( s, _( "Tracker %1$s has invalid port \"%2$s\"" ),
                        s->endpoint, portstr );
        return FALSE;
    }
    s->port = port;

    addr = tr_pton( buf, &s->addr );
    if( addr )
    {
        if( addr->type != TR_AF_INET )
        {
            au_state_error( s, _( "Address type not supported: %s" ),
                            s->endpoint );
            return FALSE;
        }

        s->resolved = TRUE;
        return TRUE;
    }

    s->resolving = TRUE;
    tr_resolve_address( au_state_get_session( s ),
                        buf, portstr, TR_AF_INET,
                        resolver_callback, s );
    return FALSE;
}

static tr_bool
au_state_connect( au_state * s )
{
    au_transaction * t;
    struct evbuffer * pkt;
    auP_connect_request * req;

    if( au_state_is_connected( s ) )
        return TRUE;

    if( !au_state_lookup( s ) )
        return FALSE;

    if( au_state_is_connecting( s ) )
        return FALSE;

    ALLOC_PKT( pkt, req, auP_connect_request );

    req->protocol_id = tr_htonll( AUC_PROTOCOL_ID );
    req->action = htonl( AUC_ACTION_CONNECT );

    t = au_transaction_new( s, pkt );
    s->con_tid = t->id;
    au_context_add_transaction( s->context, t );
    au_context_transmit( s->context, t );

    return FALSE;
}

static void
au_state_establish( au_state * s, au_transaction * t, conid_t cid )
{
    if( s->con_tid != t->id || !cid )
        return;

    /* FIXME: What if we are already connected? */

    s->con_id = cid;
    s->con_ts = tr_time( );
    s->con_tid = 0;
    au_state_flush( s );
}

static void
au_state_send( au_state * s, au_transaction * t )
{
    auP_request_header * hdr;

    if( t->id == s->con_tid )
    {
        au_context_transmit( s->context, t );
        return;
    }

    if( !au_state_connect( s ) )
    {
        tr_list_append( &s->queue, t );
        return;
    }
    
    /* if( !t->pkt )
        return; */

    hdr = (auP_request_header *) evbuffer_pullup( t->pkt, -1 );
    hdr->connection_id = tr_htonll( s->con_id );

    au_context_transmit( s->context, t );
}

static void
au_state_get_destination( const au_state * s,
                          tr_address     * setme_addr,
                          tr_port        * setme_port )
{
    if( setme_addr )
        *setme_addr = s->addr;
    if( setme_port )
        *setme_port = s->port;
}

/***
****
***/

struct au_context
{
    tr_session * session;     /* not owned */
    tr_ptrArray transactions; /* au_transaction */
    tr_ptrArray states;       /* au_state */
};

au_context *
au_context_new( tr_session * session )
{
    au_context * c = tr_new0( au_context, 1 );
    c->session = session;
    c->transactions = TR_PTR_ARRAY_INIT;
    c->states = TR_PTR_ARRAY_INIT;
    return c;
}

void
au_context_free( au_context * c )
{
    if( !c )
        return;
    tr_ptrArrayDestruct( &c->transactions,
                         (PtrArrayForeachFunc) au_transaction_free );
    tr_ptrArrayDestruct( &c->states,
                         (PtrArrayForeachFunc) au_state_free );
    memset( c, 0, sizeof( *c ) );
    tr_free( c );
}

static tr_session *
au_context_get_session( const au_context * c )
{
    return c->session;
}

static void
au_context_add_transaction( au_context * c, au_transaction * t )
{
    tr_ptrArrayInsertSorted( &c->transactions, t, au_transaction_cmp );
}

static void
au_context_remove_transaction( au_context * c, au_transaction * t )
{
    tr_ptrArrayRemoveSorted( &c->transactions, t, au_transaction_cmp );
}

void
au_context_periodic( au_context * c )
{
    time_t now = tr_time( );
    tr_list * inactive = NULL;
    au_transaction * t;
    int i;

    for( i = 0; i < tr_ptrArraySize( &c->transactions ); ++i )
    {
        t = tr_ptrArrayNth( &c->transactions, i );
        au_transaction_check_timeout( t, now );
        if( au_transaction_has_error( t ) )
            au_transaction_notify( t, NULL, 0 );
        if( au_transaction_is_inactive( t ) )
            tr_list_append( &inactive, t );
    }

    for( i = 0; i < tr_ptrArraySize( &c->states ); ++i )
    {
        au_state * s = tr_ptrArrayNth( &c->states, i );
        au_state_check_connection( s, now );
    }

    while( ( t = tr_list_pop_front( &inactive ) ) )
    {
        au_context_remove_transaction( c, t );
        au_transaction_free( t );
    }
}

static au_transaction *
au_context_get_transaction( au_context * c, tnid_t id )
{
    au_transaction key;
    key.id = id;
    return tr_ptrArrayFindSorted( &c->transactions, &key, au_transaction_cmp );
}

static au_state *
au_context_get_state( au_context * c, const char * endpoint )
{
    au_state key, * s;
    key.endpoint = tr_strdup( endpoint );
    s = tr_ptrArrayFindSorted( &c->states, &key, au_state_cmp );
    if( !s )
    {
        s = au_state_new( c, endpoint );
        tr_ptrArrayInsertSorted( &c->states, s, au_state_cmp );
    }
    tr_free( key.endpoint );
    return s;
}

static void
au_context_transmit( au_context * c, au_transaction * t )
{
    int socket = c->session->udp_socket;
    const void * data = evbuffer_pullup( t->pkt, -1 );
    size_t len = evbuffer_get_length( t->pkt );
    tr_address addr;
    tr_port port;

    au_state_get_destination( t->state, &addr, &port );

    if( tr_netSendTo( socket, data, len, &addr, port ) == -1 )
    {
        const int err = evutil_socket_geterror( socket );
        const char * errstr = evutil_socket_error_to_string( err );
        char astr[128];
        au_transaction_error( t,
            _( "Failed to send UDP packet to %s:%d: %s" ),
            tr_ntop( &addr, astr, sizeof( astr ) ), port, errstr );
        return;
    }
    au_transaction_sent( t );
}

/***
****
***/

static int
get_event_id( const char * evstr )
{
    if( !strcmp( evstr, "started" ) )
        return AUC_EVENT_STARTED;
    if( !strcmp( evstr, "stopped" ) )
        return AUC_EVENT_STOPPED;
    if( !strcmp( evstr, "completed" ) )
        return AUC_EVENT_COMPLETED;
    /* FIXME: What about event "paused"? */
    return AUC_EVENT_NONE;
}

static struct evbuffer *
create_announce( tr_announcer     * announcer,
                 const tr_torrent * tor,
                 const tr_tier    * tier,
                 const char       * evstr )
{
    const tr_tracker_item * tracker = tier->currentTracker;
    au_context * c = announcer->udpctx;
    au_state * s = au_context_get_state( c, tracker->hostname );
    auP_announce_request * req;
    struct evbuffer * pkt;
    int numwant, event;
    tr_port port;

    assert( tor->peer_id != NULL );
    assert( strlen( (const char *) tor->peer_id ) == sizeof( req->peer_id ) );

    ALLOC_PKT( pkt, req, auP_announce_request );
    req->hdr.action = htonl( AUC_ACTION_ANNOUNCE );

    memcpy( req->info_hash, tor->info.hash, sizeof( req->info_hash ) );
    memcpy( req->peer_id, tor->peer_id, sizeof( req->peer_id ) );

    req->downloaded = tr_htonll( tier->byteCounts[TR_ANN_DOWN] );
    req->left = tr_htonll( tr_cpLeftUntilComplete( &tor->completion ) );
    req->uploaded = tr_htonll( tier->byteCounts[TR_ANN_UP] );

    event = get_event_id( evstr );
    req->event = htonl( event );
    req->ip_address = 0; /* Have tracker use packet sender. */
    req->key = htonl( s->key );

    if( !strcmp( evstr, "stopped" ) )
        numwant = 0;
    else
        numwant = NUMWANT;
    req->num_want = htonl( numwant );
    port = tr_sessionGetPublicPeerPort( announcer->session );
    req->port = htons( port );

    return pkt;
}

static struct evbuffer *
create_scrape( tr_announcer          * a UNUSED,
               const tr_torrent      * tor,
               const tr_tracker_item * tracker UNUSED )
{
    auP_scrape_request * req;
    struct evbuffer * pkt;

    ALLOC_PKT( pkt, req, auP_scrape_request );
    req->hdr.action = htonl( AUC_ACTION_SCRAPE );

    /* FIXME: What about scraping multiple torrents at the same time? */

    evbuffer_add( pkt, tor->info.hash, sizeof( tor->info.hash ) );
    return pkt;
}

struct evbuffer *
au_create_stop( tr_announcer     * announcer,
                const tr_torrent * tor,
                const tr_tier    * tier )
{
    assert( announcer != NULL );
    assert( tr_isTorrent( tor ) );
    assert( tier != NULL );
    assert( tier->currentTracker != NULL );
    assert( tier->currentTracker->type == TR_TRACKER_TYPE_UDP );

    return create_announce( announcer, tor, tier, "stopped" );
}

void
au_send_stop( tr_announcer    * announcer,
              const char      * endpoint,
              struct evbuffer * pkt )
{
    au_context * c;
    au_transaction * t;
    au_state * s;

    assert( announcer != NULL );
    assert( endpoint != NULL );
    assert( pkt != NULL );

    c = announcer->udpctx;
    s = au_context_get_state( c, endpoint );
    t = au_transaction_new( s, pkt );
    au_context_add_transaction( c, t );
    au_state_send( s, t );
}

void
au_send_announce( tr_announcer     * announcer,
                  const tr_torrent * tor,
                  const tr_tier    * tier,
                  const char       * evstr,
                  tr_web_done_func * callback,
                  void             * cbdata )
{
    const tr_tracker_item * tracker;
    au_context * c;
    struct evbuffer * pkt;
    au_transaction * t;
    au_state * s;

    assert( announcer != NULL );
    assert( tr_isTorrent( tor ) );
    assert( tier != NULL );
    assert( tier->currentTracker != NULL );

    tracker = tier->currentTracker;

    assert( tracker != NULL );
    assert( tracker->type == TR_TRACKER_TYPE_UDP );

    c = announcer->udpctx;
    s = au_context_get_state( c, tracker->hostname );

    if( tor->peer_id != NULL )
	{
        pkt = create_announce( announcer, tor, tier, evstr );
        t = au_transaction_new( s, pkt );
        au_transaction_set_callback( t, callback, cbdata );
        au_context_add_transaction( c, t );
        au_state_send( s, t );
	}
}

void
au_send_scrape( tr_announcer     * announcer,
                const tr_torrent * tor,
                const tr_tier    * tier,
                tr_web_done_func * callback,
                void             * cbdata )
{
    const tr_tracker_item * tracker;
    au_context * c;
    struct evbuffer * pkt;
    au_transaction * t;
    au_state * s;

    assert( announcer != NULL );
    assert( tr_isTorrent( tor ) );
    assert( tier != NULL );
    assert( tier->currentTracker != NULL );

    tracker = tier->currentTracker;

    assert( tracker != NULL );
    assert( tracker->type == TR_TRACKER_TYPE_UDP );

    c = announcer->udpctx;
    s = au_context_get_state( c, tracker->hostname );
    pkt = create_scrape( announcer, tor, tracker );
    t = au_transaction_new( s, pkt );
    au_transaction_set_callback( t, callback, cbdata );
    au_context_add_transaction( c, t );
    au_state_send( s, t );
}

tr_bool
au_parse_announce( tr_tier * tier, const char * data,
                   size_t len, tr_bool * got_scrape )
{
    tr_tracker_item * tracker = tier->currentTracker;
    const auP_announce_response * res;
    int action, count, leechers, seeders;

    res = (const auP_announce_response *) data;
    action = ntohl( res->hdr.action );

    publishErrorClear( tier );
    if( got_scrape )
        *got_scrape = FALSE;

    if( action == AUC_ACTION_ERROR )
    {
        const size_t hdrsz = sizeof( auP_response_header );
        const size_t rlen = sizeof( tier->lastAnnounceStr );
        assert( len > hdrsz );
        data += hdrsz;
        len -= hdrsz;
        tr_strlcpy( tier->lastAnnounceStr, data, MIN( len, rlen ) );
        publishMessage( tier, tier->lastAnnounceStr, TR_TRACKER_ERROR );
        return FALSE;
    }

    tier->announceIntervalSec = ntohl( res->interval );
    leechers = ntohl( res->leechers );
    seeders = ntohl( res->seeders );
    tracker->leecherCount = leechers;
    tracker->seederCount = seeders;

    if( tier->lastAnnounceStr[0] == '\0' )
        tr_strlcpy( tier->lastAnnounceStr, _( "Success" ),
                    sizeof( tier->lastAnnounceStr ) );

    count = publishPeersCompact( tier, seeders, leechers,
                                 data + sizeof( *res ),
                                 len - sizeof( *res ) );
    tier->lastAnnouncePeerCount = count;

    return TRUE;
}

tr_bool
au_parse_scrape( tr_tier * tier, const char * data, size_t len,
                 char * result, size_t resultlen )
{
    tr_tracker_item * tracker = tier->currentTracker;
    const auP_scrape_response * res;
    const auP_scrape_item * item;
    int action;

    res = (const auP_scrape_response *) data;
    action = ntohl( res->hdr.action );

    if( action == AUC_ACTION_ERROR )
    {
        const size_t hdrsz = sizeof( auP_response_header );
        assert( len > hdrsz );
        data += hdrsz;
        len -= hdrsz;
        tr_strlcpy( result, data, MIN( resultlen, len ) );
        return FALSE;
    }

    data += sizeof( *res );
    len -= sizeof( *res );

    if( len < sizeof( *item ) )
    {
        tr_strlcpy( result, _( "Error parsing response" ), resultlen );
        return FALSE;
    }

    publishErrorClear( tier );

    /* FIXME: What about multiple scrapes at once? */

    item = (const auP_scrape_item *) data;
    tracker->seederCount = ntohl( item->complete );
    tracker->leecherCount = ntohl( item->incomplete );
    tracker->downloadCount = ntohl( item->downloaded );

    tr_strlcpy( result, _( "Success" ), resultlen );
    return TRUE;
}

/***
****
***/

static void
handle_connect( au_transaction * t, const uint8_t * data, size_t len )
{
    const auP_connect_response * res;
    au_state * s = t->state;

    if( len < sizeof( *res ) )
    {
        au_transaction_error( t,
            _( "Malformed connect response: expecting length "
               "%1$zu but got %2$zu" ), sizeof( *res ), len );
        return;
    }

    res = (const auP_connect_response *) data;

    au_state_establish( s, t, tr_ntohll( res->connection_id ) );
}

static void
handle_announce( au_transaction * t, const uint8_t * data, size_t len )
{
    const auP_announce_response * res;

    if( len < sizeof( *res ) )
    {
        au_transaction_error( t,
            _( "Malformed announce response: expecting length "
               "at least %1$zu but got %2$zu" ), sizeof( *res ), len );
        return;
    }

    au_transaction_notify( t, data, len );
    /* Continued in au_parse_announce(). */
}

static void
handle_scrape( au_transaction * t, const uint8_t * data, size_t len )
{
    const auP_scrape_response * res;

    if( len < sizeof( *res ) )
    {
        au_transaction_error( t,
            _( "Malformed scrape response: expecting length "
               "at least %1$zu but got %2$zu" ), sizeof( *res ), len );
        return;
    }

    au_transaction_notify( t, data, len );
    /* Continued in au_parse_scrape(). */
}

static void
handle_error( au_transaction * t, const uint8_t * data, size_t len )
{
    const auP_error_response * res;

    if( len <= sizeof( *res ) )
    {
        au_transaction_error( t,
            _( "Malformed error response: expecting length "
               "greater than %1$zu but got %2$zu" ), sizeof( *res ), len );
        return;
    }

    au_transaction_notify( t, data, len );
    /* Continued in au_parse_announce() or au_parse_scrape(). */
}

tr_bool
tr_announcerHandleUDP( tr_announcer     * announcer,
                       const uint8_t    * data,
                       size_t             len,
                       const tr_address * from_addr UNUSED,
                       tr_port            from_port UNUSED )
{
    au_context * c = announcer->udpctx;
    const auP_response_header * hdr;
    au_transaction * t;
    int action;

    if( len < sizeof( *hdr ) )
        return FALSE;

    hdr = (const auP_response_header *) data;

    t = au_context_get_transaction( c, ntohl( hdr->transaction_id ) );
    if( !t )
        return FALSE;
    au_context_remove_transaction( c, t );

    action = ntohl( hdr->action );
    switch( action )
    {
        case AUC_ACTION_CONNECT:  handle_connect ( t, data, len ); break;
        case AUC_ACTION_ANNOUNCE: handle_announce( t, data, len ); break;
        case AUC_ACTION_SCRAPE:   handle_scrape  ( t, data, len ); break;
        case AUC_ACTION_ERROR:    handle_error   ( t, data, len ); break;
        default:
            au_transaction_error( t,
                _( "Unsupported action type: %d" ), action );
            break;
    }

    if( au_transaction_has_error( t ) )
        au_transaction_notify( t, NULL, 0 );
    au_transaction_free( t );

    return TRUE;
}
