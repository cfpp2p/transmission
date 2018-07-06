/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: torrent.c 12931 2011-09-28 16:06:19Z jordan $
 */

#include <signal.h> /* signal() */
#include <sys/types.h> /* stat */
#include <sys/stat.h> /* stat */
#ifndef WIN32
 #include <sys/wait.h> /* wait() */
#else
 #include <process.h>
 #define waitpid(pid, status, options)	_cwait(status, pid, WAIT_CHILD)
#endif
#include <unistd.h> /* stat */
#include <dirent.h>

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <string.h> /* memcmp */
#include <stdlib.h> /* qsort */
#include <stdio.h> /* remove() */

#include <event2/util.h> /* evutil_vsnprintf() */

#include "transmission.h"
#include "announcer.h"
#include "bandwidth.h"
#include "bencode.h"
#include "cache.h"
#include "completion.h"
#include "crypto.h" /* for tr_sha1 */
#include "resume.h"
#include "fdlimit.h" /* tr_fdTorrentClose */
#include "inout.h" /* tr_ioTestPiece() */
#include "list.h"
#include "magnet.h"
#include "metainfo.h"
#include "peer-common.h" /* MAX_BLOCK_SIZE */
#include "peer-mgr.h"
#include "platform.h" /* TR_PATH_DELIMITER_STR */
#include "ptrarray.h"
#include "session.h"
#include "torrent.h"
#include "torrent-magnet.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"
#include "verify.h"
#include "version.h"

/***
****
***/

#define tr_deeplog_tor( tor, ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, tr_torrentName( tor ), __VA_ARGS__ ); \
    } while( 0 )

/***
****
***/

const char *
tr_torrentName( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->info.name;
}

int
tr_torrentId( const tr_torrent * tor )
{
    return tor->uniqueId;
}

tr_torrent*
tr_torrentFindFromId( tr_session * session, int id )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( tor->uniqueId == id )
            return tor;

    return NULL;
}

tr_torrent*
tr_torrentFindFromHashString( tr_session *  session, const char * str )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( !evutil_ascii_strcasecmp( str, tor->info.hashString ) )
            return tor;

    return NULL;
}

tr_torrent*
tr_torrentFindFromHash( tr_session * session, const uint8_t * torrentHash )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( *tor->info.hash == *torrentHash )
            if( !memcmp( tor->info.hash, torrentHash, SHA_DIGEST_LENGTH ) )
                return tor;

    return NULL;
}

tr_torrent*
tr_torrentFindFromMagnetLink( tr_session * session, const char * magnet )
{
    tr_magnet_info * info;
    tr_torrent * tor = NULL;

    if(( info = tr_magnetParse( magnet )))
    {
        tor = tr_torrentFindFromHash( session, info->hash );
        tr_magnetFree( info );
    }

    return tor;
}

tr_torrent*
tr_torrentFindFromObfuscatedHash( tr_session * session,
                                  const uint8_t * obfuscatedTorrentHash )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( !memcmp( tor->obfuscatedHash, obfuscatedTorrentHash,
                     SHA_DIGEST_LENGTH ) )
            return tor;

    return NULL;
}

bool
tr_torrentIsPieceTransferAllowed( const tr_torrent  * tor,
                                  tr_direction        direction )
{
    int limit;
    bool allowed = true;

    if( tr_torrentUsesSpeedLimit( tor, direction ) )
        if( tr_torrentGetSpeedLimit_Bps( tor, direction ) <= 0 )
            allowed = false;

    if( tr_torrentUsesSessionLimits( tor ) )
        if( tr_sessionGetActiveSpeedLimit_Bps( tor->session, direction, &limit ) )
            if( limit <= 0 )
                allowed = false;

    return allowed;
}

/***
****  PER-TORRENT UL / DL SPEEDS
***/

void
tr_torrentSetSpeedLimit_Bps( tr_torrent * tor, tr_direction dir, int Bps )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );
    assert( Bps >= 0 );

    if( tr_bandwidthSetDesiredSpeed_Bps( &tor->bandwidth, dir, Bps ) )
        tr_torrentSetDirty( tor );
}
void
tr_torrentSetSpeedLimit_KBps( tr_torrent * tor, tr_direction dir, int KBps )
{
    tr_torrentSetSpeedLimit_Bps( tor, dir, toSpeedBytes( KBps ) );
}

int
tr_torrentGetSpeedLimit_Bps( const tr_torrent * tor, tr_direction dir )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );

    return tr_bandwidthGetDesiredSpeed_Bps( &tor->bandwidth, dir );
}
int
tr_torrentGetSpeedLimit_KBps( const tr_torrent * tor, tr_direction dir )
{
    return toSpeedKBps( tr_torrentGetSpeedLimit_Bps( tor, dir ) );
}

void
tr_torrentUseSpeedLimit( tr_torrent * tor, tr_direction dir, bool do_use )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );

    if( tr_bandwidthSetLimited( &tor->bandwidth, dir, do_use ) )
        tr_torrentSetDirty( tor );
}

bool
tr_torrentUsesSpeedLimit( const tr_torrent * tor, tr_direction dir )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );

    return tr_bandwidthIsLimited( &tor->bandwidth, dir );
}

void
tr_torrentUseSessionLimits( tr_torrent * tor, bool doUse )
{
    bool changed;

    assert( tr_isTorrent( tor ) );

    changed = tr_bandwidthHonorParentLimits( &tor->bandwidth, TR_UP, doUse );
    changed |= tr_bandwidthHonorParentLimits( &tor->bandwidth, TR_DOWN, doUse );

    if( changed )
        tr_torrentSetDirty( tor );
}

bool
tr_torrentUsesSessionLimits( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tr_bandwidthAreParentLimitsHonored( &tor->bandwidth, TR_UP );
}

/***
****
***/

void
tr_torrentSetCheatMode( tr_torrent * tor, tr_cheatMode_t mode )
{
    assert( tr_isTorrent( tor ) );

    if( ( mode >= TR_CHEAT_DEACT && mode < TR_CHEAT_COUNT ) && mode != tor->cheatMode )
    {
        tor->cheatMode = mode;

        tr_torrentSetDirty( tor );
    }
}

tr_cheatMode_t
tr_torrentGetCheatMode( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->cheatMode;
}

void
tr_torrentSetStreamingMode( tr_torrent * tor, tr_streamingMode_t mode )
{
    assert( tr_isTorrent( tor ) );

    if( ( mode >= TR_STREAMING_OFF ) && ( mode < TR_STREAMING_COUNT ) && ( mode != tor->streamingMode ) )
    {
        tr_torrentLock( tor );
        tor->streamingMode = mode;
        tr_torrentSetDirty( tor );
        tr_peerMgrRebuildRequests( tor );

        tr_torrentUnlock( tor );
    }
}

tr_streamingMode_t
tr_torrentGetStreamingMode( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tor->info.webseedCount > 0 )
        return TR_STREAMING_FORCED;
    else
        return tor->streamingMode;
}

void
tr_torrentSetRatioMode( tr_torrent *  tor, tr_ratiolimit mode )
{
    assert( tr_isTorrent( tor ) );
    assert( mode==TR_RATIOLIMIT_GLOBAL || mode==TR_RATIOLIMIT_SINGLE || mode==TR_RATIOLIMIT_UNLIMITED  );

    if( mode != tor->ratioLimitMode )
    {
        tor->ratioLimitMode = mode;

        tr_torrentSetDirty( tor );
    }
}

tr_ratiolimit
tr_torrentGetRatioMode( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->ratioLimitMode;
}

void
tr_torrentSetRatioLimit( tr_torrent * tor, double desiredRatio )
{
    assert( tr_isTorrent( tor ) );

    if( (int)(desiredRatio*100.0) != (int)(tor->desiredRatio*100.0) )
    {
        tor->desiredRatio = desiredRatio;

        tr_torrentSetDirty( tor );
    }
}

double
tr_torrentGetRatioLimit( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->desiredRatio;
}

bool
tr_torrentGetSeedRatio( const tr_torrent * tor, double * ratio )
{
    bool isLimited;

    switch( tr_torrentGetRatioMode( tor ) )
    {
        case TR_RATIOLIMIT_SINGLE:
            isLimited = true;
            if( ratio )
                *ratio = tr_torrentGetRatioLimit( tor );
            break;

        case TR_RATIOLIMIT_GLOBAL:
            isLimited = tr_sessionIsRatioLimited( tor->session );
            if( isLimited && ratio )
                *ratio = tr_sessionGetRatioLimit( tor->session );
            break;

        default: /* TR_RATIOLIMIT_UNLIMITED */
            isLimited = false;
            break;
    }

    return isLimited;
}

/* returns true if the seed ratio applies --
 * it applies if the torrent's a seed AND it has a seed ratio set */
static bool
tr_torrentGetSeedRatioBytes( tr_torrent  * tor,
                             uint64_t    * setmeLeft,
                             uint64_t    * setmeGoal )
{
    double seedRatio;
    bool seedRatioApplies = false;

    if( tr_torrentGetSeedRatio( tor, &seedRatio ) )
    {
        const uint64_t u = tor->uploadedCur + tor->uploadedPrev;
        const uint64_t d = tor->downloadedCur + tor->downloadedPrev;
        const uint64_t baseline = d ? d : tr_cpSizeWhenDone( &tor->completion );
        const uint64_t goal = baseline * seedRatio;
        if( setmeLeft ) *setmeLeft = goal > u ? goal - u : 0;
        if( setmeGoal ) *setmeGoal = goal;
        seedRatioApplies = tr_torrentIsSeed( tor );
    }

    return seedRatioApplies;
}

static bool
tr_torrentIsSeedRatioDone( tr_torrent * tor )
{
    uint64_t bytesLeft;
    return tr_torrentGetSeedRatioBytes( tor, &bytesLeft, NULL ) && !bytesLeft;
}

/***
****
***/

void
tr_torrentSetIdleMode( tr_torrent *  tor, tr_idlelimit mode )
{
    assert( tr_isTorrent( tor ) );
    assert( mode==TR_IDLELIMIT_GLOBAL || mode==TR_IDLELIMIT_SINGLE || mode==TR_IDLELIMIT_UNLIMITED  );

    if( mode != tor->idleLimitMode )
    {
        tor->idleLimitMode = mode;

        tr_torrentSetDirty( tor );
    }
}

tr_idlelimit
tr_torrentGetIdleMode( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->idleLimitMode;
}

void
tr_torrentSetIdleLimit( tr_torrent * tor, uint16_t idleMinutes )
{
    assert( tr_isTorrent( tor ) );

    if( idleMinutes > 0 )
    {
        tor->idleLimitMinutes = idleMinutes;

        tr_torrentSetDirty( tor );
    }
}

uint16_t
tr_torrentGetIdleLimit( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->idleLimitMinutes;
}

bool
tr_torrentGetSeedIdle( const tr_torrent * tor, uint16_t * idleMinutes )
{
    bool isLimited;

    switch( tr_torrentGetIdleMode( tor ) )
    {
        case TR_IDLELIMIT_SINGLE:
            isLimited = true;
            if( idleMinutes )
                *idleMinutes = tr_torrentGetIdleLimit( tor );
            break;

        case TR_IDLELIMIT_GLOBAL:
            isLimited = tr_sessionIsIdleLimited( tor->session );
            if( isLimited && idleMinutes )
                *idleMinutes = tr_sessionGetIdleLimit( tor->session );
            break;

        default: /* TR_IDLELIMIT_UNLIMITED */
            isLimited = false;
            break;
    }

    return isLimited;
}

static bool
tr_torrentIsSeedIdleLimitDone( tr_torrent * tor )
{
    uint16_t idleMinutes;
    return tr_torrentGetSeedIdle( tor, &idleMinutes )
        && difftime(tr_time(), MAX(tor->startDate, tor->activityDate)) >= idleMinutes * 60u;
}

/***
****
***/

void
tr_torrentCheckSeedLimit( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( !tor->isRunning || !tr_torrentIsSeed( tor ) || tor->isStopping )
        return;

    /* if we're seeding and reach our seed ratio limit, stop the torrent */
    if( tr_torrentIsSeedRatioDone( tor ) )
    {
        tr_torinf( tor, "Seed ratio reached; pausing torrent" );

        tor->isStopping = true;

        /* maybe notify the client */
        if( tor->ratio_limit_hit_func != NULL )
            tor->ratio_limit_hit_func( tor, tor->ratio_limit_hit_func_user_data );
    }
    /* if we're seeding and reach our inactiviy limit, stop the torrent */
    else if( tr_torrentIsSeedIdleLimitDone( tor ) )
    {
        tr_torinf( tor, "Seeding idle limit reached; pausing torrent" );

        tor->isStopping = true;
        tor->finishedSeedingByIdle = true;

        /* maybe notify the client */
        if( tor->idle_limit_hit_func != NULL )
            tor->idle_limit_hit_func( tor, tor->idle_limit_hit_func_user_data );
    }
}

/***
****
***/

void
tr_torrentSetLocalError( tr_torrent * tor, const char * fmt, ... )
{
    va_list ap;

    assert( tr_isTorrent( tor ) );

    va_start( ap, fmt );
    tor->error = TR_STAT_LOCAL_ERROR;
    tor->errorTracker[0] = '\0';
    evutil_vsnprintf( tor->errorString, sizeof( tor->errorString ), fmt, ap );
    va_end( ap );

    tr_torerr( tor, "%s", tor->errorString );

    if( tor->isRunning )
        tor->isStopping = true;
}

static void
tr_torrentClearError( tr_torrent * tor )
{
    tor->error = TR_STAT_OK;
    tor->errorString[0] = '\0';
    tor->errorTracker[0] = '\0';
}

static void
onTrackerResponse( tr_torrent * tor, const tr_tracker_event * event, void * unused UNUSED )
{
    switch( event->messageType )
    {
        case TR_TRACKER_PEERS:
        {
            size_t i;
            const int8_t seedProbability = event->seedProbability;
            const bool allAreSeeds = seedProbability == 100;

             if( allAreSeeds )
                tr_tordbg( tor, "Got %zu seeds from tracker", event->pexCount );
            else
                tr_tordbg( tor, "Got %zu peers from tracker", event->pexCount );

            for( i = 0; i < event->pexCount; ++i )
                tr_peerMgrAddPex( tor, TR_PEER_FROM_TRACKER, &event->pex[i], seedProbability );

            break;
        }

        case TR_TRACKER_WARNING:
            tr_torerr( tor, _( "Tracker warning: \"%s\"" ), event->text );
            tor->error = TR_STAT_TRACKER_WARNING;
            tr_strlcpy( tor->errorTracker, event->tracker, sizeof( tor->errorTracker ) );
            tr_strlcpy( tor->errorString, event->text, sizeof( tor->errorString ) );
            break;

        case TR_TRACKER_ERROR:
            tr_torerr( tor, _( "Tracker error: \"%s\"" ), event->text );
            tor->error = TR_STAT_TRACKER_ERROR;
            tr_strlcpy( tor->errorTracker, event->tracker, sizeof( tor->errorTracker ) );
            tr_strlcpy( tor->errorString, event->text, sizeof( tor->errorString ) );
            break;

        case TR_TRACKER_ERROR_CLEAR:
            if( tor->error != TR_STAT_LOCAL_ERROR )
                tr_torrentClearError( tor );
            break;
    }
}

/***
****
****  TORRENT INSTANTIATION
****
***/

static tr_piece_index_t
getBytePiece( const tr_info * info, uint64_t byteOffset )
{
    tr_piece_index_t piece;
    assert( info );
    assert( info->pieceSize != 0 );

    piece = byteOffset / info->pieceSize;

    /* handle 0-byte files at the end of a torrent */
    if (byteOffset == info->totalSize)
      piece = info->pieceCount - 1;

    return piece;
}

static void
initFilePieces( tr_info *       info,
                tr_file_index_t fileIndex )
{
    tr_file * file;
    uint64_t  firstByte, lastByte;

    assert( info );
    assert( fileIndex < info->fileCount );

    file = &info->files[fileIndex];
    firstByte = file->offset;
    lastByte = firstByte + ( file->length ? file->length - 1 : 0 );
    file->firstPiece = getBytePiece( info, firstByte );
    file->lastPiece = getBytePiece( info, lastByte );
}

static int
pieceHasFile( tr_piece_index_t piece,
              const tr_file *  file )
{
    return ( file->firstPiece <= piece ) && ( piece <= file->lastPiece );
}

static tr_priority_t
calculatePiecePriority( const tr_torrent * tor,
                        tr_piece_index_t   piece,
                        int                fileHint )
{
    tr_file_index_t i;
    tr_priority_t priority = TR_PRI_LOW;

    /* find the first file that has data in this piece */
    if( fileHint >= 0 ) {
        i = fileHint;
        while( i > 0 && pieceHasFile( piece, &tor->info.files[i - 1] ) )
            --i;
    } else {
        for( i = 0; i < tor->info.fileCount; ++i )
            if( pieceHasFile( piece, &tor->info.files[i] ) )
                break;
    }

    /* the piece's priority is the max of the priorities
     * of all the files in that piece */
    for( ; i < tor->info.fileCount; ++i )
    {
        const tr_file * file = &tor->info.files[i];

        if( !pieceHasFile( piece, file ) )
            break;

        priority = MAX( priority, file->priority );

        /* when dealing with multimedia files, getting the first and
           last pieces can sometimes allow you to preview it a bit
           before it's fully downloaded... */
        if( file->priority >= TR_PRI_NORMAL )
            if( file->firstPiece == piece || file->lastPiece == piece )
                priority = TR_PRI_HIGH;
    }

    return priority;
}

static void
tr_torrentInitFilePieces( tr_torrent * tor )
{
    int * firstFiles;
    tr_file_index_t f;
    tr_piece_index_t p;
    uint64_t offset = 0;
    tr_info * inf = &tor->info;

    /* assign the file offsets */
    for( f=0; f<inf->fileCount; ++f ) {
        inf->files[f].offset = offset;
        offset += inf->files[f].length;
        initFilePieces( inf, f );
    }

    /* build the array of first-file hints to give calculatePiecePriority */
    firstFiles = tr_new( int, inf->pieceCount );
    for( p=f=0; p<inf->pieceCount; ++p ) {
        while( inf->files[f].lastPiece < p )
            ++f;
        firstFiles[p] = f;
    }

#if 0
    /* test to confirm the first-file hints are correct */
    for( p=0; p<inf->pieceCount; ++p ) {
        f = firstFiles[p];
        assert( inf->files[f].firstPiece <= p );
        assert( inf->files[f].lastPiece >= p );
        if( f > 0 )
            assert( inf->files[f-1].lastPiece < p );
        for( f=0; f<inf->fileCount; ++f )
            if( pieceHasFile( p, &inf->files[f] ) )
                break;
        assert( (int)f == firstFiles[p] );
    }
#endif

    for( p=0; p<inf->pieceCount; ++p )
        inf->pieces[p].priority = calculatePiecePriority( tor, p, firstFiles[p] );

    tr_free( firstFiles );
}

static void torrentStart( tr_torrent * tor, bool bypass_queue );

/**
 * Decide on a block size. Constraints:
 * (1) most clients decline requests over 16 KiB
 * (2) pieceSize must be a multiple of block size
 */
uint32_t
tr_getBlockSize( uint32_t pieceSize )
{
    uint32_t b = MAX_BLOCK_SIZE;

//  this was established in the previos revision logic
//  do we really want to allow ANY piece sizes less than MAX_BLOCK_SIZE?	
  if( pieceSize <= MAX_BLOCK_SIZE ) return pieceSize;

  /**
  * ticket 4005
  * use a MIN_BLOCK_SIZE of 8193 - same as previous revision 
  * lesser block sizes create an undo number of requests
  * note - allowing a block size of 1 would guarantee any piece size
  * but smaller requests result in higher overhead
  * due to tracking a greater number of requests
  * there should be some constraints on this
  * we cant have everything
  */

// try for the LARGEST block size we can find
// not just any one thats greater than 8192 as per previous revision
// this way we are more efficient and less overhead
    while( (b > 0) && (pieceSize % b) )
        b--;

    if( pieceSize % b ) /* not cleanly divisible */
        return 0;
    return b;
}

static void refreshCurrentDir( tr_torrent * tor );

static void
torrentInitFromInfo( tr_torrent * tor )
{
    uint64_t t;
    tr_info * info = &tor->info;

    tor->blockSize = tr_getBlockSize( info->pieceSize );

    if( info->pieceSize )
        tor->lastPieceSize = (uint32_t)(info->totalSize % info->pieceSize);

    if( !tor->lastPieceSize )
        tor->lastPieceSize = info->pieceSize;

    if( tor->blockSize )
        tor->lastBlockSize = info->totalSize % tor->blockSize;

    if( !tor->lastBlockSize )
        tor->lastBlockSize = tor->blockSize;

    tor->blockCount = tor->blockSize
        ? ( info->totalSize + tor->blockSize - 1 ) / tor->blockSize
        : 0;

    tor->blockCountInPiece = tor->blockSize
        ? info->pieceSize / tor->blockSize
        : 0;

    tor->blockCountInLastPiece = tor->blockSize
        ? ( tor->lastPieceSize + tor->blockSize - 1 ) / tor->blockSize
        : 0;

    /* check our work */
    if( tor->blockSize != 0 )
        assert( ( info->pieceSize % tor->blockSize ) == 0 );
    t = info->pieceCount - 1;
    t *= info->pieceSize;
    t += tor->lastPieceSize;
    assert( t == info->totalSize );
    t = tor->blockCount - 1;
    t *= tor->blockSize;
    t += tor->lastBlockSize;
    assert( t == info->totalSize );
    t = info->pieceCount - 1;
    t *= tor->blockCountInPiece;
    t += tor->blockCountInLastPiece;
    assert( t == (uint64_t)tor->blockCount );

    tr_cpConstruct( &tor->completion, tor );

    tr_torrentInitFilePieces( tor );

    tor->completeness = tr_cpGetStatus( &tor->completion );
}

static void tr_torrentFireMetadataCompleted( tr_torrent * tor );

void
tr_torrentGotNewInfoDict( tr_torrent * tor )
{
    torrentInitFromInfo( tor );

    tr_peerMgrOnTorrentGotMetainfo( tor );

    tr_torrentFireMetadataCompleted( tor );
}

static bool
hasAnyLocalData( const tr_torrent * tor )
{
    tr_file_index_t i;

    for( i=0; i<tor->info.fileCount; ++i )
        if( tr_torrentFindFile2( tor, i, NULL, NULL, NULL ) )
            return true;

    return false;
}

static bool
setLocalErrorIfFilesDisappeared( tr_torrent * tor )
{
    const bool disappeared = ( tr_cpHaveTotal( &tor->completion ) > 0 ) && !hasAnyLocalData( tor );

    if( disappeared )
    {
        tr_deeplog_tor( tor, "%s", "[LAZY] uh oh, the files disappeared" );
        tr_torrentSetLocalError( tor, "%s", _( "No data found! Ensure drives connected or use \"Set Location\". To re-download, remove/re-add torrent OR re-verify" ) );
    }

    return disappeared;
}

static bool
localErrFilesDisappearedUE( tr_torrent * tor )
{
    const bool disappeared = ( tr_cpHaveTotal( &tor->completion ) > 0 ) && !hasAnyLocalData( tor );

    if( disappeared )
    {
        tr_deeplog_tor( tor, "%s", "[LAZY] uh oh WARNING, the files disappeared" );
        tr_torrentSetLocalError( tor, "%s", _( "WARNING! possible user error! files MAYBE disappeared!!! to IGNORE: after verify use Start/Resume Now" ) );
    }

    return disappeared;
}

static bool
blockSizeVerySmall( tr_torrent * tor )
{
    const bool small = ( tor->blockSize < 8193 );

    tr_torinf( tor, "block size of (%d)", tor->blockSize );

    return small;
}

static bool fileExists( const char * filename, time_t * optional_mtime );

static bool
torrentDataExists( const tr_torrent * tor )
{
    char * path, * spath;
    bool exists;
    if( !tr_torrentHasMetadata( tor ) )
        return false;
    path = tr_buildPath( tr_torrentGetCurrentDir( tor ),
                         tr_torrentName( tor ), NULL );
    exists = fileExists( path, NULL );

    if( !exists )
    {
    path = NULL;
    spath = tr_strdup_printf( "%s.part", tr_torrentName( tor ) );
    path = tr_buildPath( tr_torrentGetCurrentDir( tor ), spath, NULL );
    exists = fileExists( path, NULL );
    tr_free( spath );
    if( !exists )
        exists = fileExists( tor->pieceTempDir, NULL );
    }
    tr_free( path );
    return exists;

}

static void
onSigCHLD (int i UNUSED)
{
#ifdef WIN32

  _cwait (NULL, -1, WAIT_CHILD);

#else

  int rc;
  do
    rc = waitpid (-1, NULL, WNOHANG);
  while (rc>0 || (rc==-1 && errno==EINTR));

#endif
}

static void
torrentCallScript( const tr_torrent * tor, const char * script )
{
    char timeStr[128];
    const time_t now = tr_time( );

    tr_strlcpy( timeStr, ctime( &now ), sizeof( timeStr ) );
    *strchr( timeStr,'\n' ) = '\0';

    if( script && *script )
    {
        int i;
        char * cmd[] = { tr_strdup( script ), NULL };
        char * env[] = {
            tr_strdup_printf( "TR_APP_VERSION=%s", SHORT_VERSION_STRING ),
            tr_strdup_printf( "TR_TIME_LOCALTIME=%s", timeStr ),
            tr_strdup_printf( "TR_TORRENT_DIR=%s", tor->currentDir ),
            tr_strdup_printf ("TR_TORRENT_GROUP=%s", tr_torrentGetDownloadGroup (tor)),
            tr_strdup_printf( "TR_TORRENT_ID=%d", tr_torrentId( tor ) ),
            tr_strdup_printf( "TR_TORRENT_HASH=%s", tor->info.hashString ),
            tr_strdup_printf( "TR_TORRENT_NAME=%s", tr_torrentName( tor ) ),
            NULL };

        tr_torinf( tor, "Calling script \"%s\"", script );

#ifdef WIN32
        if (_spawnvpe (_P_NOWAIT, script, (const char*)cmd, env) == -1)
          tr_torerr (tor, "error executing script \"%s\": %s", cmd[0], tr_strerror (errno));
#else
        signal( SIGCHLD, onSigCHLD );

        if( !fork( ) )
        {
            for (i=0; env[i]; ++i)
                putenv(env[i]);
				
            if (execvp (script, cmd) == -1)
              tr_torerr (tor, "error executing script \"%s\": %s", cmd[0], tr_strerror (errno));

            _exit( 0 );
        }
#endif

        for( i=0; cmd[i]; ++i ) tr_free( cmd[i] );
        for( i=0; env[i]; ++i ) tr_free( env[i] );
    }
}

static bool
path_is_bad( const char * path )
{
    if( ( path == NULL ) || ( *path == '\0' ) ) return true;
	
    //do NOT allow backward traverse
    if( !strncmp( path, "../", 3 ) ) return true;
    if( ( strstr( path, "/../" ) != NULL ) ) return true;

    // check for path end that allows backward traverse
    if( strlen( path ) > 2 ) {
        const char * endOfString = strrchr( path, '\0' );
        --endOfString;
        --endOfString;
        if( ( !strcmp( --endOfString, "/.." ) ) ) return true;
        return false;
    }

    if( ( strlen( path ) == 2 )
        && ( !strcmp( path, ".." ) ) ) return true;

    return false;
}

static void
torrentInit( tr_torrent * tor, const tr_ctor * ctor )
{
    char * s;
    int doStart;
    uint64_t loaded;
    const char * dir;
    const char * group;
    bool isNewTorrent;
    struct stat st;
    static int nextUniqueId = 1;
    tr_session * session = tr_ctorGetSession( ctor );

    assert( session != NULL );

    tr_sessionLock( session );

    tor->session   = session;
    tor->uniqueId = nextUniqueId++;
    tor->magicNumber = TORRENT_MAGIC_NUMBER;
    tor->queuePosition = session->torrentCount;
    tor->reverifyTorrent = tor->session->reverifyTorrents;

    tr_peerIdInitTor( tor );

    tr_sha1( tor->obfuscatedHash, "req2", 4,
             tor->info.hash, SHA_DIGEST_LENGTH,
             NULL );

    tor->downloadDir = NULL;
    if( !tr_ctorGetDownloadDir( ctor, TR_FORCE, &dir ) ||
        !tr_ctorGetDownloadDir( ctor, TR_FALLBACK, &dir ) )
            tor->downloadDir = tr_strdup( dir );

    tor->incompleteDir = NULL;
    if( tr_ctorGetIncompleteDir( ctor, &dir ) )
        dir = tr_sessionGetIncompleteDir( session );
    if( tr_sessionIsIncompleteDirEnabled( session ) )
        tor->incompleteDir = tr_strdup( dir );

    s = tr_metainfoGetBasename( &tor->info, TR_METAINFO_BASENAME_NAME_AND_PARTIAL_HASH, session );
    tor->pieceTempDir = tr_buildPath( tr_sessionGetPieceTempDir( tor->session ), s, NULL );
    tr_free( s );

  if (!tr_ctorGetDownloadGroup (ctor, TR_FORCE, &group) ||
    !tr_ctorGetDownloadGroup (ctor, TR_FALLBACK, &group))
  {
      tor->downloadGroup = tr_strdup (group);
  }


    tr_bandwidthConstruct( &tor->bandwidth, session, &session->bandwidth );

    tor->bandwidth.priority = tr_ctorGetBandwidthPriority( ctor );

    tor->error = TR_STAT_OK;

    tor->finishedSeedingByIdle = false;

    tr_peerMgrAddTorrent( session->peerMgr, tor );

    assert( !tor->downloadedCur );
    assert( !tor->uploadedCur );

    tr_torrentSetAddedDate( tor, tr_time( ) ); /* this is a default value to be
                                                  overwritten by the resume file */

    torrentInitFromInfo( tor );

    bool didMigrateRenamedResumeFile = false;
    loaded = tr_torrentLoadResume(tor, ~0, ctor, &didMigrateRenamedResumeFile);

    if (didMigrateRenamedResumeFile)
    {
        /* Rename torrent file as well */
        tr_metainfoMigrateFile(session, &tor->info, TR_METAINFO_BASENAME_HASH, TR_METAINFO_BASENAME_NAME_AND_PARTIAL_HASH);
    }

    tor->completeness = tr_cpGetStatus( &tor->completion );
    setLocalErrorIfFilesDisappeared( tor );

    tr_ctorInitTorrentPriorities( ctor, tor );
    tr_ctorInitTorrentWanted( ctor, tor );

    tor->currentDir = NULL;
    if( ( tor->downloadDir != NULL ) || ( tor->incompleteDir != NULL ) )
        refreshCurrentDir( tor );
//    tr_mkdirp( tor->pieceTempDir, 0777 );

    if( path_is_bad( tor->pieceTempDir ) )
    {
        tr_torrentSetLocalError( tor, "Illegal pieceTemp directory: (%s) Change default setting then remove/re-add torrent", tor->pieceTempDir );
        tor->isRunning = 0;
    }

    if( path_is_bad( tor->currentDir ) )
    {
        if( ( path_is_bad( tor->incompleteDir ) ) && ( tor->incompleteDir != NULL ) )
        {
            tr_torrentSetLocalError( tor, "Illegal incomplete directory: (%s) restart torrent to IGNORE, maybe change default incomplete directory", tor->incompleteDir );
            tor->incompleteDir = NULL;
            refreshCurrentDir( tor );
            tor->isRunning = 0;
        }
        else
        {
// should always be redundant with tor downloadDir
            tr_torrentSetLocalError( tor, "Illegal currentDir: (%s) Use torrent set-location, maybe change default download/incomplete directory, restart torrent", tor->currentDir );
            tor->isRunning = 0;
        }
    }

    if( path_is_bad( tor->downloadDir ) )
    {
        tr_torrentSetLocalError( tor, "Illegal download directory: (%s) Use torrent set-location, change default download directory if needed, and restart torrent", tor->downloadDir );
        tor->isRunning = 0;
    }

    doStart = tor->isRunning;
    tor->isRunning = 0;

    if( !( loaded & TR_FR_BLOCKLIST_OVERRIDE ) )
    {
        tor->blocklistOverride = false;
    }

    if( !( loaded & TR_FR_SPEEDLIMIT ) )
    {
        tr_torrentUseSpeedLimit( tor, TR_UP, false );
        tr_torrentSetSpeedLimit_Bps( tor, TR_UP, tr_sessionGetSpeedLimit_Bps( tor->session, TR_UP ) );
        tr_torrentUseSpeedLimit( tor, TR_DOWN, false );
        tr_torrentSetSpeedLimit_Bps( tor, TR_DOWN, tr_sessionGetSpeedLimit_Bps( tor->session, TR_DOWN ) );
        tr_torrentUseSessionLimits( tor, true );
    }

    if( !( loaded & TR_FR_RATIOLIMIT ) )
    {
        tr_torrentSetRatioMode( tor, TR_RATIOLIMIT_GLOBAL );
        tr_torrentSetRatioLimit( tor, tr_sessionGetRatioLimit( tor->session ) );
    }

    if( !( loaded & TR_FR_IDLELIMIT ) )
    {
        tr_torrentSetIdleMode( tor, TR_IDLELIMIT_GLOBAL );
        tr_torrentSetIdleLimit( tor, tr_sessionGetIdleLimit( tor->session ) );
    }

    if( !( loaded & TR_FR_STREAMINGMODE ) )
    {
        tr_torrentSetStreamingMode( tor, tor->session->streamModeDefault );
    }

    if( !( loaded & TR_FR_CHEATMODE ) )
    {
        tr_torrentSetCheatMode( tor, tor->session->cheatModeDefault );
    }

    if( !( loaded & TR_FR_CHEAT_RATIO )
        || ( tor->cheatRatio < (float)(-1.9) ) || ( tor->cheatRatio > (float)(99) ) )
    {
        tor->cheatRatio = 0;
    }

    // random float, range 0.0 to 0.1
    tor->cheatRand = ( (float)tr_cryptoRandInt(100000)/1000000 ) + tor->cheatRatio;

    /* add the torrent to tr_session.torrentList */
    session->torrentCount++;
    if( session->torrentList == NULL )
        session->torrentList = tor;
    else {
        tr_torrent * it = session->torrentList;
        while( it->next != NULL )
            it = it->next;
        it->next = tor;
    }

    /* if we don't have a local .torrent file already, assume the torrent is new */
    isNewTorrent = stat( tor->info.torrent, &st );

    /* maybe save our own copy of the metainfo */
    if( tr_ctorGetSave( ctor ) )
    {
        const tr_benc * val;
        if( !tr_ctorGetMetainfo( ctor, &val ) )
        {
            const char * path = tor->info.torrent;
            const int err = tr_bencToFile( val, TR_FMT_BENC_TORRENT, path );
            if( err )
                tr_torrentSetLocalError( tor, "Unable to save torrent file: %s", tr_strerror( err ) );
            tr_sessionSetTorrentFile( tor->session, tor->info.hashString, path );
        }
    }

    tor->tiers = tr_announcerAddTorrent( tor, onTrackerResponse, NULL );

    if( isNewTorrent )
    {
        if( tor->session->prefetchMagnets && !tr_torrentHasMetadata( tor ) && ( doStart == 0 ) )
            {
                tor->preFetchMagnet = true;
                torrentStart( tor, true );
            }
        else
            {
                tor->startAfterVerify = false;
                if( tr_torrentHasMetadata( tor ) )
                    {
                        tr_torrentVerify( tor );

                        if( ( !path_is_bad( tor->pieceTempDir ) ) )
                            tr_mkdirp( tor->pieceTempDir, 0777 );

                        if (tr_sessionIsTorrentAddedScriptEnabled (tor->session))
                          torrentCallScript (tor, tr_sessionGetTorrentAddedScript (tor->session));
                    }

                if( doStart ) torrentStart( tor, false );
            }
    }
    else if( doStart )
    {
        tr_torrentStart( tor );
    }

    tr_sessionUnlock( session );
}

static tr_parse_result
torrentParseImpl( const tr_ctor * ctor, tr_info * setmeInfo,
                  bool * setmeHasInfo, int * dictLength )
{
    int             doFree;
    bool            didParse;
    bool            hasInfo = false;
    tr_info         tmp;
    const tr_benc * metainfo;
    tr_session    * session = tr_ctorGetSession( ctor );
    tr_parse_result result = TR_PARSE_OK;

    if( setmeInfo == NULL )
        setmeInfo = &tmp;
    memset( setmeInfo, 0, sizeof( tr_info ) );

    if( tr_ctorGetMetainfo( ctor, &metainfo ) )
        return TR_PARSE_ERR;

    didParse = tr_metainfoParse( session, metainfo, setmeInfo,
                                 &hasInfo, dictLength );
    doFree = didParse && ( setmeInfo == &tmp );

    if( !didParse )
        result = TR_PARSE_ERR;

    if( didParse && hasInfo && !tr_getBlockSize( setmeInfo->pieceSize ) )
        result = TR_PARSE_ERR;

    if( didParse && session && tr_torrentExists( session, setmeInfo->hash ) )
        result = TR_PARSE_DUPLICATE;

    if( doFree )
        tr_metainfoFree( setmeInfo );

    if( setmeHasInfo != NULL )
        *setmeHasInfo = hasInfo;

    return result;
}

tr_parse_result
tr_torrentParse( const tr_ctor * ctor, tr_info * setmeInfo )
{
    return torrentParseImpl( ctor, setmeInfo, NULL, NULL );
}

tr_torrent *
tr_torrentNew( const tr_ctor * ctor, int * setmeError )
{
    int len;
    bool hasInfo;
    tr_info tmpInfo;
    tr_parse_result r;
    tr_torrent * tor = NULL;

    assert( ctor != NULL );
    assert( tr_isSession( tr_ctorGetSession( ctor ) ) );

    r = torrentParseImpl( ctor, &tmpInfo, &hasInfo, &len );
    if( r == TR_PARSE_OK )
    {
        tor = tr_new0( tr_torrent, 1 );
        tor->info = tmpInfo;
        if( hasInfo )
            tor->infoDictLength = len;
        torrentInit( tor, ctor );
    }
    else
    {
        if( r == TR_PARSE_DUPLICATE )
            tr_metainfoFree( &tmpInfo );

        if( setmeError )
            *setmeError = r;
    }

    return tor;
}

/**
***
**/

 void
tr_torrentSetDownloadGroup (tr_torrent * tor, const char * group)
{
  assert (tr_isTorrent (tor));

  if (!group || !tor->downloadGroup || strcmp (group, tor->downloadGroup))
    {
      tr_free (tor->downloadGroup);
      tor->downloadGroup = tr_strdup (group);
      tor->anyDate = tr_time ();
      tr_torrentSetDirty (tor);
    }
}

const char*
tr_torrentGetDownloadGroup (const tr_torrent * tor)
{
  assert (tr_isTorrent (tor));

  return tor->downloadGroup;
}


void
tr_torrentSetDownloadDir( tr_torrent * tor, const char * path )
{
    assert( tr_isTorrent( tor  ) );

    if( !path || !tor->downloadDir || strcmp( path, tor->downloadDir ) )
    {
        tr_free( tor->downloadDir );
        tor->downloadDir = tr_strdup( path );
        tr_torrentSetDirty( tor );
    }

    refreshCurrentDir( tor );
}

const char*
tr_torrentGetDownloadDir( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    return tor->downloadDir;
}

const char *
tr_torrentGetCurrentDir( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    return tor->currentDir;
}


void
tr_torrentChangeMyPort( tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    if( tor->isRunning )
        tr_announcerChangeMyPort( tor );
}

static inline void
tr_torrentManualUpdateImpl( void * vtor )
{
    tr_torrent * tor = vtor;

    assert( tr_isTorrent( tor  ) );

    if( tor->isRunning )
        tr_announcerManualAnnounce( tor );
}

void
tr_torrentManualUpdate( tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    tr_runInEventThread( tor->session, tr_torrentManualUpdateImpl, tor );
}

bool
tr_torrentCanManualUpdate( const tr_torrent * tor )
{
    return ( tr_isTorrent( tor  ) )
        && ( tor->isRunning )
        && ( tr_announcerCanManualAnnounce( tor ) );
}

const tr_info *
tr_torrentInfo( const tr_torrent * tor )
{
    return tr_isTorrent( tor ) ? &tor->info : NULL;
}

const tr_stat *
tr_torrentStatCached( tr_torrent * tor )
{
    const time_t now = tr_time( );

    return tr_isTorrent( tor ) && ( now == tor->lastStatTime )
         ? &tor->stats
         : tr_torrentStat( tor );
}

void
tr_torrentSetVerifyState( tr_torrent * tor, tr_verify_state state )
{
    assert( tr_isTorrent( tor ) );
    assert( state==TR_VERIFY_NONE || state==TR_VERIFY_WAIT || state==TR_VERIFY_NOW );

    tor->verifyState = state;
    tor->anyDate = tr_time( );
}

static tr_torrent_activity
torrentGetActivity( const tr_torrent * tor )
{
    const bool is_seed = tr_torrentIsSeed( tor );
    assert( tr_isTorrent( tor ) );

    if( tor->verifyState == TR_VERIFY_NOW )
        return TR_STATUS_CHECK;

    if( tor->verifyState == TR_VERIFY_WAIT )
        return TR_STATUS_CHECK_WAIT;

    if( tor->isRunning )
        return is_seed ? TR_STATUS_SEED : TR_STATUS_DOWNLOAD;

    if( tr_torrentIsQueued( tor ) ) {
        if( is_seed && tr_sessionGetQueueEnabled( tor->session, TR_UP ) )
            return TR_STATUS_SEED_WAIT;
        if( !is_seed && tr_sessionGetQueueEnabled( tor->session, TR_DOWN ) )
            return TR_STATUS_DOWNLOAD_WAIT;
    }

    return TR_STATUS_STOPPED;
}

tr_torrent_activity
tr_torrentGetActivity( tr_torrent * tor )
{
    /* FIXME: is this call still needed? */
    tr_torrentRecheckCompleteness( tor );

    return torrentGetActivity( tor );
}

static time_t
torrentGetIdleSecs( const tr_torrent * tor )
{
    int idle_secs;
    const tr_torrent_activity activity = torrentGetActivity( tor );

    if ((activity == TR_STATUS_DOWNLOAD || activity == TR_STATUS_SEED) && tor->startDate != 0)
        idle_secs = difftime(tr_time(), MAX(tor->startDate, tor->activityDate));
    else
        idle_secs = -1;

    return idle_secs;
}

bool
tr_torrentIsStalled( const tr_torrent * tor )
{
    return tr_sessionGetQueueStalledEnabled( tor->session )
        && ( torrentGetIdleSecs( tor ) > ( tr_sessionGetQueueStalledMinutes( tor->session ) * 60 ) );
}


static double
getVerifyProgress( const tr_torrent * tor )
{
    double d = 0;

    assert( tr_isTorrent( tor ) );

    if( tr_torrentHasMetadata( tor ) )
    {
        tr_piece_index_t i, n;
        tr_piece_index_t checked = 0;

        for( i=0, n=tor->info.pieceCount; i!=n; ++i )
            if( tor->info.pieces[i].checked )
                ++checked;

        d = checked / (double)tor->info.pieceCount;
    }

    return d;
}

const tr_stat *
tr_torrentStat( tr_torrent * tor )
{
    tr_stat *               s;
    uint64_t                now;
    uint64_t                seedRatioBytesLeft;
    uint64_t                seedRatioBytesGoal;
    bool                    seedRatioApplies;
    uint16_t                seedIdleMinutes;

    if( !tor )
        return NULL;

    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    tor->lastStatTime = tr_time( );

    s = &tor->stats;
    s->id = tor->uniqueId;
    s->activity = tr_torrentGetActivity( tor );
    s->error = tor->error;
    s->queuePosition = tor->queuePosition;
    s->isStalled = tr_torrentIsStalled( tor );
    tr_strlcpy( s->errorString, tor->errorString, sizeof( s->errorString ) );

    s->manualAnnounceTime = tr_announcerNextManualAnnounce( tor );

    tr_peerMgrTorrentStats( tor,
                            &s->peersConnected,
                            &s->webseedsSendingToUs,
                            &s->peersSendingToUs,
                            &s->peersGettingFromUs,
                            s->peersFrom );

    now = tr_time_msec( );
    s->rawUploadSpeed_KBps     = toSpeedKBps( tr_bandwidthGetRawSpeed_Bps  ( &tor->bandwidth, now, TR_UP ) );
    s->pieceUploadSpeed_KBps   = toSpeedKBps( tr_bandwidthGetPieceSpeed_Bps( &tor->bandwidth, now, TR_UP ) );
    s->rawDownloadSpeed_KBps   = toSpeedKBps( tr_bandwidthGetRawSpeed_Bps  ( &tor->bandwidth, now, TR_DOWN ) );
    s->pieceDownloadSpeed_KBps = toSpeedKBps( tr_bandwidthGetPieceSpeed_Bps( &tor->bandwidth, now, TR_DOWN ) );

    s->percentComplete = tr_cpPercentComplete ( &tor->completion );
    s->metadataPercentComplete = tr_torrentGetMetadataPercent( tor );

    s->percentDone         = tr_cpPercentDone  ( &tor->completion );
    s->leftUntilDone       = tr_cpLeftUntilDone( &tor->completion );
    s->sizeWhenDone        = tr_cpSizeWhenDone ( &tor->completion );
    s->recheckProgress     = s->activity == TR_STATUS_CHECK ? getVerifyProgress( tor ) : 0;
    s->activityDate        = tor->activityDate;
    s->addedDate           = tor->addedDate;
    s->doneDate            = tor->doneDate;
    s->startDate           = tor->startDate;
    s->secondsSeeding      = tor->secondsSeeding;
    s->secondsDownloading  = tor->secondsDownloading;
    s->idleSecs            = torrentGetIdleSecs( tor );

    s->corruptEver      = tor->corruptCur    + tor->corruptPrev;
    s->downloadedEver   = tor->downloadedCur + tor->downloadedPrev;
    s->uploadedEver     = tor->uploadedCur   + tor->uploadedPrev;
    s->haveValid        = tr_cpHaveValid( &tor->completion );
    s->haveUnchecked    = tr_cpHaveTotal( &tor->completion ) - s->haveValid;
    s->desiredAvailable = tr_peerMgrGetDesiredAvailable( tor );

    s->ratio = tr_getRatio( s->uploadedEver,
                            s->downloadedEver ? s->downloadedEver : s->haveValid );

    seedRatioApplies = tr_torrentGetSeedRatioBytes( tor, &seedRatioBytesLeft,
                                                         &seedRatioBytesGoal );

    switch( s->activity )
    {
        /* etaXLSpeed exists because if we use the piece speed directly,
         * brief fluctuations cause the ETA to jump all over the place.
         * so, etaXLSpeed is a smoothed-out version of the piece speed
         * to dampen the effect of fluctuations */

        case TR_STATUS_DOWNLOAD:
            if( ( tor->etaDLSpeedCalculatedAt + 800 ) < now ) {
                tor->etaDLSpeed_KBps = ( ( tor->etaDLSpeedCalculatedAt + 4000 ) < now )
                    ? s->pieceDownloadSpeed_KBps /* if no recent previous speed, no need to smooth */
                    : ((tor->etaDLSpeed_KBps*4.0) + s->pieceDownloadSpeed_KBps)/5.0; /* smooth across 5 readings */
                tor->etaDLSpeedCalculatedAt = now;
            }

            if( (s->leftUntilDone > s->desiredAvailable) && (tor->info.webseedCount < 1) )
                s->eta = TR_ETA_NOT_AVAIL;
            else if( tor->etaDLSpeed_KBps < 1 )
                s->eta = TR_ETA_UNKNOWN;
            else
                s->eta = s->leftUntilDone / toSpeedBytes(tor->etaDLSpeed_KBps);

            s->etaIdle = TR_ETA_NOT_AVAIL;
            break;

        case TR_STATUS_SEED: {
            if( !seedRatioApplies )
                s->eta = TR_ETA_NOT_AVAIL;
            else {
                if( ( tor->etaULSpeedCalculatedAt + 800 ) < now ) {
                    tor->etaULSpeed_KBps = ( ( tor->etaULSpeedCalculatedAt + 4000 ) < now )
                        ? s->pieceUploadSpeed_KBps /* if no recent previous speed, no need to smooth */
                        : ((tor->etaULSpeed_KBps*4.0) + s->pieceUploadSpeed_KBps)/5.0; /* smooth across 5 readings */
                    tor->etaULSpeedCalculatedAt = now;
                }
                if( tor->etaULSpeed_KBps < 1 )
                    s->eta = TR_ETA_UNKNOWN;
                else
                    s->eta = seedRatioBytesLeft / toSpeedBytes(tor->etaULSpeed_KBps);
            }

            if( tor->etaULSpeed_KBps < 1 && tr_torrentGetSeedIdle( tor, &seedIdleMinutes ) )
                s->etaIdle = seedIdleMinutes * 60 - s->idleSecs;
            else
                s->etaIdle = TR_ETA_NOT_AVAIL;
            break;
        }

        default:
            s->eta = TR_ETA_NOT_AVAIL;
            s->etaIdle = TR_ETA_NOT_AVAIL;
            break;
    }

    /* s->haveValid is here to make sure a torrent isn't marked 'finished'
     * when the user hits "uncheck all" prior to starting the torrent... */
    s->finished = tor->finishedSeedingByIdle || (seedRatioApplies && !seedRatioBytesLeft && s->haveValid);

    if( !seedRatioApplies || s->finished )
        s->seedRatioPercentDone = 1;
    else if( !seedRatioBytesGoal ) /* impossible? safeguard for div by zero */
        s->seedRatioPercentDone = 0;
    else
        s->seedRatioPercentDone = (double)(seedRatioBytesGoal - seedRatioBytesLeft) / seedRatioBytesGoal;

    tr_torrentUnlock( tor );

    /* test some of the constraints */
    assert( s->sizeWhenDone <= tor->info.totalSize );
    assert( s->leftUntilDone <= s->sizeWhenDone );
    assert( s->desiredAvailable <= s->leftUntilDone );
    return s;
}

/***
****
***/

static uint64_t
fileBytesCompleted( const tr_torrent * tor, tr_file_index_t index )
{
    uint64_t total = 0;
    const tr_file * f = &tor->info.files[index];

    if( f->length )
    {
        tr_block_index_t first;
        tr_block_index_t last;
        tr_torGetFileBlockRange( tor, index, &first, &last );

        if( first == last )
        {
            if( tr_cpBlockIsComplete( &tor->completion, first ) )
                total = f->length;
        }
        else
        {
            /* the first block */
            if( tr_cpBlockIsComplete( &tor->completion, first ) )
                total += tor->blockSize - ( f->offset % tor->blockSize );

            /* the middle blocks */
            if( first + 1 < last ) {
                uint64_t u = tr_bitfieldCountRange( &tor->completion.blockBitfield, first+1, last );
                u *= tor->blockSize;
                total += u;
            }

            /* the last block */
            if( tr_cpBlockIsComplete( &tor->completion, last ) )
                total += ( f->offset + f->length ) - ( (uint64_t)tor->blockSize * last );
        }
    }

    return total;
}

tr_file_stat *
tr_torrentFiles( const tr_torrent * tor,
                 tr_file_index_t *  fileCount )
{
    tr_file_index_t       i;
    const tr_file_index_t n = tor->info.fileCount;
    tr_file_stat *        files = tr_new0( tr_file_stat, n );
    tr_file_stat *        walk = files;
    const bool            isSeed = tor->completeness == TR_SEED;

    assert( tr_isTorrent( tor ) );

    for( i=0; i<n; ++i, ++walk ) {
        const uint64_t b = isSeed ? tor->info.files[i].length : fileBytesCompleted( tor, i );
        walk->bytesCompleted = b;
        walk->progress = tor->info.files[i].length > 0 ? ( (float)b / tor->info.files[i].length ) : 1.0f;
    }

    if( fileCount )
        *fileCount = n;

    return files;
}

void
tr_torrentFilesFree( tr_file_stat *            files,
                     tr_file_index_t fileCount UNUSED )
{
    tr_free( files );
}

/***
****
***/

double*
tr_torrentWebSpeeds_KBps( const tr_torrent * tor )
{
    double * ret = NULL;

    if( tr_isTorrent( tor ) )
    {
        tr_torrentLock( tor );
        ret = tr_peerMgrWebSpeeds_KBps( tor );
        tr_torrentUnlock( tor );
    }

    return ret;
}

tr_peer_stat *
tr_torrentPeers( const tr_torrent * tor, int * peerCount )
{
    tr_peer_stat * ret = NULL;

    if( tr_isTorrent( tor ) )
    {
        tr_torrentLock( tor );
        ret = tr_peerMgrPeerStats( tor, peerCount );
        tr_torrentUnlock( tor );
    }

    return ret;
}

void
tr_torrentPeersFree( tr_peer_stat * peers, int peerCount UNUSED )
{
    tr_free( peers );
}

tr_tracker_stat *
tr_torrentTrackers( const tr_torrent * torrent, int * setmeTrackerCount )
{
    tr_tracker_stat * ret = NULL;

    if( tr_isTorrent( torrent ) )
    {
        tr_torrentLock( torrent );
        ret = tr_announcerStats( torrent, setmeTrackerCount );
        tr_torrentUnlock( torrent );
    }

    return ret;
}

void
tr_torrentTrackersFree( tr_tracker_stat * trackers, int trackerCount )
{
    tr_announcerStatsFree( trackers, trackerCount );
}

void
tr_torrentAvailability( const tr_torrent * tor, int8_t * tab, int size )
{
    if( tr_isTorrent( tor ) && ( tab != NULL ) && ( size > 0 ) )
    {
        tr_torrentLock( tor );
        tr_peerMgrTorrentAvailability( tor, tab, size );
        tr_torrentUnlock( tor );
    }
}

void
tr_torrentAmountFinished( const tr_torrent * tor,
                          float *            tab,
                          int                size )
{
    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );
    tr_cpGetAmountDone( &tor->completion, tab, size );
    tr_torrentUnlock( tor );
}

static void
tr_torrentResetTransferStats( tr_torrent * tor )
{
    tr_torrentLock( tor );

    tor->downloadedPrev += tor->downloadedCur;
    tor->downloadedCur   = 0;
    tor->uploadedPrev   += tor->uploadedCur;
    tor->uploadedCur     = 0;
    tor->corruptPrev    += tor->corruptCur;
    tor->corruptCur      = 0;

    tr_torrentSetDirty( tor );

    tr_torrentUnlock( tor );
}

void
tr_torrentSetHasPiece( tr_torrent *     tor,
                       tr_piece_index_t pieceIndex,
                       bool             has )
{
    assert( tr_isTorrent( tor ) );
    assert( pieceIndex < tor->info.pieceCount );

    if( has )
        tr_cpPieceAdd( &tor->completion, pieceIndex );
    else
        tr_cpPieceRem( &tor->completion, pieceIndex );
}

/***
****
***/

#ifndef NDEBUG
static bool queueIsSequenced( tr_session * );
#endif

static void
freeTorrent( tr_torrent * tor )
{
    tr_torrent * t;
    tr_session *  session = tor->session;
    tr_info *    inf = &tor->info;
    const time_t now = tr_time( );

    assert( !tor->isRunning );

    tr_sessionLock( session );

    tr_peerMgrRemoveTorrent( tor );

    tr_announcerRemoveTorrent( session->announcer, tor );

    tr_cpDestruct( &tor->completion );

    tr_free( tor->downloadDir );
    tr_free( tor->pieceTempDir );
    tr_free( tor->incompleteDir );
    tr_free( tor->downloadGroup );

    if( tor == session->torrentList )
        session->torrentList = tor->next;
    else for( t = session->torrentList; t != NULL; t = t->next ) {
        if( t->next == tor ) {
            t->next = tor->next;
            break;
        }
    }

    /* decrement the torrent count */
    assert( session->torrentCount >= 1 );
    session->torrentCount--;

    /* resequence the queue positions */
    t = NULL;
    while(( t = tr_torrentNext( session, t ))) {
        if( t->queuePosition > tor->queuePosition ) {
            t->queuePosition--;
            t->anyDate = now;
        }
    }
    assert( queueIsSequenced( session ) );

    tr_bandwidthDestruct( &tor->bandwidth );

    tr_metainfoFree( inf );
    memset( tor, ~0, sizeof( tr_torrent ) );
    tr_free( tor );

    tr_sessionUnlock( session );
}

/**
***  Start/Stop Callback
**/

static void torrentSetQueued( tr_torrent * tor, bool queued );

static void
torrentStartImpl( void * vtor )
{
    time_t now;
    tr_torrent * tor = vtor;

    assert( tr_isTorrent( tor ) );

    tr_sessionLock( tor->session );

    tr_torrentRecheckCompleteness( tor );
    torrentSetQueued( tor, false );

    if( tor->session->isBlocklistEnabled ) {
        // turn off blocklists if not already overrride and the torrent directs this
        if( tor->blocklistOverride && !tor->session->isBlocklistOverride )
            tr_blocklistSetOverride( tor->session, true );
        // put blocklists back on if previous override and the torrent directs this
        else if( !tor->blocklistOverride && tor->session->isBlocklistOverride )
            tr_blocklistSetOverride( tor->session, false );
    }

    now = tr_time( );
    tor->isRunning = true;
    tor->completeness = tr_cpGetStatus( &tor->completion );
    tor->startDate = tor->anyDate = now;
    tr_torrentClearError( tor );
    tor->finishedSeedingByIdle = false;

    tr_torrentResetTransferStats( tor );
    tr_announcerTorrentStarted( tor );
    tor->dhtAnnounceAt = now + tr_cryptoWeakRandInt( 20 );
    tor->dhtAnnounce6At = now + tr_cryptoWeakRandInt( 20 );
    tor->lpdAnnounceAt = now;
    tr_peerMgrStartTorrent( tor );

    tr_sessionUnlock( tor->session );
}

uint64_t
tr_torrentGetCurrentSizeOnDisk( const tr_torrent * tor )
{
    tr_file_index_t i;
    uint64_t byte_count = 0;
    const tr_file_index_t n = tor->info.fileCount;

    for( i=0; i<n; ++i )
    {
        struct stat sb;
        char * filename = tr_torrentFindFile( tor, i );

        sb.st_size = 0;
        if( filename && !stat( filename, &sb ) )
            byte_count += sb.st_size;

        tr_free( filename );
    }

    return byte_count;
}

static bool
torrentShouldQueue( const tr_torrent * tor )
{
    const tr_direction dir = tr_torrentGetQueueDirection( tor );

    return tr_sessionCountQueueFreeSlots( tor->session, dir ) == 0;
}

static void
torrentStart( tr_torrent * tor, bool bypass_queue )
{

    if( path_is_bad( tor->downloadDir ) )
    {
        tr_torrentSetLocalError( tor, "Illegal download directory: (%s) Use torrent set-location, change default download directory if needed, and restart torrent", tor->downloadDir );
        return;
    }

    if( path_is_bad( tor->currentDir ) )
    {
        if( ( path_is_bad( tor->incompleteDir ) ) && ( tor->incompleteDir != NULL ) )
        {
            tr_torrentSetLocalError( tor, "Illegal incomplete directory: (%s) Change default setting then remove/re-add torrent", tor->incompleteDir );
            return;
        }
        else
// this should never happen
        {
            tr_torrentSetLocalError( tor, "Illegal currentDir: (%s) Maybe change default incomplete or download directory and restart torrent", tor->currentDir );
            return;
        }
    }

    if( path_is_bad( tor->pieceTempDir ) )
    {
        tr_torrentSetLocalError( tor, "Illegal pieceTemp directory: (%s) Change default setting then remove/re-add torrent", tor->pieceTempDir );
        return;
    }

    if( blockSizeVerySmall( tor ) && !bypass_queue && tr_torrentHasMetadata( tor ) )
    {
        tr_torrentSetLocalError( tor, "WARNING!: small block size (%d), use Start/Resume Now to start", tor->blockSize );
        return;
    }

    switch( torrentGetActivity( tor ) )
    {
        case TR_STATUS_SEED:
        case TR_STATUS_DOWNLOAD:
            return; /* already started */
            break;

        case TR_STATUS_SEED_WAIT:
        case TR_STATUS_DOWNLOAD_WAIT:
            if( !bypass_queue )
                return; /* already queued */
            break;

        case TR_STATUS_CHECK:
        case TR_STATUS_CHECK_WAIT:
            /* verifying right now... wait until that's done so
             * we'll know what completeness to use/announce */
            if( fileExists( tor->pieceTempDir, NULL ) )
                tor->startAfterVerify = true;
//            else
//                tor->startAfterVerify = false;
            return;
            break;

        case TR_STATUS_STOPPED:
            if( !bypass_queue && torrentShouldQueue( tor ) ) {
                torrentSetQueued( tor, true );
                return;
            }
            break;
    }

    /* don't allow the torrent to be started if the files disappeared */
    /* IGNORE the error and start if Start/Resume Now issued */
    if( !bypass_queue && localErrFilesDisappearedUE( tor ) )
        return;

    /* otherwise, start it now... */
    tr_sessionLock( tor->session );

    /* allow finished torrents to be resumed */
    if( tr_torrentIsSeedRatioDone( tor ) ) {
        tr_torinf( tor, _( "Restarted manually -- disabling its seed ratio" ) );
        tr_torrentSetRatioMode( tor, TR_RATIOLIMIT_UNLIMITED );
    }

    /* corresponds to the peer_id sent as a tracker request parameter.
     * one tracker admin says: "When the same torrent is opened and
     * closed and opened again without quitting Transmission ...
     * change the peerid. It would help sometimes if a stopped event
     * was missed to ensure that we didn't think someone was cheating. */
    tr_peerIdInitTor( tor );
    tor->isRunning = 1;
    tr_torrentSetDirty( tor );
    tr_runInEventThread( tor->session, torrentStartImpl, tor );

    tr_sessionUnlock( tor->session );
}

void
tr_torrentStart( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
        torrentStart( tor, false );
}

void
tr_torrentStartNow( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
        torrentStart( tor, true );
}

static void
torrentRecheckDoneImpl( void * vtor )
{
    tr_torrent * tor = vtor;
    assert( tr_isTorrent( tor ) );

    tr_torrentRecheckCompleteness( tor );

    if( tor->startAfterVerify ) {
        tor->startAfterVerify = false;
        torrentStart( tor, false );
    }
}

static void
torrentRecheckDoneCB( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    tr_runInEventThread( tor->session, torrentRecheckDoneImpl, tor );
}

static bool
quickVerify( tr_torrent * tor )
{
    if( torrentDataExists( tor ) )
        return false;
    tr_cpReset( &tor->completion );
    tor->anyDate = tr_time( );
    tr_torrentSetDirty( tor );
    return true;
}

static void
verifyTorrent( void * vtor )
{
    tr_torrent * tor = vtor;

    tr_sessionLock( tor->session );

    /* if the torrent's already being verified, stop it */
    tr_verifyRemove( tor );

    /* if the torrent's running, stop it & set the restart-after-verify flag */
    if( tor->startAfterVerify || tor->isRunning ) {
        /* don't clobber isStopping */
        const bool startAfter = tor->isStopping ? false : true;
        tr_torrentStop( tor );
        if( fileExists( tor->pieceTempDir, NULL ) )
            tor->startAfterVerify = startAfter;
        else
            tor->startAfterVerify = false;
    }

    if( localErrFilesDisappearedUE( tor ) )
        tor->startAfterVerify = false;
//    else
//    {
        if( quickVerify( tor ) )
            torrentRecheckDoneCB( tor );
        else
            tr_verifyAdd( tor, torrentRecheckDoneCB );
//    }

    tr_sessionUnlock( tor->session );
}

void
tr_torrentVerify( tr_torrent * tor )
{
            if( ( tr_isTorrent( tor ) ) && ( !path_is_bad( tor->currentDir ) )  && ( !path_is_bad( tor->pieceTempDir ) ) )
                tr_runInEventThread( tor->session, verifyTorrent, tor );
}

static void
setExistingFilesVerified( tr_torrent * tor )
{
    tr_file_index_t fi;
    tr_piece_index_t pi;
    const tr_info * info = tr_torrentInfo( tor );
    bool * missing = tr_new0( bool, info->pieceCount );
    tr_cheatMode_t cheatMode = tr_torrentGetCheatMode( tor );

    if(cheatMode == TR_CHEAT_ALWSEED)
    {
        for( pi = 0; pi < info->pieceCount; ++pi )
        {
            tr_torrentSetHasPiece( tor, pi, true );
            tr_torrentSetPieceChecked( tor, pi );
        }
    }
    else
    {
        for( fi = 0; fi < info->fileCount; ++fi  )
        {
            const tr_file * file = &info->files[fi];
            const bool have = !file->dnd
                && tr_torrentFindFile2( tor, fi, NULL, NULL, NULL );

            for( pi = file->firstPiece; pi <= file->lastPiece; ++pi )
                if( !missing[pi] && !have )
                    missing[pi] = true;
        }

        for( pi = 0; pi < info->pieceCount; ++pi )
        {
            tr_torrentSetHasPiece( tor, pi, !missing[pi] );
            if( !missing[pi] )
                tr_torrentSetPieceChecked( tor, pi );
        }
    }

    tr_free( missing );
}

static void
setTorrentFilesVerified( void * vtor )
{
    tr_torrent * tor = vtor;
    bool startAfter = false;

    assert( tr_isTorrent( tor ) );
    tr_sessionLock( tor->session );

    tr_verifyRemove( tor );
    if( ( fileExists( tor->pieceTempDir, NULL ) ) && ( tor->startAfterVerify || tor->isRunning ) ) {
        startAfter = !tor->isStopping;

//        if( !fileExists( tor->pieceTempDir, NULL ) )
//            startAfter = false;

        tr_torrentStop( tor );
    }

    setExistingFilesVerified( tor );
    tor->anyDate = tr_time( );
    tr_torrentRecheckCompleteness( tor );

    if( startAfter )
        torrentStart( tor, true );

    tr_sessionUnlock( tor->session );
}

void
tr_torrentSetFilesVerified( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
        tr_runInEventThread( tor->session, setTorrentFilesVerified, tor );
}

void
tr_torrentSave( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tor->isDirty )
    {
        tor->isDirty = false;
        tr_torrentSaveResume( tor );
    }
}

static void
stopTorrent( void * vtor )
{
    tr_torrent * tor = vtor;
    tr_torinf( tor, "Pausing" );

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );

    tr_verifyRemove( tor );
    const bool wasQueued = tr_torrentIsQueued( tor );
    tr_peerMgrStopTorrent( tor );

    if( !wasQueued )
        tr_announcerTorrentStopped( tor );

    tr_cacheFlushTorrent( tor->session->cache, tor );

    tr_fdTorrentClose( tor->session, tor->uniqueId );

    if( !tor->isDeleting )
        tr_torrentSave( tor );

    torrentSetQueued( tor, false );

    tr_torrentUnlock( tor );

    if( tor->magnetVerify ) {
        tor->magnetVerify = false;
        tr_torinf( tor, "Magnet Verify" );
        refreshCurrentDir( tor );
        const bool startMagAfter = tor->startAfterVerify;
        tor->startAfterVerify = false;
        tr_torrentVerify( tor );
        tr_mkdirp( tor->pieceTempDir, 0777 );

        if (tr_sessionIsTorrentAddedScriptEnabled (tor->session))
          torrentCallScript (tor, tr_sessionGetTorrentAddedScript (tor->session));

        if( startMagAfter ) torrentStart( tor, false );

    }
}

void
tr_torrentStop( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tr_isTorrent( tor ) )
    {
        tr_sessionLock( tor->session );

        tor->isRunning = 0;
        tor->isStopping = 0;
        tor->preFetchMagnet = 0;
        tr_torrentSetDirty( tor );
        tr_runInEventThread( tor->session, stopTorrent, tor );

        tr_sessionUnlock( tor->session );
    }
}

static void
deleteLocalFile( const char * filename, tr_fileFunc fileFunc )
{
    struct stat sb;
    if( !stat( filename, &sb ) ) /* if file exists... */
        fileFunc( filename );
}

/**
 * @brief Delete all temporary piece files for the torrent.
 */
static void
tr_torrentRemovePieceTemp( tr_torrent * tor )
{
    DIR * dir;
    struct dirent * d;
    tr_list * l;
    tr_list * files = NULL;
    const char * path = tor->pieceTempDir;

    if( ( dir = opendir( path ) ) )
    {
        while( ( d = readdir( dir ) ) )
        {
            const char * name = d->d_name;

            if( name && strcmp( name, "." ) && strcmp( name, ".." ) )
                tr_list_append( &files, tr_buildPath( path, name, NULL ) );
        }

        closedir( dir );
        tr_list_append( &files, tr_strdup( path ) );
    }

    for( l = files; l != NULL; l = l->next )
        deleteLocalFile( l->data, remove );

    tr_list_free( &files, tr_free );
}


static void
closeTorrent( void * vtor )
{
    tr_benc * d;
    tr_torrent * tor = vtor;

    assert( tr_isTorrent( tor ) );

    d = tr_bencListAddDict( &tor->session->removedTorrents, 2 );
    tr_bencDictAddInt( d, "id", tor->uniqueId );
    tr_bencDictAddInt( d, "date", tr_time( ) );

    tr_torinf( tor, "%s", _( "Removing torrent" ) );

    tor->magnetVerify = false;
    stopTorrent( tor );

    if( tor->isDeleting )
    {
        tr_metainfoRemoveSaved( tor->session, &tor->info );
        tr_torrentRemoveResume( tor );

        if( path_is_bad( tor->pieceTempDir ) )
	        tr_torerr( tor, "bad path -- skip deleting pieceTemp directory \"%s\" ", tor->pieceTempDir );
        else
            tr_torrentRemovePieceTemp( tor );

    }

    tor->isRunning = 0;
    freeTorrent( tor );
}

void
tr_torrentFree( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
    {
        tr_session * session = tor->session;
        assert( tr_isSession( session ) );
        tr_sessionLock( session );

        tr_torrentClearCompletenessCallback( tor );
        tr_runInEventThread( session, closeTorrent, tor );

        tr_sessionUnlock( session );
    }
}

struct remove_data
{
    tr_torrent   * tor;
    bool           deleteFlag;
    tr_fileFunc  * deleteFunc;
};

static void tr_torrentDeleteLocalData( tr_torrent *, tr_fileFunc );

static void
removeTorrent( void * vdata )
{
    struct remove_data * data = vdata;

// July 25 2014 -cfp- we need to make sure the torrent is stopped
// and locked to prevent crashes with stalled peer io and cache
    tr_session * session = data->tor->session;
    tr_sessionLock( session );

    if( data->deleteFlag )
    {
        stopTorrent( data->tor );
        tr_torrentDeleteLocalData( data->tor, data->deleteFunc );
    }
    tr_torrentClearCompletenessCallback( data->tor );
    closeTorrent( data->tor );

    tr_free( data );

    tr_sessionUnlock( session );
}

void
tr_torrentRemove( tr_torrent   * tor,
                  bool           deleteFlag,
                  tr_fileFunc    deleteFunc )
{
    struct remove_data * data;

    assert( tr_isTorrent( tor ) );

    tr_session * session = tor->session;
    tr_sessionLock( session );

    tor->isDeleting = 1;
    tor->magnetVerify = false;

    data = tr_new0( struct remove_data, 1 );
    data->tor = tor;
    data->deleteFlag = deleteFlag;
    data->deleteFunc = deleteFunc;
    tr_runInEventThread( tor->session, removeTorrent, data );

    tr_sessionUnlock( session );

}

/**
***  Completeness
**/

static const char *
getCompletionString( int type )
{
    switch( type )
    {
        /* Translators: this is a minor point that's safe to skip over, but FYI:
           "Complete" and "Done" are specific, different terms in Transmission:
           "Complete" means we've downloaded every file in the torrent.
           "Done" means we're done downloading the files we wanted, but NOT all
           that exist */
        case TR_PARTIAL_SEED:
            return _( "Done" );

        case TR_SEED:
            return _( "Complete" );

        default:
            return _( "Incomplete" );
    }
}

static void
fireCompletenessChange( tr_torrent       * tor,
                        tr_completeness    status,
                        bool               wasRunning )
{
    assert( ( status == TR_LEECH )
         || ( status == TR_SEED )
         || ( status == TR_PARTIAL_SEED ) );

    if( tor->completeness_func )
        tor->completeness_func( tor, status, wasRunning,
                                tor->completeness_func_user_data );
}

void
tr_torrentSetCompletenessCallback( tr_torrent                    * tor,
                                   tr_torrent_completeness_func    func,
                                   void                          * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->completeness_func = func;
    tor->completeness_func_user_data = user_data;
}

void
tr_torrentClearCompletenessCallback( tr_torrent * torrent )
{
    tr_torrentSetCompletenessCallback( torrent, NULL, NULL );
}

void
tr_torrentSetRatioLimitHitCallback( tr_torrent                     * tor,
                                    tr_torrent_ratio_limit_hit_func  func,
                                    void                           * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->ratio_limit_hit_func = func;
    tor->ratio_limit_hit_func_user_data = user_data;
}

void
tr_torrentClearRatioLimitHitCallback( tr_torrent * torrent )
{
    tr_torrentSetRatioLimitHitCallback( torrent, NULL, NULL );
}

void
tr_torrentSetIdleLimitHitCallback( tr_torrent                    * tor,
                                   tr_torrent_idle_limit_hit_func  func,
                                   void                          * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->idle_limit_hit_func = func;
    tor->idle_limit_hit_func_user_data = user_data;
}

void
tr_torrentClearIdleLimitHitCallback( tr_torrent * torrent )
{
    tr_torrentSetIdleLimitHitCallback( torrent, NULL, NULL );
}

void
tr_torrentRecheckCompleteness( tr_torrent * tor )
{
    tr_completeness completeness;

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );

    completeness = tr_cpGetStatus( &tor->completion );

    if( completeness != tor->completeness )
    {
        const int recentChange = tor->downloadedCur != 0;
        const bool wasLeeching = !tr_torrentIsSeed( tor );
        const bool wasRunning = tor->isRunning;

        if( recentChange )
        {
            tr_torinf( tor, _( "State changed from \"%1$s\" to \"%2$s\"" ),
                      getCompletionString( tor->completeness ),
                      getCompletionString( completeness ) );
        }

        tor->completeness = completeness;
        tr_fdTorrentClose( tor->session, tor->uniqueId );

        fireCompletenessChange( tor, completeness, wasRunning );

        if( tr_torrentIsSeed( tor ) )
        {
            if( recentChange )
            {
                tr_announcerTorrentCompleted( tor );
                tor->doneDate = tor->anyDate = tr_time( );
            }

            if( wasLeeching && wasRunning )
            {
                /* clear interested flag on all peers */
                tr_peerMgrClearInterest( tor );

                /* if completeness was TR_LEECH then the seed limit check will have been skipped in bandwidthPulse */
                tr_torrentCheckSeedLimit( tor );
            }

            if( tor->currentDir == tor->incompleteDir )
                tr_torrentSetLocation( tor, tor->downloadDir, true, NULL, NULL );
				
            if( tor->session->reverifyTorrents )
            {
                if( tor->reverifyTorrent-- )
                    tr_torrentVerify( tor );
                else
                {
                    tr_torrentSetLocalError( tor, "%s", _( "Reverify count exceeded - pausing torrent -- resetting retries" ) );
                    tor->reverifyTorrent = tor->session->reverifyTorrents;  /* reset reverify tries */
                }
            }

            if( tr_sessionIsTorrentDoneScriptEnabled( tor->session ) )
                torrentCallScript( tor, tr_sessionGetTorrentDoneScript( tor->session ) );
        }

        tr_torrentSetDirty( tor );
    }

    tr_torrentUnlock( tor );
}

/***
****
***/

static void
tr_torrentFireMetadataCompleted( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tor->metadata_func )
        tor->metadata_func( tor, tor->metadata_func_user_data );
}

void
tr_torrentSetMetadataCallback( tr_torrent                * tor,
                               tr_torrent_metadata_func    func,
                               void                      * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->metadata_func = func;
    tor->metadata_func_user_data = user_data;
}


/**
***  File priorities
**/

void
tr_torrentInitFilePriority( tr_torrent *    tor,
                            tr_file_index_t fileIndex,
                            tr_priority_t   priority )
{
    tr_piece_index_t i;
    tr_file *        file;

    assert( tr_isTorrent( tor ) );
    assert( fileIndex < tor->info.fileCount );
    assert( tr_isPriority( priority ) );

    file = &tor->info.files[fileIndex];
    file->priority = priority;
    for( i = file->firstPiece; i <= file->lastPiece; ++i )
        tor->info.pieces[i].priority = calculatePiecePriority( tor, i, fileIndex );
}

void
tr_torrentSetFilePriorities( tr_torrent             * tor,
                             const tr_file_index_t  * files,
                             tr_file_index_t          fileCount,
                             tr_priority_t            priority )
{
    tr_file_index_t i;
    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    for( i = 0; i < fileCount; ++i )
        if( files[i] < tor->info.fileCount )
            tr_torrentInitFilePriority( tor, files[i], priority );
    tr_torrentSetDirty( tor );
    tr_peerMgrRebuildRequests( tor );

    tr_torrentUnlock( tor );
}

tr_priority_t*
tr_torrentGetFilePriorities( const tr_torrent * tor )
{
    tr_file_index_t i;
    tr_priority_t * p;

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );
    p = tr_new0( tr_priority_t, tor->info.fileCount );
    for( i = 0; i < tor->info.fileCount; ++i )
        p[i] = tor->info.files[i].priority;
    tr_torrentUnlock( tor );

    return p;
}

/**
****
***/

// static bool fileExists( const char * filename, time_t * optional_mtime );

bool
tr_torrentFindPieceTemp2( const tr_torrent  * tor,
                          tr_piece_index_t    pieceIndex,
                          const char       ** base,
                          char             ** subpath )
{
    const char * b = tor->pieceTempDir;
    char * s, * filename;
    bool exists = false;

    s = tr_strdup_printf( "%010u.dat", pieceIndex );

    filename = tr_buildPath( b, s, NULL );
    exists = fileExists( filename, NULL );
    tr_free( filename );

    if( base )
        *base = b;
    if( subpath )
        *subpath = s;
    else
        tr_free( s );

    return exists;
}

char *
tr_torrentFindPieceTemp( const tr_torrent * tor,
                         tr_piece_index_t   pieceIndex )
{
    const char * base;
    char * subpath, * filename = NULL;

    if( tr_torrentFindPieceTemp2( tor, pieceIndex, &base, &subpath ) )
    {
        filename = tr_buildPath( base, subpath, NULL );
        tr_free( subpath );
    }
    return filename;
}

/**
***  File DND
**/

static void
removePieceTemp( tr_torrent * tor, tr_piece_index_t piece )
{
    char * filename;
    tr_fdFileClose( tor->session, tor, piece, TR_FD_INDEX_PIECE );
    if( ( filename = tr_torrentFindPieceTemp( tor, piece ) ) )
    {
        deleteLocalFile( filename, remove );
        tr_free( filename );
    }
}

/**
 * @return TRUE if the file should use temporary piece files.
 */
static bool
usePieceTemp( tr_torrent * tor, tr_file_index_t i )
{
    int fd;

    if( !tor->info.files[i].dnd )
        return false;

    tr_cacheFlushFile( tor->session->cache, tor, i );

    return !tr_torrentFindFile2( tor, i, NULL, NULL, NULL );
}

/**
 * @note This function assumes @a tor is valid and already locked, and
 *       @a file_index is a valid file index for the torrent.
 * @note When @a file->dnd is TRUE and @a dnd is false, this function has
 *       the side effect of copying over data from temporary piece files
 *       to the destination file.
 * @see readOrWriteBytes()
 */
static void
setFileDND( tr_torrent * tor, tr_file_index_t file_index, int8_t dnd )
{
    tr_file * file = &tor->info.files[file_index];
    tr_file_index_t i;
    int nr, fdpt;
    int8_t svdnd;
    const tr_piece_index_t fpindex = file->firstPiece;
    const tr_piece_index_t lpindex = file->lastPiece;
    bool fpmovept, rwfpmovept, lpmovept, rwlpmovept, svusept, fpdnd, lpdnd, fpnopt, lpnopt;
    size_t fpoverlap, lpoverlap;
    uint32_t fpoffset, lpoffset;
    uint8_t * fpbuf, * lpbuf;

    if( file->dnd == dnd )
        return;

    /* Flags indicating whether we need to copy over existing data
     * from temporary piece files to the actual destination file. */
    fpmovept = file->usept && !dnd;
    lpmovept = fpmovept && fpindex != lpindex;

    /* Check cache and filesystem to make sure temporary piece files exist. */

    if( fpmovept )
    {
        tr_cacheFlushPiece( tor->session->cache, tor, fpindex );
        fpmovept = tr_torrentFindPieceTemp2( tor, fpindex, NULL, NULL );
    }
    if( lpmovept )
    {
        tr_cacheFlushPiece( tor->session->cache, tor, lpindex );
        lpmovept = tr_torrentFindPieceTemp2( tor, lpindex, NULL, NULL );
    }

    rwfpmovept = fpmovept;
    rwlpmovept = lpmovept;

    // eliminate compiler warnings - may be used uninitialized in this function
    fpoffset = 0;
    fpoverlap = 0;
    fpbuf = NULL;
//  lpoffset = 0;
    lpoverlap = 0;
    lpbuf = NULL;

    if( fpmovept )
    {
        char * filename = tr_torrentFindPieceTemp( tor, fpindex );
        fdpt = filename == NULL ? -1 : tr_open_file_for_scanning( filename );
        tr_free( filename );

        if( fdpt >= 0 )
        {

        fpoffset = file->offset - tr_pieceOffset( tor, fpindex, 0, 0 );
        fpoverlap = tr_torPieceCountBytes( tor, fpindex ) - fpoffset;
        if( fpoverlap > file->length )
            fpoverlap = file->length;
        fpbuf = tr_malloc0( fpoverlap );
        nr = tr_pread( fdpt, fpbuf, fpoverlap, fpoffset );
        if( nr < 0 )
            {
                tr_free( fpbuf );
                fpmovept = false;
            }
        }
        else fpmovept = false;
        
        tr_close_file( fdpt );
    }

    if( lpmovept && (rwfpmovept == fpmovept) )
    {

        char * filename = tr_torrentFindPieceTemp( tor, lpindex );
        fdpt = filename == NULL ? -1 : tr_open_file_for_scanning( filename );
        tr_free( filename );

        if( fdpt >= 0 )
        {

        lpoffset = 0;
        lpoverlap = file->offset + file->length - tr_pieceOffset( tor, lpindex, 0, 0 );
        lpbuf = tr_malloc0( lpoverlap );

        nr = tr_pread( fdpt, lpbuf, lpoverlap, lpoffset );
        if( nr < 0 )
            {
                tr_free( lpbuf );
                lpmovept = false;
                if ( fpmovept ) tr_free( fpbuf );
            }
        }
        else {
            lpmovept = false;
            if ( fpmovept ) tr_free( fpbuf );
            }

        tr_close_file( fdpt );
    }

    svdnd = file->dnd;
    svusept = file->usept;
	
    file->dnd = dnd;
    if( fpmovept || lpmovept )
        file->usept = false;
    else
        file->usept = usePieceTemp( tor, file_index );

    if( fpmovept && (rwlpmovept == lpmovept) )
    {
        nr = tr_ioWrite( tor, fpindex, fpoffset, fpoverlap, fpbuf );
        if ( nr != 0 ) {
             fpmovept = false;
             if ( lpmovept ) tr_free( lpbuf );
        }
        tr_free( fpbuf );
    }

    if( lpmovept && (rwfpmovept == fpmovept) )
    {
        nr = tr_ioWrite( tor, lpindex, lpoffset, lpoverlap, lpbuf );
        if ( nr != 0 ) lpmovept = false;
        tr_free( lpbuf );
    }

    /* Check conditions for setting piece DND and
     * removing temporary piece files:
     * - We can set the piece to DND if all files using
     *   that piece are DND.
     * - We can remove the temporary piece file if all
     *   files using it have 'usept' set to false.
     * - Do not delete temporary piece files if write failed. */

    // Do not change DND state if piece temp copy failed

    if ( (rwfpmovept == fpmovept) && (rwlpmovept == lpmovept) )
    {
    fpdnd = file->dnd;
    fpnopt = !file->usept;
    if( file_index > 0 )
    {
        for( i = file_index - 1; fpdnd || fpnopt; --i )
        {
            if( tor->info.files[i].lastPiece != fpindex )
                break;
            if( fpdnd )
                fpdnd = tor->info.files[i].dnd;
            if( fpnopt )
                fpnopt = !tor->info.files[i].usept;
            if( !i )
                break;
        }
    }

    lpdnd = file->dnd;
    lpnopt = !file->usept;
    for( i = file_index + 1; ( lpdnd || lpnopt ) && i < tor->info.fileCount; ++i )
    {
        if( tor->info.files[i].firstPiece != lpindex )
            break;
        if( lpdnd )
            lpdnd = tor->info.files[i].dnd;
        if( lpnopt )
            lpnopt = !tor->info.files[i].usept;
    }

    if( fpindex == lpindex )
    {
        tor->info.pieces[fpindex].dnd = fpdnd && lpdnd;
        if( fpnopt && lpnopt )
            removePieceTemp( tor, fpindex );
    }
    else
    {
        tr_piece_index_t p;
        tor->info.pieces[fpindex].dnd = fpdnd;
        tor->info.pieces[lpindex].dnd = lpdnd;
        for( p = fpindex + 1; p < lpindex; ++p )
            tor->info.pieces[p].dnd = dnd;
        if( fpnopt )
            removePieceTemp( tor, fpindex );
        if( lpnopt )
            removePieceTemp( tor, lpindex );
    }

    if( tr_cpFileIsComplete( &tor->completion, file_index ) )
        tr_torrentFileCompleted( tor, file_index );

    }
    else
    {
    file->dnd = svdnd;
    if( rwfpmovept || rwlpmovept )
        file->usept = svusept;
    else
        file->usept = svusept;
    }
}

void
tr_torrentInitFileDLs( tr_torrent             * tor,
                       const tr_file_index_t  * files,
                       tr_file_index_t          fileCount,
                       bool                     doDownload )
{
    tr_file_index_t i;

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );

    for( i=0; i<fileCount; ++i )
        if( files[i] < tor->info.fileCount )
            setFileDND( tor, files[i], !doDownload );

    tr_cpInvalidateDND( &tor->completion );

    tr_torrentUnlock( tor );
}

void
tr_torrentSetFileDLs( tr_torrent             * tor,
                      const tr_file_index_t  * files,
                      tr_file_index_t          fileCount,
                      bool                     doDownload )
{
    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    tr_torrentInitFileDLs( tor, files, fileCount, doDownload );
    tr_torrentSetDirty( tor );
    tr_peerMgrRebuildRequests( tor );

    tr_torrentUnlock( tor );
}

/***
****
***/

tr_priority_t
tr_torrentGetPriority( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->bandwidth.priority;
}

void
tr_torrentSetPriority( tr_torrent * tor, tr_priority_t priority )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isPriority( priority ) );

    if( tor->bandwidth.priority != priority )
    {
        tor->bandwidth.priority = priority;

        tr_torrentSetDirty( tor );
    }
}

/***
****
***/

void
tr_torrentSetPeerLimit( tr_torrent * tor,
                        uint16_t     maxConnectedPeers )
{
    assert( tr_isTorrent( tor ) );

    if ( tor->maxConnectedPeers != maxConnectedPeers )
    {
        tor->maxConnectedPeers = maxConnectedPeers;

        tr_torrentSetDirty( tor );
    }
}

uint16_t
tr_torrentGetPeerLimit( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->maxConnectedPeers;
}

/***
****
***/

void
tr_torrentGetBlockLocation( const tr_torrent * tor,
                            tr_block_index_t   block,
                            tr_piece_index_t * piece,
                            uint32_t         * offset,
                            uint32_t         * length )
{
    uint64_t pos = block;
    pos *= tor->blockSize;
    *piece = pos / tor->info.pieceSize;
    *offset = pos - ( *piece * tor->info.pieceSize );
    *length = tr_torBlockCountBytes( tor, block );
}


tr_block_index_t
_tr_block( const tr_torrent * tor,
           tr_piece_index_t   index,
           uint32_t           offset )
{
    tr_block_index_t ret;

    assert( tr_isTorrent( tor ) );

    ret = index;
    ret *= ( tor->info.pieceSize / tor->blockSize );
    ret += offset / tor->blockSize;
    return ret;
}

bool
tr_torrentReqIsValid( const tr_torrent * tor,
                      tr_piece_index_t   index,
                      uint32_t           offset,
                      uint32_t           length )
{
    int err = 0;

    assert( tr_isTorrent( tor ) );

    if( index >= tor->info.pieceCount )
        err = 1;
    else if( length < 1 )
        err = 2;
    else if( ( offset + length ) > tr_torPieceCountBytes( tor, index ) )
        err = 3;
    else if( length > MAX_BLOCK_SIZE )
        err = 4;
    else if( tr_pieceOffset( tor, index, offset, length ) > tor->info.totalSize )
        err = 5;

    if( err ) tr_tordbg( tor, "index %lu offset %lu length %lu err %d\n",
                              (unsigned long)index,
                              (unsigned long)offset,
                              (unsigned long)length,
                              err );

    return !err;
}

uint64_t
tr_pieceOffset( const tr_torrent * tor,
                tr_piece_index_t   index,
                uint32_t           offset,
                uint32_t           length )
{
    uint64_t ret;

    assert( tr_isTorrent( tor ) );

    ret = tor->info.pieceSize;
    ret *= index;
    ret += offset;
    ret += length;
    return ret;
}

void
tr_torGetFileBlockRange( const tr_torrent        * tor,
                         const tr_file_index_t     file,
                         tr_block_index_t        * first,
                         tr_block_index_t        * last )
{
    const tr_file * f = &tor->info.files[file];
    uint64_t offset = f->offset;
    *first = offset / tor->blockSize;
    if( !f->length )
        *last = *first;
    else {
        offset += f->length - 1;
        *last = offset / tor->blockSize;
    }
}

void
tr_torGetPieceBlockRange( const tr_torrent        * tor,
                          const tr_piece_index_t    piece,
                          tr_block_index_t        * first,
                          tr_block_index_t        * last )
{
    uint64_t offset = tor->info.pieceSize;
    offset *= piece;
    *first = offset / tor->blockSize;
    offset += ( tr_torPieceCountBytes( tor, piece ) - 1 );
    *last = offset / tor->blockSize;
}


/***
****
***/

void
tr_torrentSetPieceChecked( tr_torrent * tor, tr_piece_index_t pieceIndex )
{
    assert( tr_isTorrent( tor ) );
    assert( pieceIndex < tor->info.pieceCount );

    tor->info.pieces[pieceIndex].checked = 1;
}

void
tr_torrentSetChecked( tr_torrent * tor, int8_t when )
{
    tr_piece_index_t i, n;

    assert( tr_isTorrent( tor ) );

    for( i=0, n=tor->info.pieceCount; i!=n; ++i )
        tor->info.pieces[i].checked = when;
}

bool
tr_torrentCheckPiece( tr_torrent * tor, tr_piece_index_t pieceIndex )
{
    const bool pass = tr_ioTestPiece( tor, pieceIndex );

    tr_deeplog_tor( tor, "[LAZY] tr_torrentCheckPiece tested piece %zu, pass==%d", (size_t)pieceIndex, (int)pass );
    tr_torrentSetHasPiece( tor, pieceIndex, pass );
    if( pass )
        tr_torrentSetPieceChecked( tor, pieceIndex );
    tor->anyDate = tr_time( );
    tr_torrentSetDirty( tor );

    return pass;
}

time_t
tr_torrentGetFileMTime( const tr_torrent * tor, tr_file_index_t i )
{
    time_t mtime = 0;
    tr_fd_index_type it;
    it = TR_FD_INDEX_FILE;
    if( !tr_fdFileGetCachedMTime( tor->session, tor->uniqueId, i, it, &mtime ) )
        tr_torrentFindFile2( tor, i, NULL, NULL, &mtime );
    return mtime;
}

bool
tr_torrentPieceNeedsCheck( const tr_torrent * tor, tr_piece_index_t p )
{
    const tr_info * inf = tr_torrentInfo( tor );

    /* if we've never checked this piece, then it needs to be checked */
    if( !inf->pieces[p].checked )
        return true;

    return false;
}

/***
****
***/

static int
compareTrackerByTier( const void * va, const void * vb )
{
    const tr_tracker_info * a = va;
    const tr_tracker_info * b = vb;

    /* sort by tier */
    if( a->tier != b->tier )
        return a->tier - b->tier;

    /* get the effects of a stable sort by comparing the two elements' addresses */
    return a - b;
}

bool
tr_torrentSetAnnounceList( tr_torrent             * tor,
                           const tr_tracker_info  * trackers_in,
                           int                      trackerCount )
{
    int i;
    tr_benc metainfo;
    bool ok = true;
    tr_tracker_info * trackers;

    tr_torrentLock( tor );

    assert( tr_isTorrent( tor ) );

    /* ensure the trackers' tiers are in ascending order */
    trackers = tr_memdup( trackers_in, sizeof( tr_tracker_info ) * trackerCount );
    qsort( trackers, trackerCount, sizeof( tr_tracker_info ), compareTrackerByTier );

    /* look for bad URLs */
    for( i=0; ok && i<trackerCount; ++i )
        if( !tr_urlIsValidTracker( trackers[i].announce ) )
            ok = false;

    /* save to the .torrent file */
    if( ok && !tr_bencLoadFile( &metainfo, TR_FMT_BENC, tor->info.torrent ) )
    {
        bool hasInfo;
        tr_info tmpInfo;

        /* remove the old fields */
        tr_bencDictRemove( &metainfo, "announce" );
        tr_bencDictRemove( &metainfo, "announce-list" );

        /* add the new fields */
        if( trackerCount > 0 )
        {
            tr_bencDictAddStr( &metainfo, "announce", trackers[0].announce );
        }
        if( trackerCount > 1 )
        {
            int i;
            int prevTier = -1;
            tr_benc * tier = NULL;
            tr_benc * announceList = tr_bencDictAddList( &metainfo, "announce-list", 0 );

            for( i=0; i<trackerCount; ++i ) {
                if( prevTier != trackers[i].tier ) {
                    prevTier = trackers[i].tier;
                    tier = tr_bencListAddList( announceList, 0 );
                }
                tr_bencListAddStr( tier, trackers[i].announce );
            }
        }

        /* try to parse it back again, to make sure it's good */
        memset( &tmpInfo, 0, sizeof( tr_info ) );
        if( tr_metainfoParse( tor->session, &metainfo, &tmpInfo,
                              &hasInfo, &tor->infoDictLength ) )
        {
            /* it's good, so keep these new trackers and free the old ones */

            tr_info swap;
            swap.trackers = tor->info.trackers;
            swap.trackerCount = tor->info.trackerCount;
            tor->info.trackers = tmpInfo.trackers;
            tor->info.trackerCount = tmpInfo.trackerCount;
            tmpInfo.trackers = swap.trackers;
            tmpInfo.trackerCount = swap.trackerCount;

            tr_metainfoFree( &tmpInfo );
            tr_bencToFile( &metainfo, TR_FMT_BENC_TORRENT, tor->info.torrent );
        }

        /* cleanup */
        tr_bencFree( &metainfo );

        /* if we had a tracker-related error on this torrent,
         * and that tracker's been removed,
         * then clear the error */
        if(    ( tor->error == TR_STAT_TRACKER_WARNING )
            || ( tor->error == TR_STAT_TRACKER_ERROR ) )
        {
            bool clear = true;

            for( i=0; clear && i<trackerCount; ++i )
                if( !strcmp( trackers[i].announce, tor->errorTracker ) )
                    clear = false;

            if( clear )
                tr_torrentClearError( tor );
        }

        /* tell the announcer to reload this torrent's tracker list */
        tr_announcerResetTorrent( tor->session->announcer, tor );
    }

    tr_torrentUnlock( tor );

    tr_free( trackers );
    return ok;
}

/**
***
**/

void
tr_torrentSetAddedDate( tr_torrent * tor,
                        time_t       t )
{
    assert( tr_isTorrent( tor ) );

    tor->addedDate = t;
    tor->anyDate = MAX( tor->anyDate, tor->addedDate );
}

void
tr_torrentSetActivityDate( tr_torrent * tor, time_t t )
{
    assert( tr_isTorrent( tor ) );

    tor->activityDate = t;
    tor->anyDate = MAX( tor->anyDate, tor->activityDate );
}

void
tr_torrentSetDoneDate( tr_torrent * tor,
                       time_t       t )
{
    assert( tr_isTorrent( tor ) );

    tor->doneDate = t;
    tor->anyDate = MAX( tor->anyDate, tor->doneDate );
}

/**
***
**/

uint64_t
tr_torrentGetBytesLeftToAllocate( const tr_torrent * tor )
{
    tr_file_index_t i;
    uint64_t bytesLeft = 0;

    assert( tr_isTorrent( tor ) );

    for( i=0; i<tor->info.fileCount; ++i )
    {
        if( !tor->info.files[i].dnd )
        {
            struct stat sb;
            const uint64_t length = tor->info.files[i].length;
            char * path = tr_torrentFindFile( tor, i );

            bytesLeft += length;

            if( ( path != NULL ) && !stat( path, &sb )
                                 && S_ISREG( sb.st_mode )
                                 && ( (uint64_t)sb.st_size <= length ) )
                bytesLeft -= sb.st_size;

            tr_free( path );
        }
    }

    return bytesLeft;
}

/****
*****  Removing the torrent's local data
****/

static bool
isJunkFile( const char * base )
{
    int i;
    static const char * files[] = { ".DS_Store", "desktop.ini", "Thumbs.db" };
    static const int file_count = sizeof(files) / sizeof(files[0]);

    for( i=0; i<file_count; ++i )
        if( !strcmp( base, files[i] ) )
            return true;

#ifdef SYS_DARWIN
    /* check for resource forks. <http://support.apple.com/kb/TA20578> */
    if( !memcmp( base, "._", 2 ) )
        return true;
#endif

    return false;
}

static void
removeEmptyFoldersAndJunkFiles( const char * folder )
{
    DIR * odir;
    if(( odir = opendir( folder ))) {
        struct dirent * d;
        while(( d = readdir( odir ))) {
            if( strcmp( d->d_name, "." ) && strcmp( d->d_name, ".." ) ) {
                struct stat sb;
                char * filename = tr_buildPath( folder, d->d_name, NULL );
                if( !stat( filename, &sb ) && S_ISDIR( sb.st_mode ) )
                    removeEmptyFoldersAndJunkFiles( filename );
                else if( isJunkFile( d->d_name ) )
                    remove( filename );
                tr_free( filename );
            }
        }
        remove( folder );
        closedir( odir );
    }
}

// static bool fileExists( const char * filename, time_t * optional_mtime );

/**
 * This convoluted code does something (seemingly) simple:
 * remove the torrent's local files.
 *
 * Fun complications:
 * 1. Try to preserve the directory hierarchy in the recycle bin.
 * 2. If there are nontorrent files, don't delete them...
 * 3. ...unless the other files are "junk", such as .DS_Store
 */
static void
deleteLocalData( tr_torrent * tor, tr_fileFunc func )
{
    int i, n;
    tr_file_index_t f;
    char * base;
    DIR * odir;
    char * tmpdir = NULL;
    tr_ptrArray files = TR_PTR_ARRAY_INIT;
    tr_ptrArray folders = TR_PTR_ARRAY_INIT;
    const void * const vstrcmp = strcmp;
    const char * const top = tor->currentDir;

    /* if it's a magnet link, there's nothing to move... */
    if( !tr_torrentHasMetadata( tor ) )
        return;

    if( path_is_bad( tor->currentDir ) )
    {
        tr_torerr( tor, " bad path error deleting  \"%s\" ", tor->currentDir );
        return;
    }

    /***
    ****  Move the local data to a new tmpdir
    ***/

    base = tr_strdup_printf( "%s__XXXXXX", tr_torrentName( tor ) );
    tmpdir = tr_buildPath( top, base, NULL );
    mkdtemp( tmpdir );
    tr_free( base );

    for( f=0; f<tor->info.fileCount; ++f )
    {
        char * filename = tr_buildPath( top, tor->info.files[f].name, NULL );
        if( !fileExists( filename, NULL ) ) {
                char * partial = tr_torrentBuildPartial( tor, f );
                tr_free( filename );
                filename = tr_buildPath( top, partial, NULL );
                tr_free( partial );
                if( !fileExists( filename, NULL ) ) {
                        tr_free( filename );
                        filename = NULL;
                }
        }

        if( filename != NULL )
        {
            char * target = tr_buildPath( tmpdir, tor->info.files[f].name, NULL );
            char * target_dir = tr_dirname( target );
            tr_mkdirp( target_dir, 0777 );
            rename( filename, target );
            tr_ptrArrayAppend( &files, target );
            tr_free( target_dir );
            tr_free( filename );
        }
    }

    /***
    ****  Remove tmpdir.
    ****
    ****  Try deleting the top-level files & folders to preserve
    ****  the directory hierarchy in the recycle bin.
    ****  If case that fails -- for example, rmdir() doesn't
    ****  delete nonempty folders -- go from the bottom up too.
    ***/

    /* try deleting the local data's top-level files & folders */
    if(( odir = opendir( tmpdir )))
    {
        struct dirent * d;
        while(( d = readdir( odir )))
        {
            if( strcmp( d->d_name, "." ) && strcmp( d->d_name, ".." ) )
            {
                char * file = tr_buildPath( tmpdir, d->d_name, NULL );
                func( file );
                tr_free( file );
            }
        }
        closedir( odir );
    }

    /* go from the bottom up */
    for( i=0, n=tr_ptrArraySize(&files); i<n; ++i )
    {
        char * walk = tr_strdup( tr_ptrArrayNth( &files, i ) );
        while( fileExists( walk, NULL ) && !tr_is_same_file( tmpdir, walk ) )
        {
            char * tmp = tr_dirname( walk );
            func( walk );
            tr_free( walk );
            walk = tmp;
        }
        tr_free( walk );
    }

    /***
    ****  The local data has been removed.
    ****  What's left in top are empty folders, junk, and user-generated files.
    ****  Remove the first two categories and leave the third.
    ***/

    /* build a list of 'top's child directories that belong to this torrent */
    for( f=0; f<tor->info.fileCount; ++f )
    {
        char * dir;
        char * filename;

        /* get the directory that this file goes in... */
        filename = tr_buildPath( top, tor->info.files[f].name, NULL );
        dir = tr_dirname( filename );
        tr_free( filename );
        if( !tr_is_same_file( top, dir ) && strcmp( top, dir ) ) {
            for( ;; ) {
                char * parent = tr_dirname( dir );
                if( tr_is_same_file( top, parent ) || !strcmp( top, parent ) ) {
                    /* break is guaranteed now */
                    tr_free( parent ); /* free parent now since dir won't point to it on break */
                    if( tr_ptrArrayFindSorted( &folders, dir, vstrcmp ) == NULL ) {
                        tr_ptrArrayInsertSorted( &folders, tr_strdup( dir ), vstrcmp );
                    }
                    break;
                }
                tr_free( dir ); /* free dir and maybe equivalent parent pointed to by prior */
                dir = parent; /* point dir at current parent  */
            }
        }
        tr_free( dir ); /* free dir and maybe equivalent parent pointed to by prior */
    }
    for( i=0, n=tr_ptrArraySize(&folders); i<n; ++i )
        removeEmptyFoldersAndJunkFiles( tr_ptrArrayNth( &folders, i ) );

    /* cleanup */
    rmdir( tmpdir );
    tr_free( tmpdir );
    tr_ptrArrayDestruct( &folders, tr_free );
    tr_ptrArrayDestruct( &files, tr_free );
}

static void
tr_torrentDeleteLocalData( tr_torrent * tor, tr_fileFunc func )
{
    assert( tr_isTorrent( tor ) );

    if( func == NULL )
        func = remove;

    /* close all the files because we're about to delete them */
    tr_cacheFlushTorrent( tor->session->cache, tor );
    tr_fdTorrentClose( tor->session, tor->uniqueId );

    deleteLocalData( tor, func );
}

/***
****
***/

struct LocationData
{
    bool move_from_old_location;
    volatile int * setme_state;
    volatile double * setme_progress;
    char * location;
    tr_torrent * tor;
};

static void
setLocation( void * vdata )
{
    bool err = false;
    struct LocationData * data = vdata;
    tr_torrent * tor = data->tor;
    const bool do_move = data->move_from_old_location;
    const char * location = data->location;
    double bytesHandled = 0;

    assert( tr_isTorrent( tor ) );

    tr_dbg( "Moving \"%s\" location from currentDir \"%s\" to \"%s\"",
            tr_torrentName(tor), tor->currentDir, location );

    tr_mkdirp( location, 0777 );

    if( !tr_is_same_file( location, tor->currentDir ) )
    {
        tr_file_index_t i;

        /* bad idea to move files while they're being verified... */
        tr_verifyRemove( tor );

        /* try to move the files.
         * FIXME: there are still all kinds of nasty cases, like what
         * if the target directory runs out of space halfway through... */
        for( i=0; !err && i<tor->info.fileCount; ++i )
        {
            const tr_file * f = &tor->info.files[i];
            const char * oldbase;
            char * sub;
            if( tr_torrentFindFile2( tor, i, &oldbase, &sub, NULL ) )
            {
                char * oldpath = tr_buildPath( oldbase, sub, NULL );
                char * newpath = tr_buildPath( location, sub, NULL );

                tr_dbg( "Found file #%d: %s", (int)i, oldpath );

                if( do_move && !tr_is_same_file( oldpath, newpath ) )
                {
                    bool renamed = false;
                    errno = 0;
                    tr_torinf( tor, "moving \"%s\" to \"%s\"", oldpath, newpath );
                    if( tr_moveFile( oldpath, newpath, &renamed ) )
                    {
                        err = true;
                        tr_torerr( tor, "error moving \"%s\" to \"%s\": %s",
                                        oldpath, newpath, tr_strerror( errno ) );
                    }
                }

                tr_free( newpath );
                tr_free( oldpath );
                tr_free( sub );
            }

            if( data->setme_progress )
            {
                bytesHandled += f->length;
                *data->setme_progress = bytesHandled / tor->info.totalSize;
            }
        }

        if( !err && do_move )
            /* blow away the leftover subdirectories in the old location */
            tr_torrentDeleteLocalData( tor, remove );
    }

    if( !err )
    {
        /* set the new location and reverify */
        tr_torrentSetDownloadDir( tor, location );

        if( do_move )
        {
            tr_free( tor->incompleteDir );
            tor->incompleteDir = NULL;
            tor->currentDir = tor->downloadDir;
        }
    }

    if( data->setme_state )
        *data->setme_state = err ? TR_LOC_ERROR : TR_LOC_DONE;

    /* cleanup */
    tr_free( data->location );
    tr_free( data );
}

void
tr_torrentSetLocation( tr_torrent       * tor,
                       const char       * location,
                       bool               move_from_old_location,
                       volatile double  * setme_progress,
                       volatile int     * setme_state )
{
    if( path_is_bad( tor->currentDir ) ) tr_torerr( tor, "bad path error set-location source \"%s\" ", tor->currentDir );

    if( ( path_is_bad( tor->currentDir ) ) && ( !path_is_bad( location ) ) )
    {
        tr_torerr( tor, "bad path set-location source IGNORING \"%s\" ", tor->currentDir );
        move_from_old_location = false;
    }

    if( path_is_bad( location ) )
    {
        tr_torerr( tor, "bad path error set-location destination \"%s\" ", location );
        return;
    }

    struct LocationData * data;

    assert( tr_isTorrent( tor ) );

    if( setme_state )
        *setme_state = TR_LOC_MOVING;
    if( setme_progress )
        *setme_progress = 0;

    /* run this in the libtransmission thread */
    data = tr_new( struct LocationData, 1 );
    data->tor = tor;
    data->location = tr_strdup( location );
    data->move_from_old_location = move_from_old_location;
    data->setme_state = setme_state;
    data->setme_progress = setme_progress;
    tr_runInEventThread( tor->session, setLocation, data );
}

/***
****
***/

void
tr_torrentFileCompleted( tr_torrent * tor, tr_file_index_t fileNum )
{
    char * sub;
    const char * base;
    const tr_info * inf = &tor->info;
    const tr_file * f = &inf->files[fileNum];
    tr_piece * p;
    const tr_piece * pend;

    /* close the file so that we can reopen in read-only mode as needed */
    tr_fdFileClose( tor->session, tor, fileNum, TR_FD_INDEX_FILE );

    /* now that the file is complete and closed, we can start watching its
     * mtime timestamp for changes to know if we need to reverify pieces */
    for( p=&inf->pieces[f->firstPiece], pend=&inf->pieces[f->lastPiece]+1; p!=pend; ++p )
        p->checked = 1;

    /* if the torrent's current filename isn't the same as the one in the
     * metadata -- for example, if it had the ".part" suffix appended to
     * it until now -- then rename it to match the one in the metadata */
    if( tr_torrentFindFile2( tor, fileNum, &base, &sub, NULL ) )
    {
        if( strcmp( sub, f->name ) )
        {
            char * oldpath = tr_buildPath( base, sub, NULL );
            char * newpath = tr_buildPath( base, f->name, NULL );

            if( rename( oldpath, newpath ) )
                tr_torerr( tor, "Error moving \"%s\" to \"%s\": %s", oldpath, newpath, tr_strerror( errno ) );

            tr_free( newpath );
            tr_free( oldpath );
        }

        tr_free( sub );
    }
}

/***
****
***/

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

bool
tr_torrentFindFile2( const tr_torrent * tor, tr_file_index_t fileNum,
                     const char ** base, char ** subpath, time_t * mtime )
{
    char * part = NULL;
    const tr_file * file;
    const char * b = NULL;
    const char * s = NULL;

    assert( tr_isTorrent( tor ) );
    assert( fileNum < tor->info.fileCount );

    file = &tor->info.files[fileNum];

    if( ( b == NULL ) && ( tor->downloadDir != NULL ) ) {
        char * filename = tr_buildPath( tor->downloadDir, file->name, NULL );
        if( fileExists( filename, mtime ) ) {
            b = tor->downloadDir;
            s = file->name;
        }
        tr_free( filename );
    }

    if( ( b == NULL ) && ( tor->incompleteDir != NULL ) ) {
        char * filename = tr_buildPath( tor->incompleteDir, file->name, NULL );
        if( fileExists( filename, mtime ) ) {
            b = tor->incompleteDir;
            s = file->name;
        }
        tr_free( filename );
    }

    if( b == NULL )
        part = tr_torrentBuildPartial( tor, fileNum );

    if( ( b == NULL ) && ( tor->incompleteDir != NULL ) ) {
        char * filename = tr_buildPath( tor->incompleteDir, part, NULL );
        if( fileExists( filename, mtime ) ) {
            b = tor->incompleteDir;
            s = part;
        }
        tr_free( filename );
    }

    if( ( b == NULL ) && ( tor->downloadDir != NULL ) ) {
        char * filename = tr_buildPath( tor->downloadDir, part, NULL );
        if( fileExists( filename, mtime ) ) {
            b = tor->downloadDir;
            s = part;
        }
        tr_free( filename );
    }

    if( base != NULL )
        *base = b;
    if( subpath != NULL )
        *subpath = tr_strdup( s );

    tr_free( part );
    return b != NULL;
}

char*
tr_torrentFindFile( const tr_torrent * tor, tr_file_index_t fileNum )
{
    char * subpath;
    char * ret = NULL;
    const char * base;

    if( tr_torrentFindFile2( tor, fileNum, &base, &subpath, NULL ) )
    {
        ret = tr_buildPath( base, subpath, NULL );
        tr_free( subpath );
    }

    return ret;
}

/* Decide whether we should be looking for files in downloadDir or incompleteDir. */
static void
refreshCurrentDir( tr_torrent * tor )
{
    const char * dir = NULL;

    if( tor->incompleteDir == NULL )
        dir = tor->downloadDir;
    else if( !tr_torrentHasMetadata( tor ) ) /* no files to find */
        dir = tor->incompleteDir;
    else if( !tr_torrentFindFile2( tor, 0, &dir, NULL, NULL ) )
        dir = tor->incompleteDir;

// fail assert when new torrent added to download to NULL directory and no incomplete specified directory either
// SRS 10-16-2014 so  path_is_bad now handles this without having to assert
//    assert( dir != NULL );
//    assert( ( dir == tor->downloadDir ) || ( dir == tor->incompleteDir ) );
    tor->currentDir = dir;
}

char*
tr_torrentBuildPartial( const tr_torrent * tor, tr_file_index_t fileNum )
{
    return tr_strdup_printf( "%s.part", tor->info.files[fileNum].name );
}

/***
****
***/

static int
compareTorrentByQueuePosition( const void * va, const void * vb )
{
    const tr_torrent * a = * (const tr_torrent **) va;
    const tr_torrent * b = * (const tr_torrent **) vb;

    return a->queuePosition - b->queuePosition;
}

#ifndef NDEBUG
static bool
queueIsSequenced( tr_session * session )
{
    int i ;
    int n ;
    bool is_sequenced = true;
    tr_torrent * tor;
    tr_torrent ** tmp = tr_new( tr_torrent *, session->torrentCount );

    /* get all the torrents */
    n = 0;
    tor = NULL;
    while(( tor = tr_torrentNext( session, tor )))
        tmp[n++] = tor;

    /* sort them by position */
    qsort( tmp, n, sizeof( tr_torrent * ), compareTorrentByQueuePosition );

#if 0
    fprintf( stderr, "%s", "queue: " );
    for( i=0; i<n; ++i )
        fprintf( stderr, "%d ", tmp[i]->queuePosition );
    fputc( '\n', stderr );
#endif

    /* test them */
    for( i=0; is_sequenced && i<n; ++i )
        if( tmp[i]->queuePosition != i )
            is_sequenced = false;

    tr_free( tmp );
    return is_sequenced;
}
#endif

int
tr_torrentGetQueuePosition( const tr_torrent * tor )
{
    return tor->queuePosition;
}

void
tr_torrentSetQueuePosition( tr_torrent * tor, int pos )
{
    int back = -1;
    tr_torrent * walk;
    const int old_pos = tor->queuePosition;
    const time_t now = tr_time( );

    if( pos < 0 )
        pos = 0;

    tor->queuePosition = -1;

    walk = NULL;
    while(( walk = tr_torrentNext( tor->session, walk )))
    {
        if( old_pos < pos ) {
            if( ( old_pos <= walk->queuePosition ) && ( walk->queuePosition <= pos ) ) {
                walk->queuePosition--;
                walk->anyDate = now;
            }
        }

        if( old_pos > pos ) {
            if( ( pos <= walk->queuePosition ) && ( walk->queuePosition < old_pos ) ) {
                walk->queuePosition++;
                walk->anyDate = now;
            }
        }

        if( back < walk->queuePosition )
            back = walk->queuePosition;
    }

    tor->queuePosition = MIN( pos, (back+1) );
    tor->anyDate = now;

    assert( queueIsSequenced( tor->session ) );
}

void
tr_torrentsQueueMoveTop( tr_torrent ** torrents_in, int n )
{
    int i;
    tr_torrent ** torrents = tr_memdup( torrents_in, sizeof( tr_torrent * ) * n );
    qsort( torrents, n, sizeof( tr_torrent * ), compareTorrentByQueuePosition );
    for( i=n-1; i>=0; --i )
        tr_torrentSetQueuePosition( torrents[i], 0 );
    tr_free( torrents );
}

void
tr_torrentsQueueMoveUp( tr_torrent ** torrents_in, int n )
{
    int i;
    tr_torrent ** torrents = tr_memdup( torrents_in, sizeof( tr_torrent * ) * n );
    qsort( torrents, n, sizeof( tr_torrent * ), compareTorrentByQueuePosition );
    for( i=0; i<n; ++i )
        tr_torrentSetQueuePosition( torrents[i], torrents[i]->queuePosition - 1 );
    tr_free( torrents );
}

void
tr_torrentsQueueMoveDown( tr_torrent ** torrents_in, int n )
{
    int i;
    tr_torrent ** torrents = tr_memdup( torrents_in, sizeof( tr_torrent * ) * n );
    qsort( torrents, n, sizeof( tr_torrent * ), compareTorrentByQueuePosition );
    for( i=n-1; i>=0; --i )
        tr_torrentSetQueuePosition( torrents[i], torrents[i]->queuePosition + 1 );
    tr_free( torrents );
}

void
tr_torrentsQueueMoveBottom( tr_torrent ** torrents_in, int n )
{
    int i;
    tr_torrent ** torrents = tr_memdup( torrents_in, sizeof( tr_torrent * ) * n );
    qsort( torrents, n, sizeof( tr_torrent * ), compareTorrentByQueuePosition );
    for( i=0; i<n; ++i )
        tr_torrentSetQueuePosition( torrents[i], INT_MAX );
    tr_free( torrents );
}

static void
torrentSetQueued( tr_torrent * tor, bool queued )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isBool( queued ) );

    if( tr_torrentIsQueued( tor ) != queued )
    {
        tor->isQueued = queued;
        tor->anyDate = tr_time( );
        tr_torrentSetDirty( tor );
    }
}

void
tr_torrentSetQueueStartCallback( tr_torrent * torrent, void (*callback)( tr_torrent *, void * ), void * user_data )
{
    torrent->queue_started_callback = callback;
    torrent->queue_started_user_data = user_data;
}


