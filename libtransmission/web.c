/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: web.c 13112 2011-12-14 05:58:23Z jordan $
 */

#include <string.h> /* strlen(), strstr() */
#include <stdlib.h> /* getenv() */

#ifdef WIN32
  #include <ws2tcpip.h>
#else
  #include <sys/select.h>
#endif

#include <curl/curl.h>

#include <event2/buffer.h>

#include "transmission.h"
#include "net.h" /* tr_address */
#include "torrent.h"
#include "platform.h" /* mutex */
#include "session.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"
#include "version.h" /* User-Agent */
#include "web.h"
#include "list.h"

#include <sys/types.h> /* stat */
#include <sys/stat.h> /* stat */
#include <unistd.h> /* stat */

#if LIBCURL_VERSION_NUM >= 0x070F06 /* CURLOPT_SOCKOPT* was added in 7.15.6 */
 #define USE_LIBCURL_SOCKOPT
#endif

enum
{
    THREADFUNC_MAX_SLEEP_MSEC = 1000,
};

#if 0
#define dbgmsg(...) \
    do { \
        fprintf( stderr, __VA_ARGS__ ); \
        fprintf( stderr, "\n" ); \
    } while( 0 )
#else
#define dbgmsg( ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, "web", __VA_ARGS__ ); \
    } while( 0 )
#endif

/***
****
***/
tr_list * stopeasyhandle=NULL;
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

static void
task_free( struct tr_web_task * task )
{
    if( task->freebuf )
        evbuffer_free( task->freebuf );
    tr_free( task->cookies );
    tr_free( task->range );
    tr_free( task->url );
    tr_free( task->tracker_addr );
    tr_free( task );
}

/***
****
***/

struct tr_web
{
    bool curl_verbose;
    int close_mode;
    struct tr_web_task * tasks;
    tr_lock * taskLock;
    char * cookie_filename;
};

/***
****
***/

static size_t
writeFunc( void * ptr, size_t size, size_t nmemb, void * vtask )
{
    const size_t byteCount = size * nmemb;
    struct tr_web_task * task = vtask;
    if (task->torrentId != -1)
        {
	    tr_torrent * tor = tr_torrentFindFromId (task->session, task->torrentId);
        if (tor && tor->isRunning && !tor->isStopping && !tr_torrentIsSeed( tor ))
            {

            bool isPieceData;
            if( task->freebuf==NULL/*webseed*/)
                isPieceData = true;
            else
                isPieceData = false;

            unsigned int n=tr_bandwidthClamp(&(tor->bandwidth), TR_DOWN, nmemb, isPieceData );
            unsigned int n2=tr_bandwidthClamp(&(task->session->bandwidth), TR_DOWN, nmemb, isPieceData );
			if ( n2 < n ) n = n2;
            if(n<1&&isPieceData/*only limit webseed*/) {
                tr_list_append(&stopeasyhandle,task->curl_easy);
                return CURL_WRITEFUNC_PAUSE;
                }
            }
        else
            {
            if( !tor )
                tr_dbg( "?unknown? torrent deleted - cancelled webseed %p's buffer write - old ID was %d - ",
				         task, task->torrentId );
            else
                tr_tordbg( tor, "torrent paused - cancelled webseed %p's buffer write - ", task );

            if( task->tracker_addr )
                tr_dbg( "connection closed - Webseed IP:%s - torrent ID was %d - ", task->tracker_addr, task->torrentId );
            return byteCount + 1;
            }
        }
    evbuffer_add( task->response, ptr, byteCount );
    dbgmsg( "wrote %zu bytes to task %p's buffer", byteCount, task );
    return byteCount;
}

#ifdef USE_LIBCURL_SOCKOPT
static int
sockoptfunction( void * vtask, curl_socket_t fd, curlsocktype purpose UNUSED )
{
    struct tr_web_task * task = vtask;
    const bool isScrape = strstr( task->url, "scrape" ) != NULL;
    const bool isAnnounce = strstr( task->url, "announce" ) != NULL;

    /* announce and scrape requests have tiny payloads. */
    if( isScrape || isAnnounce )
    {
        const int sndbuf = isScrape ? 4096 : 1024;
        const int rcvbuf = isScrape ? 4096 : 3072;
        setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf) );
        setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf) );
    }

    /* return nonzero if this function encountered an error */
    return 0;
}
#endif

static long
getTimeoutFromURL( const struct tr_web_task * task )
{
    long timeout;
    const tr_session * session = task->session;

    if( !session || session->isClosed ) timeout = 20L;
    else if( strstr( task->url, "scrape" ) != NULL ) timeout = 30L;
    else if( strstr( task->url, "announce" ) != NULL ) timeout = 90L;
    else timeout = 240L;

    return timeout;
}

static int
progress_callback_func( void * vtask, double dltotal, double dlnow,
                                        double ultotal, double ulnow )
{
    struct tr_web_task * task = vtask;
    tr_torrent * wsTor = NULL;
    if( task->torrentId != -1 ) {
        wsTor = tr_torrentFindFromId( task->session, task->torrentId );
        if( !wsTor ) {
            tr_dbg( "not a torrent - progress test %d - ", (int)ulnow );
            if( task->tracker_addr && task->torrentId ) {
                tr_dbg( "connection closed on deleted torrent - Webseed IP:%s - old ID was %d - ", task->tracker_addr, task->torrentId );
                tr_dbg( "?unknown? torrent deleted - cancelled webseed %p's buffer write - old ID was %d - ", task, task->torrentId );
            }
            else if( task->tracker_addr )
                tr_dbg( "connection closed on deleted torrent - cancelled webseed %p's buffer write - Webseed IP:%s - ", task, task->tracker_addr );
            else if( task->torrentId )
                tr_dbg( "?unknown? torrent deleted - cancelled webseed %p's buffer write - old ID was %d - ", task, task->torrentId );
            else
                tr_dbg( "?unknown? torrent deleted - cancelled webseed %p's buffer write - ", task );

            return 1;
        }
    }

    if( wsTor && ( !wsTor->isRunning || wsTor->isStopping || tr_torrentIsSeed( wsTor ) ) ) {
        tr_dbg( "not running torrent - progress test %d - ", (int)ulnow );
        if( task->tracker_addr )
            tr_tordbg( wsTor, "Webseed IP:%s connection closed - torrent was paused by user", task->tracker_addr );
        else
            tr_tordbg( wsTor, "Torrent paused - Webseed disconnected - IP: ???unknown???" );
        if( task->tracker_addr && task->is_blocklisted )
            tr_tordbg( wsTor, "Webseed in BLOCKLIST- IP:%s connection closed by user pausing torrent", task->tracker_addr );
        return 1;
    }

    return 0;
}

static CURL *
createEasy( tr_session * s, struct tr_web * web, struct tr_web_task * task )
{
    bool is_default_value;
    const tr_address * addr;
    CURL * e = task->curl_easy = curl_easy_init( );

    task->timeout_secs = getTimeoutFromURL( task );
    if( task->freebuf==NULL )
    {
        task->timeout_secs = (long)s->webseedTimeout;

        // special value to turn off the dropping of connections for stopped torrents
        if( s->dropInterruptedWebseeds ) {
            curl_easy_setopt( e, CURLOPT_PROGRESSFUNCTION, progress_callback_func );
            /* pass the struct pointer into the progress function */
            curl_easy_setopt( e, CURLOPT_PROGRESSDATA, task );
            curl_easy_setopt( e, CURLOPT_NOPROGRESS, 0L );
        }
    }

    curl_easy_setopt( e, CURLOPT_AUTOREFERER, 1L );

    if (web->cookie_filename != NULL)
    curl_easy_setopt( e, CURLOPT_COOKIEFILE, web->cookie_filename );

    curl_easy_setopt( e, CURLOPT_ENCODING, "" );
//    Set compression to what curl was built with
//    https://github.com/transmission/transmission/pull/495

    curl_easy_setopt( e, CURLOPT_FOLLOWLOCATION, 1L );
    curl_easy_setopt( e, CURLOPT_MAXREDIRS, (long)s->maxRedirect );
    curl_easy_setopt( e, CURLOPT_NOSIGNAL, 1L );
    curl_easy_setopt( e, CURLOPT_PRIVATE, task );
#ifdef USE_LIBCURL_SOCKOPT
    curl_easy_setopt( e, CURLOPT_SOCKOPTFUNCTION, sockoptfunction );
    curl_easy_setopt( e, CURLOPT_SOCKOPTDATA, task );
#endif
    curl_easy_setopt( e, CURLOPT_SSL_VERIFYHOST, 0L );
    curl_easy_setopt( e, CURLOPT_SSL_VERIFYPEER, 0L );
    curl_easy_setopt( e, CURLOPT_TIMEOUT, task->timeout_secs );
    curl_easy_setopt( e, CURLOPT_URL, task->url );

    if( strlen( tr_sessionGetUserAgent( s ) ) )
        curl_easy_setopt( e, CURLOPT_USERAGENT, tr_sessionGetUserAgent( s ) );
    else
        curl_easy_setopt( e, CURLOPT_USERAGENT, TR_NAME "/" SHORT_VERSION_STRING );

    curl_easy_setopt( e, CURLOPT_VERBOSE, (long)(web->curl_verbose?1:0) );
    curl_easy_setopt( e, CURLOPT_WRITEDATA, task );
    curl_easy_setopt( e, CURLOPT_WRITEFUNCTION, writeFunc );

    if((( addr = tr_sessionGetPublicAddress( s, TR_AF_INET, &is_default_value ))) && !is_default_value )
        curl_easy_setopt( e, CURLOPT_INTERFACE, tr_address_to_string( addr ) );
    else if ((( addr = tr_sessionGetPublicAddress( s, TR_AF_INET6, &is_default_value ))) && !is_default_value )
        curl_easy_setopt( e, CURLOPT_INTERFACE, tr_address_to_string( addr ) );

    if( task->cookies != NULL )
        curl_easy_setopt( e, CURLOPT_COOKIE, task->cookies );

    if( task->range != NULL ) {
        curl_easy_setopt( e, CURLOPT_RANGE, task->range );
        /* don't bother asking the server to compress webseed fragments */
        curl_easy_setopt( e, CURLOPT_ENCODING, "identity" );
    }

    return e;
}

/***
****
***/

static void
task_finish_func( void * vtask )
{
    struct tr_web_task * task = vtask;
    dbgmsg( "finished web task %p; got %ld", task, task->code );

    if( task->done_func != NULL )
        task->done_func( task->session,
                         task->did_connect,
                         task->did_timeout,
                         task->is_blocklisted,
                         task->tracker_addr,
                         task->code,
                         evbuffer_pullup( task->response, -1 ),
                         evbuffer_get_length( task->response ),
                         task->done_func_user_data );

    task_free( task );
}

/****
*****
****/

struct tr_web_task *
tr_webRun( tr_session         * session,
           const char         * url,
           const char         * range,
           const char         * cookies,
           tr_web_done_func     done_func,
           void               * done_func_user_data )
{
    return tr_webRunWithBuffer( session, -1, url, range, cookies,
                                done_func, done_func_user_data,
                                NULL );
}

struct tr_web_task *
tr_webRunWithBuffer( tr_session         * session,
                     int                  torrentId,
                     const char         * url,
                     const char         * range,
                     const char         * cookies,
                     tr_web_done_func     done_func,
                     void               * done_func_user_data,
                     struct evbuffer    * buffer )
{
    struct tr_web * web = session->web;

    if( web != NULL )
    {
        struct tr_web_task * task = tr_new0( struct tr_web_task, 1 );

        task->session = session;
        task->torrentId = torrentId;
        task->url = tr_strdup( url );
        task->range = tr_strdup( range );
        task->cookies = tr_strdup( cookies);
        task->done_func = done_func;
        task->done_func_user_data = done_func_user_data;
        task->response = buffer ? buffer : evbuffer_new( );
        task->freebuf = buffer ? NULL : task->response;

        tr_lockLock( web->taskLock );
        task->next = web->tasks;
        web->tasks = task;
        tr_lockUnlock( web->taskLock );
        return task;
    }
    return NULL;
}

/**
 * Portability wrapper for select().
 *
 * http://msdn.microsoft.com/en-us/library/ms740141%28VS.85%29.aspx
 * On win32, any two of the parameters, readfds, writefds, or exceptfds,
 * can be given as null. At least one must be non-null, and any non-null
 * descriptor set must contain at least one handle to a socket.
 */
static void
tr_select( int nfds,
           fd_set * r_fd_set, fd_set * w_fd_set, fd_set * c_fd_set,
           struct timeval  * t )
{
#ifdef WIN32
    if( !r_fd_set->fd_count && !w_fd_set->fd_count && !c_fd_set->fd_count )
    {
        const long int msec = t->tv_sec*1000 + t->tv_usec/1000;
        tr_wait_msec( msec );
    }
    else if( select( 0, r_fd_set->fd_count ? r_fd_set : NULL,
                        w_fd_set->fd_count ? w_fd_set : NULL,
                        c_fd_set->fd_count ? c_fd_set : NULL, t ) < 0 )
    {
        char errstr[512];
        const int e = EVUTIL_SOCKET_ERROR( );
        tr_net_strerror( errstr, sizeof( errstr ), e );
        dbgmsg( "Error: select (%d) %s", e, errstr );
    }
#else
    select( nfds, r_fd_set, w_fd_set, c_fd_set, t );
#endif
}

#ifdef SYS_DARWIN
 #define TR_STAT_MTIME(sb) ((sb).st_mtimespec.tv_sec)
#else
 #define TR_STAT_MTIME(sb) ((sb).st_mtime)
#endif

static bool
fileExists( const char * filename, time_t * mtime )
{
    struct stat sb;
    const bool ok = !stat( filename, &sb );

    if( ok && ( mtime != NULL ) )
        *mtime = TR_STAT_MTIME( sb );

    return ok;
}

static void
tr_webThreadFunc( void * vsession )
{
    char * str;
    CURLM * multi;
    struct tr_web * web;
    int taskCount = 0;
    struct tr_web_task * task;
    tr_session * session = vsession;

    /* try to enable ssl for https support; but if that fails,
     * try a plain vanilla init */
    if( curl_global_init( CURL_GLOBAL_SSL ) )
        curl_global_init( 0 );

    web = tr_new0( struct tr_web, 1 );
    web->close_mode = ~0;
    web->taskLock = tr_lockNew( );
    web->tasks = NULL;
    web->curl_verbose = getenv( "TR_CURL_VERBOSE" ) != NULL;

    str = tr_buildPath (session->configDir, "cookies.txt", NULL);
    if (fileExists (str, NULL))
        web->cookie_filename = tr_strdup (str);
    tr_free (str);

    multi = curl_multi_init( );
    session->web = web;

    for( ;; )
    {
        long msec;
        int unused;
        CURLMsg * msg;
        CURLMcode mcode;

        if( web->close_mode == TR_WEB_CLOSE_NOW )
            break;
        if( ( web->close_mode == TR_WEB_CLOSE_WHEN_IDLE ) && ( web->tasks == NULL ) && ( taskCount <= 0 ) )
            break;

        /* add tasks from the queue */
        tr_lockLock( web->taskLock );
        while( web->tasks != NULL )
        {
            /* pop the task */
            task = web->tasks;
            web->tasks = task->next;
            task->next = NULL;

            dbgmsg( "adding task to curl: [%s]", task->url );
            curl_multi_add_handle( multi, createEasy( session, web, task ));
            /*fprintf( stderr, "adding a task.. taskCount is now %d\n", taskCount );*/
            ++taskCount;
        }
        tr_lockUnlock( web->taskLock );

        //restart stopped curl handle;
        CURL *handle;
        int n=tr_list_size(stopeasyhandle);
        while(n--){
            handle=tr_list_pop_front(&stopeasyhandle);
            curl_easy_pause(handle,CURLPAUSE_CONT);
        }
        /* maybe wait a little while before calling curl_multi_perform() */
        msec = 0;
        curl_multi_timeout( multi, &msec );
        if( msec < 0 )
            msec = THREADFUNC_MAX_SLEEP_MSEC;
        if( session->isClosed )
            msec = 100; /* on shutdown, call perform() more frequently */
        if( msec > 0 )
        {
            int usec;
            int max_fd;
            struct timeval t;
            fd_set r_fd_set, w_fd_set, c_fd_set;

            max_fd = 0;
            FD_ZERO( &r_fd_set );
            FD_ZERO( &w_fd_set );
            FD_ZERO( &c_fd_set );
            curl_multi_fdset( multi, &r_fd_set, &w_fd_set, &c_fd_set, &max_fd );

            if( msec > THREADFUNC_MAX_SLEEP_MSEC )
                msec = THREADFUNC_MAX_SLEEP_MSEC;

            usec = msec * 1000;
            t.tv_sec =  usec / 1000000;
            t.tv_usec = usec % 1000000;
            tr_select( max_fd+1, &r_fd_set, &w_fd_set, &c_fd_set, &t );
        }

        /* call curl_multi_perform() */
        do {
            mcode = curl_multi_perform( multi, &unused );
        } while( mcode == CURLM_CALL_MULTI_PERFORM );

        /* pump completed tasks from the multi */
        while(( msg = curl_multi_info_read( multi, &unused )))
        {
            if(( msg->msg == CURLMSG_DONE ) && ( msg->easy_handle != NULL ))
            {
                double total_time;
                struct tr_web_task * task;
                long req_bytes_sent;
                const char * server_ip;
                struct tr_address addr;
                CURL * e = msg->easy_handle;
                CURLcode res = msg->data.result;
                curl_easy_getinfo( e, CURLINFO_PRIVATE, (void*)&task );
                curl_easy_getinfo( e, CURLINFO_RESPONSE_CODE, &task->code );


                task->is_blocklisted = 0;
                tr_address_from_string( &addr, "0.0.0.0" );

                // hook for blocklist check
                server_ip = NULL;
                if( !curl_easy_getinfo( e, CURLINFO_PRIMARY_IP, &server_ip ) )  // CURLE_OK
                {
                    if( tr_address_from_string( &addr, server_ip ) )
                    {
                        if( tr_sessionIsAddressBlocked( task->session, &addr ) )
                        task->is_blocklisted = 1;
                    }
                }
                const tr_address * send_address = &addr;
                task->tracker_addr = tr_strdup( tr_address_to_string( send_address ) );

                curl_easy_getinfo( e, CURLINFO_REQUEST_SIZE, &req_bytes_sent );
                curl_easy_getinfo( e, CURLINFO_TOTAL_TIME, &total_time );
                task->did_connect = task->code>0 || req_bytes_sent>0;
                task->did_timeout = !task->code && ( total_time >= task->timeout_secs );
                curl_multi_remove_handle( multi, e );
                tr_list_remove_data (&stopeasyhandle, e);
                curl_easy_cleanup( e );

// http://curl.haxx.se/libcurl/c/curl_easy_getinfo.html
// You should not free the memory returned by this function unless it is explicitly mentioned below.
//                tr_free( server_ip );


/*fprintf( stderr, "removing a completed task.. taskCount is now %d (response code: %d, response len: %d)\n", taskCount, (int)task->code, (int)evbuffer_get_length(task->response) );*/
                if( ( progress_callback_func( task, 0, 0, 0, (double)999 ) ) || ( res == CURLE_TOO_MANY_REDIRECTS ) )
                    task->code = 999L;
                tr_runInEventThread( task->session, task_finish_func, task );
                --taskCount;
            }
        }
    }

    /* Discard any remaining tasks.
     * This is rare, but can happen on shutdown with unresponsive trackers. */
    while( web->tasks != NULL ) {
        task = web->tasks;
        web->tasks = task->next;
        dbgmsg( "Discarding task \"%s\"", task->url );
        task_free( task );
    }

    /* cleanup */
    tr_list_free (&stopeasyhandle, NULL);
    curl_multi_cleanup( multi );
    tr_lockFree( web->taskLock );
    tr_free( web->cookie_filename );
    tr_free( web );
    session->web = NULL;
}

void
tr_webInit( tr_session * session )
{
    tr_threadNew( tr_webThreadFunc, session );
}

void
tr_webClose( tr_session * session, tr_web_close_mode close_mode )
{
    if( session->web != NULL )
    {
        session->web->close_mode = close_mode;

        if( close_mode == TR_WEB_CLOSE_NOW )
            while( session->web != NULL )
                tr_wait_msec( 100 );
    }
}

void
tr_webGetTaskInfo( struct tr_web_task * task, tr_web_task_info info, void * dst )
{
    curl_easy_getinfo( task->curl_easy, (CURLINFO) info, dst );
}

/*****
******
******
*****/

const char *
tr_webGetResponseStr( long code )
{
    switch( code )
    {
        case   0: return "No Response";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 306: return "(Unused)";
        case 307: return "Temporary Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Requested Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 999: return "Too many redirects";
        default:  return "Unknown Error";
    }
}

void
tr_http_escape( struct evbuffer  * out,
                const char * str, int len, bool escape_slashes )
{
    const char * end;

    if( ( len < 0 ) && ( str != NULL ) )
        len = strlen( str );

    for( end=str+len; str && str!=end; ++str ) {
        if(    ( *str == ',' )
            || ( *str == '-' )
            || ( *str == '.' )
            || ( ( '0' <= *str ) && ( *str <= '9' ) )
            || ( ( 'A' <= *str ) && ( *str <= 'Z' ) )
            || ( ( 'a' <= *str ) && ( *str <= 'z' ) )
            || ( ( *str == '/' ) && ( !escape_slashes ) ) )
            evbuffer_add_printf( out, "%c", *str );
        else
            evbuffer_add_printf( out, "%%%02X", (unsigned)(*str&0xFF) );
    }
}

char *
tr_http_unescape( const char * str, int len )
{
    char * tmp = curl_unescape( str, len );
    char * ret = tr_strdup( tmp );
    curl_free( tmp );
    return ret;
}

static int
is_rfc2396_alnum( uint8_t ch )
{
    return ( '0' <= ch && ch <= '9' )
        || ( 'A' <= ch && ch <= 'Z' )
        || ( 'a' <= ch && ch <= 'z' )
        || ch == '.'
        || ch == '-'
        || ch == '_'
        || ch == '~';
}

void
tr_http_escape_sha1( char * out, const uint8_t * sha1_digest )
{
    const uint8_t * in = sha1_digest;
    const uint8_t * end = in + SHA_DIGEST_LENGTH;

    while( in != end )
        if( is_rfc2396_alnum( *in ) )
            *out++ = (char) *in++;
        else
            out += tr_snprintf( out, 4, "%%%02x", (unsigned int)*in++ );

    *out = '\0';
}
