/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: resume.c 12921 2011-09-26 22:50:42Z jordan $
 */

#include <unistd.h> /* unlink */

#include <string.h>

#include "transmission.h"
#include "bencode.h"
#include "completion.h"
#include "metainfo.h" /* tr_metainfoGetBasename() */
#include "peer-mgr.h" /* pex */
#include "platform.h" /* tr_getResumeDir() */
#include "resume.h"
#include "session.h"
#include "torrent.h"
#include "utils.h" /* tr_buildPath */

#define KEY_ACTIVITY_DATE       "activity-date"
#define KEY_ADDED_DATE          "added-date"
#define KEY_CHEATRATIO          "cheat-ratio"
#define KEY_CORRUPT             "corrupt"
#define KEY_DONE_DATE           "done-date"
#define KEY_DOWNLOAD_DIR        "destination"
#define KEY_GROUP               "group-destination"
#define KEY_PRIVATEENABLED      "private-enabled"
#define KEY_PIECE_TEMP_DIR      "piece-temp-dir"
#define KEY_DND                 "dnd"
#define KEY_DOWNLOADED          "downloaded"
#define KEY_INCOMPLETE_DIR      "incomplete-dir"
#define KEY_MAX_PEERS           "max-peers"
#define KEY_PAUSED              "paused"
#define KEY_PEERS               "peers2"
#define KEY_PEERS6              "peers2-6"
#define KEY_FILE_PRIORITIES     "priority"
#define KEY_BANDWIDTH_PRIORITY  "bandwidth-priority"
#define KEY_PROGRESS            "progress"
#define KEY_SPEEDLIMIT_OLD      "speed-limit"
#define KEY_SPEEDLIMIT_UP       "speed-limit-up"
#define KEY_SPEEDLIMIT_DOWN     "speed-limit-down"
#define KEY_RATIOLIMIT          "ratio-limit"
#define KEY_IDLELIMIT           "idle-limit"
#define KEY_UPLOADED            "uploaded"
#define KEY_CHEATMODE           "cheat-mode"
#define KEY_STREAMINGMODE       "streaming-mode"

#define KEY_SPEED_KiBps            "speed"
#define KEY_SPEED_Bps              "speed-Bps"
#define KEY_USE_GLOBAL_SPEED_LIMIT "use-global-speed-limit"
#define KEY_USE_SPEED_LIMIT        "use-speed-limit"
#define KEY_TIME_SEEDING           "seeding-time-seconds"
#define KEY_TIME_DOWNLOADING       "downloading-time-seconds"
#define KEY_SPEEDLIMIT_DOWN_SPEED  "down-speed"
#define KEY_SPEEDLIMIT_DOWN_MODE   "down-mode"
#define KEY_SPEEDLIMIT_UP_SPEED    "up-speed"
#define KEY_SPEEDLIMIT_UP_MODE     "up-mode"
#define KEY_RATIOLIMIT_RATIO       "ratio-limit"
#define KEY_RATIOLIMIT_MODE        "ratio-mode"
#define KEY_IDLELIMIT_MINS         "idle-limit"
#define KEY_IDLELIMIT_MODE         "idle-mode"

#define KEY_PROGRESS_CHECKTIME "time-checked"
#define KEY_PROGRESS_MTIMES    "mtimes"
#define KEY_PROGRESS_BITFIELD  "bitfield"
#define KEY_PROGRESS_BLOCKS    "blocks"
#define KEY_PROGRESS_HAVE      "have"

enum
{
    MAX_REMEMBERED_PEERS = 200
};

static char*
getResumeFilename(tr_torrent const* tor, enum tr_metainfo_basename_format format)
{
    char * base = tr_metainfoGetBasename(tr_torrentInfo(tor), format, tor->session);
    char * filename = tr_strdup_printf( "%s" TR_PATH_DELIMITER_STR "%s.resume",
                                        tr_getResumeDir( tor->session ), base );
    tr_free( base );
    return filename;
}

/***
****
***/

static void
savePeers( tr_benc * dict, const tr_torrent * tor )
{
    int count;
    tr_pex * pex;

    count = tr_peerMgrGetPeers( (tr_torrent*) tor, &pex, TR_AF_INET, TR_PEERS_INTERESTING, MAX_REMEMBERED_PEERS );
    if( count > 0 )
        tr_bencDictAddRaw( dict, KEY_PEERS, pex, sizeof( tr_pex ) * count );
    tr_free( pex );

    count = tr_peerMgrGetPeers( (tr_torrent*) tor, &pex, TR_AF_INET6, TR_PEERS_INTERESTING, MAX_REMEMBERED_PEERS );
    if( count > 0 )
        tr_bencDictAddRaw( dict, KEY_PEERS6, pex, sizeof( tr_pex ) * count );

    tr_free( pex );
}

static int
addPeers( tr_torrent * tor, const uint8_t * buf, int buflen )
{
    int i;
    int numAdded = 0;
    const int count = buflen / sizeof( tr_pex );

    for( i=0; i<count && numAdded<MAX_REMEMBERED_PEERS; ++i )
    {
        tr_pex pex;
        memcpy( &pex, buf + ( i * sizeof( tr_pex ) ), sizeof( tr_pex ) );
        if( tr_isPex( &pex ) )
        {
            tr_peerMgrAddPex( tor, TR_PEER_FROM_RESUME, &pex, -1 );
            ++numAdded;
        }
    }

    return numAdded;
}


static uint64_t
loadPeers( tr_benc * dict, tr_torrent * tor )
{
    uint64_t        ret = 0;
    const uint8_t * str;
    size_t          len;

    if( tr_bencDictFindRaw( dict, KEY_PEERS, &str, &len ) )
    {
        const int numAdded = addPeers( tor, str, len );
        tr_tordbg( tor, "Loaded %d IPv4 peers from resume file", numAdded );
        ret = TR_FR_PEERS;
    }

    if( tr_bencDictFindRaw( dict, KEY_PEERS6, &str, &len ) )
    {
        const int numAdded = addPeers( tor, str, len );
        tr_tordbg( tor, "Loaded %d IPv6 peers from resume file", numAdded );
        ret = TR_FR_PEERS;
    }

    return ret;
}

/***
****
***/

static void
saveDND( tr_benc * dict, const tr_torrent * tor )
{
    tr_benc * list;
    tr_file_index_t i;
    const tr_info * const inf = tr_torrentInfo( tor );
    const tr_file_index_t n = inf->fileCount;

    list = tr_bencDictAddList( dict, KEY_DND, n );
    for( i=0; i<n; ++i )
        tr_bencListAddInt( list, inf->files[i].dnd ? 1 : 0 );
}

static uint64_t
loadDND( tr_benc * dict, tr_torrent * tor )
{
    uint64_t ret = 0;
    tr_benc * list = NULL;
    const tr_file_index_t n = tor->info.fileCount;

    if( tr_bencDictFindList( dict, KEY_DND, &list )
      && ( tr_bencListSize( list ) == n ) )
    {
        int64_t           tmp;
        tr_file_index_t * dl = tr_new( tr_file_index_t, n );
        tr_file_index_t * dnd = tr_new( tr_file_index_t, n );
        tr_file_index_t   i, dlCount = 0, dndCount = 0;

        for( i=0; i<n; ++i )
        {
            if( tr_bencGetInt( tr_bencListChild( list, i ), &tmp ) && tmp )
                dnd[dndCount++] = i;
            else
                dl[dlCount++] = i;
        }

        if( dndCount )
        {
            tr_torrentInitFileDLs ( tor, dnd, dndCount, false );
            tr_tordbg( tor, "Resume file found %d files listed as dnd",
                       dndCount );
        }
        if( dlCount )
        {
            tr_torrentInitFileDLs ( tor, dl, dlCount, true );
            tr_tordbg( tor,
                       "Resume file found %d files marked for download",
                       dlCount );
        }

        tr_free( dnd );
        tr_free( dl );
        ret = TR_FR_DND;
    }
    else
    {
        tr_tordbg(
            tor,
            "Couldn't load DND flags. DND list (%p) has %zu children; torrent has %d files",
            list, tr_bencListSize( list ), (int)n );
    }

    return ret;
}

/***
****
***/

static void
saveFilePriorities( tr_benc * dict, const tr_torrent * tor )
{
    tr_benc * list;
    tr_file_index_t i;
    const tr_info * const inf = tr_torrentInfo( tor );
    const tr_file_index_t n = inf->fileCount;

    list = tr_bencDictAddList( dict, KEY_FILE_PRIORITIES, n );
    for( i = 0; i < n; ++i )
        tr_bencListAddInt( list, inf->files[i].priority );
}

static uint64_t
loadFilePriorities( tr_benc * dict, tr_torrent * tor )
{
    tr_benc * list;
    uint64_t ret = 0;
    const tr_file_index_t n = tor->info.fileCount;

    if( tr_bencDictFindList( dict, KEY_FILE_PRIORITIES, &list )
      && ( tr_bencListSize( list ) == n ) )
    {
        int64_t priority;
        tr_file_index_t i;
        for( i = 0; i < n; ++i )
            if( tr_bencGetInt( tr_bencListChild( list, i ), &priority ) )
                tr_torrentInitFilePriority( tor, i, priority );
        ret = TR_FR_FILE_PRIORITIES;
    }

    return ret;
}

/***
****
***/

static void
saveSingleSpeedLimit( tr_benc * d, const tr_torrent * tor, tr_direction dir )
{
    tr_bencDictReserve( d, 3 );
    tr_bencDictAddInt( d, KEY_SPEED_Bps, tr_torrentGetSpeedLimit_Bps( tor, dir ) );
    tr_bencDictAddBool( d, KEY_USE_GLOBAL_SPEED_LIMIT, tr_torrentUsesSessionLimits( tor ) );
    tr_bencDictAddBool( d, KEY_USE_SPEED_LIMIT, tr_torrentUsesSpeedLimit( tor, dir ) );
}

static void
saveSpeedLimits( tr_benc * dict, const tr_torrent * tor )
{
    saveSingleSpeedLimit( tr_bencDictAddDict( dict, KEY_SPEEDLIMIT_DOWN, 0 ), tor, TR_DOWN );
    saveSingleSpeedLimit( tr_bencDictAddDict( dict, KEY_SPEEDLIMIT_UP, 0 ), tor, TR_UP );
}

static void
saveRatioLimits( tr_benc * dict, const tr_torrent * tor )
{
    tr_benc * d = tr_bencDictAddDict( dict, KEY_RATIOLIMIT, 2 );
    tr_bencDictAddReal( d, KEY_RATIOLIMIT_RATIO, tr_torrentGetRatioLimit( tor ) );
    tr_bencDictAddInt( d, KEY_RATIOLIMIT_MODE, tr_torrentGetRatioMode( tor ) );
}

static void
saveIdleLimits( tr_benc * dict, const tr_torrent * tor )
{
    tr_benc * d = tr_bencDictAddDict( dict, KEY_IDLELIMIT, 2 );
    tr_bencDictAddInt( d, KEY_IDLELIMIT_MINS, tr_torrentGetIdleLimit( tor ) );
    tr_bencDictAddInt( d, KEY_IDLELIMIT_MODE, tr_torrentGetIdleMode( tor ) );
}

static void
saveCheatMode( tr_benc * dict, const tr_torrent * tor )
{
    tr_bencDictReserve( dict, 1 );
    tr_bencDictAddInt( dict, KEY_CHEATMODE, tr_torrentGetCheatMode( tor ) );
}

static void
saveStreamingMode( tr_benc * dict, const tr_torrent * tor )
{
    tr_bencDictReserve( dict, 1 );
    tr_bencDictAddInt( dict, KEY_STREAMINGMODE, tr_torrentGetStreamingMode( tor ) );
}

static void
loadSingleSpeedLimit( tr_benc * d, tr_direction dir, tr_torrent * tor )
{
    int64_t i;
    bool boolVal;

    if( tr_bencDictFindInt( d, KEY_SPEED_Bps, &i ) )
        tr_torrentSetSpeedLimit_Bps( tor, dir, i );
    else if( tr_bencDictFindInt( d, KEY_SPEED_KiBps, &i ) )
        tr_torrentSetSpeedLimit_Bps( tor, dir, i*1024 );

    if( tr_bencDictFindBool( d, KEY_USE_SPEED_LIMIT, &boolVal ) )
        tr_torrentUseSpeedLimit( tor, dir, boolVal );

    if( tr_bencDictFindBool( d, KEY_USE_GLOBAL_SPEED_LIMIT, &boolVal ) )
        tr_torrentUseSessionLimits( tor, boolVal );
}

enum old_speed_modes
{
    TR_SPEEDLIMIT_GLOBAL,   /* only follow the overall speed limit */
    TR_SPEEDLIMIT_SINGLE    /* only follow the per-torrent limit */
};

static uint64_t
loadSpeedLimits( tr_benc * dict, tr_torrent * tor )
{
    tr_benc * d;
    uint64_t ret = 0;


    if( tr_bencDictFindDict( dict, KEY_SPEEDLIMIT_UP, &d ) )
    {
        loadSingleSpeedLimit( d, TR_UP, tor );
        ret = TR_FR_SPEEDLIMIT;
    }
    if( tr_bencDictFindDict( dict, KEY_SPEEDLIMIT_DOWN, &d ) )
    {
        loadSingleSpeedLimit( d, TR_DOWN, tor );
        ret = TR_FR_SPEEDLIMIT;
    }

    /* older speedlimit structure */
    if( !ret && tr_bencDictFindDict( dict, KEY_SPEEDLIMIT_OLD, &d ) )
    {

        int64_t i;
        if( tr_bencDictFindInt( d, KEY_SPEEDLIMIT_DOWN_SPEED, &i ) )
            tr_torrentSetSpeedLimit_Bps( tor, TR_DOWN, i*1024 );
        if( tr_bencDictFindInt( d, KEY_SPEEDLIMIT_DOWN_MODE, &i ) ) {
            tr_torrentUseSpeedLimit( tor, TR_DOWN, i==TR_SPEEDLIMIT_SINGLE );
            tr_torrentUseSessionLimits( tor, i==TR_SPEEDLIMIT_GLOBAL );
         }
        if( tr_bencDictFindInt( d, KEY_SPEEDLIMIT_UP_SPEED, &i ) )
            tr_torrentSetSpeedLimit_Bps( tor, TR_UP, i*1024 );
        if( tr_bencDictFindInt( d, KEY_SPEEDLIMIT_UP_MODE, &i ) ) {
            tr_torrentUseSpeedLimit( tor, TR_UP, i==TR_SPEEDLIMIT_SINGLE );
            tr_torrentUseSessionLimits( tor, i==TR_SPEEDLIMIT_GLOBAL );
        }
        ret = TR_FR_SPEEDLIMIT;
    }

    return ret;
}

static uint64_t
loadRatioLimits( tr_benc * dict, tr_torrent * tor )
{
    tr_benc * d;
    uint64_t ret = 0;

    if( tr_bencDictFindDict( dict, KEY_RATIOLIMIT, &d ) )
    {
        int64_t i;
        double dratio;
        if( tr_bencDictFindReal( d, KEY_RATIOLIMIT_RATIO, &dratio ) )
            tr_torrentSetRatioLimit( tor, dratio );
        if( tr_bencDictFindInt( d, KEY_RATIOLIMIT_MODE, &i ) )
            tr_torrentSetRatioMode( tor, i );
      ret = TR_FR_RATIOLIMIT;
    }

    return ret;
}

static uint64_t
loadIdleLimits( tr_benc * dict, tr_torrent * tor )
{
    tr_benc * d;
    uint64_t ret = 0;

    if( tr_bencDictFindDict( dict, KEY_IDLELIMIT, &d ) )
    {
        int64_t i;
        int64_t imin;
        if( tr_bencDictFindInt( d, KEY_IDLELIMIT_MINS, &imin ) )
            tr_torrentSetIdleLimit( tor, imin );
        if( tr_bencDictFindInt( d, KEY_IDLELIMIT_MODE, &i ) )
            tr_torrentSetIdleMode( tor, i );
      ret = TR_FR_IDLELIMIT;
    }

    return ret;
}

static uint64_t
loadCheatMode( tr_benc * dict, tr_torrent * tor)
{
    uint64_t ret = 0;
    int64_t val;

    if( tr_bencDictFindInt( dict, KEY_CHEATMODE, &val ) )
    {
        tr_torrentSetCheatMode( tor, (uint8_t) val );
        ret = TR_FR_CHEATMODE;
    }

    return ret;
}

static uint64_t
loadStreamingMode( tr_benc * dict, tr_torrent * tor)
{
    uint64_t ret = 0;
    int64_t val;

    if( tr_bencDictFindInt( dict, KEY_STREAMINGMODE, &val ) )
    {
        tr_torrentSetStreamingMode( tor, (uint8_t) val );
        ret = TR_FR_STREAMINGMODE;
    }

    return ret;
}

/***
****
***/

static void
bitfieldToBenc( const tr_bitfield * b, tr_benc * benc )
{
    if( tr_bitfieldHasAll( b ) )
        tr_bencInitStr( benc, "all", 3 );
    else if( tr_bitfieldHasNone( b ) )
        tr_bencInitStr( benc, "none", 4 );
    else {
        size_t byte_count = 0;
        uint8_t * raw = tr_bitfieldGetRaw( b, &byte_count );
        tr_bencInitRaw( benc, raw, byte_count );
        tr_free( raw );
    }
}


static void
saveProgress( tr_benc * dict, tr_torrent * tor )
{
    tr_benc * l;
    tr_benc * prog;
    tr_file_index_t fi;
    const tr_info * inf = tr_torrentInfo( tor );

    prog = tr_bencDictAddDict( dict, KEY_PROGRESS, 3 );

    /* add the file/piece check timestamps... */
    l = tr_bencDictAddList( prog, KEY_PROGRESS_CHECKTIME, inf->fileCount );
    for( fi=0; fi<inf->fileCount; ++fi )
    {
        const tr_piece * p;
        const tr_piece * pend;
        bool has_zero = false;
        bool checked = false;
        const tr_file * f = &inf->files[fi];

        /* get the oldest and newest nonzero timestamps for pieces in this file */
        for( p=&inf->pieces[f->firstPiece], pend=&inf->pieces[f->lastPiece]+1; p!=pend; ++p )
        {
            if( !p->checked )
                has_zero = true;
            else
                checked = true;
            if( has_zero && checked )
                break;
        }

        /* If some of a file's pieces have been checked more recently than
           the file's mtime, and some lest recently, then that file will
           have a list containing timestamps for each piece.

           However, the most common use case is that the file doesn't change
           after it's downloaded. To reduce overhead in the .resume file,
           only a single timestamp is saved for the file if *all* or *none*
           of the pieces were tested more recently than the file's mtime. */

        if( !has_zero ) /* all checked */
            tr_bencListAddInt( l, 1 );
        else if( !checked ) /* none checked */
            tr_bencListAddInt( l, 0 );
        else { /* some are checked, some aren't... so list piece by piece */
            tr_benc * ll = tr_bencListAddList( l, 2 + f->lastPiece - f->firstPiece );
            tr_bencListAddInt( ll, 0 ); /* not used, backwards compatibility */
            for( p=&inf->pieces[f->firstPiece], pend=&inf->pieces[f->lastPiece]+1; p!=pend; ++p )
                tr_bencListAddInt( ll, p->checked ? 1 : 0 );
        }
    }

    /* add the progress */
    if( tor->completeness == TR_SEED )
        tr_bencDictAddStr( prog, KEY_PROGRESS_HAVE, "all" );

    /* add the blocks bitfield */
    bitfieldToBenc( &tor->completion.blockBitfield,
                    tr_bencDictAdd( prog, KEY_PROGRESS_BLOCKS ) );
}

static uint64_t
loadProgress( tr_benc * dict, tr_torrent * tor )
{
    size_t i, n;
    uint64_t ret = 0;
    tr_benc * prog;
    const tr_info * inf = tr_torrentInfo( tor );

    for( i=0, n=inf->pieceCount; i<n; ++i )
        inf->pieces[i].checked = 0;

    if( tr_bencDictFindDict( dict, KEY_PROGRESS, &prog ) )
    {
        const char * err;
        const char * str;
        const uint8_t * raw;
        size_t rawlen;
        tr_benc * l;
        tr_benc * b;
        struct tr_bitfield blocks = TR_BITFIELD_INIT;

        if( tr_bencDictFindList( prog, KEY_PROGRESS_CHECKTIME, &l ) )
        {
            /* per-piece timestamps were added in 2.20.

               If some of a file's pieces have been checked more recently than
               the file's mtime, and some lest recently, then that file will
               have a list containing timestamps for each piece.

               However, the most common use case is that the file doesn't change
               after it's downloaded. To reduce overhead in the .resume file,
               only a single timestamp is saved for the file if *all* or *none*
               of the pieces were tested more recently than the file's mtime. */

            tr_file_index_t fi;

            for( fi=0; fi<inf->fileCount; ++fi )
            {
                tr_benc * b = tr_bencListChild( l, fi );
                const tr_file * f = &inf->files[fi];
                tr_piece * p = &inf->pieces[f->firstPiece];
                const tr_piece * pend = &inf->pieces[f->lastPiece]+1;

                if( tr_bencIsInt( b ) )
                {
                    int64_t t;
                    tr_bencGetInt( b, &t );
                    if( t )
                        for( ; p!=pend; ++p )
                            p->checked = 1;
                    else
                        for( ; p!=pend; ++p )
                            p->checked = 0;
                }
                else if( tr_bencIsList( b ) )
                {
                    int i = 0;
                    int64_t unused = 0; /* backwards compatibility */
                    const int pieces = f->lastPiece + 1 - f->firstPiece;

                    tr_bencGetInt( tr_bencListChild( b, 0 ), &unused ); /* backwards compatibility */

                    for( i=0; i<pieces; ++i )
                    {
                        int64_t t = 0;
                        tr_bencGetInt( tr_bencListChild( b, i+1 ), &t );
                        inf->pieces[f->firstPiece+i].checked = t ? 1 : 0;
                    }
                }
            }
        }
        else if( tr_bencDictFindList( prog, KEY_PROGRESS_MTIMES, &l ) )
        {
            tr_file_index_t fi;

            /* Before 2.20, we stored the files' mtimes in the .resume file.
               When loading the .resume file, a torrent's file would be flagged
               as untested if its stored mtime didn't match its real mtime. */

            for( fi=0; fi<inf->fileCount; ++fi )
            {
                int64_t t;

                if( tr_bencGetInt( tr_bencListChild( l, fi ), &t ) )
                {
                    const tr_file * f = &inf->files[fi];
                    tr_piece * p = &inf->pieces[f->firstPiece];
                    const tr_piece * pend = &inf->pieces[f->lastPiece]+1;
                    const time_t mtime = tr_torrentGetFileMTime( tor, fi );
                    const int8_t checked = mtime==t ? 1 : 0;

                    for( ; p!=pend; ++p )
                        p->checked = checked;
                }
            }
        }

        err = NULL;
        tr_bitfieldConstruct( &blocks, tor->blockCount );

        if(( b = tr_bencDictFind( prog, KEY_PROGRESS_BLOCKS )))
        {
            size_t buflen;
            const uint8_t * buf;

            if( !tr_bencGetRaw( b, &buf, &buflen ) )
                err = "Invalid value for \"blocks\"";
            else if( ( buflen == 3 ) && !memcmp( buf, "all", 3 ) )
                tr_bitfieldSetHasAll( &blocks );
            else if( ( buflen == 4 ) && !memcmp( buf, "none", 4 ) )
                tr_bitfieldSetHasNone( &blocks );
            else
                tr_bitfieldSetRaw( &blocks, buf, buflen, true );
        }
        else if( tr_bencDictFindStr( prog, KEY_PROGRESS_HAVE, &str ) )
        {
            if( !strcmp( str, "all" ) )
                tr_bitfieldSetHasAll( &blocks );
            else
                err = "Invalid value for HAVE";
        }
        else if( tr_bencDictFindRaw( prog, KEY_PROGRESS_BITFIELD, &raw, &rawlen ) )
        {
            tr_bitfieldSetRaw( &blocks, raw, rawlen, true );
        }
        else err = "Couldn't find 'pieces' or 'have' or 'bitfield'";

        if( err != NULL )
            tr_tordbg( tor, "Torrent needs to be verified - %s", err );
        else
            tr_cpBlockInit( &tor->completion, &blocks );

        tr_bitfieldDestruct( &blocks );
        ret = TR_FR_PROGRESS;
    }

    return ret;
}

/***
****
***/

void
tr_torrentSaveResume( tr_torrent * tor )
{
    int err;
    tr_benc top;
    char * filename;

    if( !tr_isTorrent( tor ) )
        return;

    tr_bencInitDict( &top, 55 ); /* arbitrary "big enough" number */
    tr_bencDictAddInt( &top, KEY_TIME_SEEDING, tor->secondsSeeding );
    tr_bencDictAddInt( &top, KEY_TIME_DOWNLOADING, tor->secondsDownloading );
    tr_bencDictAddInt( &top, KEY_ACTIVITY_DATE, tor->activityDate );
    tr_bencDictAddInt( &top, KEY_ADDED_DATE, tor->addedDate );
    tr_bencDictAddInt( &top, KEY_CORRUPT, tor->corruptPrev + tor->corruptCur );
    tr_bencDictAddInt( &top, KEY_DONE_DATE, tor->doneDate );
    if( tor->downloadDir != NULL )
        tr_bencDictAddStr( &top, KEY_DOWNLOAD_DIR, tor->downloadDir );
    if( tor->incompleteDir != NULL )
        tr_bencDictAddStr( &top, KEY_INCOMPLETE_DIR, tor->incompleteDir );
    if( tor->pieceTempDir != NULL )
        tr_bencDictAddStr( &top, KEY_PIECE_TEMP_DIR, tor->pieceTempDir );
    tr_bencDictAddStr (&top, KEY_GROUP, tor->downloadGroup);
    tr_bencDictAddInt( &top, KEY_DOWNLOADED, tor->downloadedPrev + tor->downloadedCur );
    tr_bencDictAddInt( &top, KEY_UPLOADED, tor->uploadedPrev + tor->uploadedCur );
    tr_bencDictAddInt( &top, KEY_MAX_PEERS, tor->maxConnectedPeers );
    tr_bencDictAddInt( &top, KEY_BANDWIDTH_PRIORITY, tr_torrentGetPriority( tor ) );
    tr_bencDictAddBool( &top, KEY_PAUSED, !tor->isRunning && !tor->isQueued );
    tr_bencDictAddReal( &top, KEY_CHEATRATIO, tor->cheatRatio );
    tr_bencDictAddBool( &top, KEY_PRIVATEENABLED, tor->info.isPrivate );
    savePeers( &top, tor );
    if( tr_torrentHasMetadata( tor ) )
    {
        saveFilePriorities( &top, tor );
        saveDND( &top, tor );
        saveProgress( &top, tor );
    }
    saveSpeedLimits( &top, tor );
    saveRatioLimits( &top, tor );
    saveIdleLimits( &top, tor );
    saveStreamingMode( &top, tor );
    saveCheatMode( &top, tor );

    filename = getResumeFilename(tor, TR_METAINFO_BASENAME_NAME_AND_PARTIAL_HASH);

    if( ( tor->downloadDir == NULL ) || ( tor->pieceTempDir == NULL ) )
    {
        bool wasn = tor->isStopping;
        tr_torrentSetLocalError( tor, "%s", _( "Illegal (null) download and/or pieceTemp directory" ) );
        tor->isStopping = wasn;
    }
    else if(( err = tr_bencToFile( &top, TR_FMT_BENC, filename )))
        {
            bool was = tor->isStopping;
            tr_torrentSetLocalError( tor, "Unable to save resume file: %s", tr_strerror( err ) );
            tor->isStopping = was;
        }

    tr_free( filename );

    tr_bencFree( &top );
}

static uint64_t
loadFromFile(tr_torrent* tor, uint64_t fieldsToLoad, bool* didMigrateRename)
{
    int64_t  i;
    const char * str;
    char * filename;
    tr_benc top;
    bool boolVal;
    uint64_t fieldsLoaded = 0;
    const bool wasDirty = tor->isDirty;

    assert( tr_isTorrent( tor ) );

    if (didMigrateRename != NULL)
    {
        *didMigrateRename = false;
    }

    filename = getResumeFilename(tor, TR_METAINFO_BASENAME_NAME_AND_PARTIAL_HASH);

    if( tr_bencLoadFile( &top, TR_FMT_BENC, filename ) )
    {
        tr_tordbg( tor, "Couldn't read \"%s\"", filename );


        char* old_filename = getResumeFilename(tor, TR_METAINFO_BASENAME_HASH);

        if (tr_bencLoadFile( &top, TR_FMT_BENC, old_filename ))
        {
            tr_tordbg( tor, "Couldn't read \"%s\"", old_filename );

            tr_free(old_filename);
            tr_free(filename);
            return fieldsLoaded;
        }

        if (!rename(old_filename, filename))
        {
            tr_tordbg(tor, "Migrated resume file from \"%s\" to \"%s\"", old_filename, filename);

            if (didMigrateRename != NULL)
            {
                *didMigrateRename = true;
            }
        }

        tr_free(old_filename);
    }

    tr_tordbg( tor, "Read resume file \"%s\"", filename );

    if( ( fieldsToLoad & TR_FR_CORRUPT )
      && tr_bencDictFindInt( &top, KEY_CORRUPT, &i ) )
    {
        tor->corruptPrev = i;
        fieldsLoaded |= TR_FR_CORRUPT;
    }

    if( ( fieldsToLoad & ( TR_FR_PROGRESS | TR_FR_DOWNLOAD_DIR ) )
      && ( tr_bencDictFindStr( &top, KEY_DOWNLOAD_DIR, &str ) )
      && ( str && *str ) )
    {
        tr_free( tor->downloadDir );
        tor->downloadDir = tr_strdup( str );
        fieldsLoaded |= TR_FR_DOWNLOAD_DIR;
    }

    if( ( fieldsToLoad & ( TR_FR_PROGRESS | TR_FR_INCOMPLETE_DIR ) )
      && ( tr_bencDictFindStr( &top, KEY_INCOMPLETE_DIR, &str ) )
      && ( str && *str ) )
    {
        tr_free( tor->incompleteDir );
        tor->incompleteDir = tr_strdup( str );
        fieldsLoaded |= TR_FR_INCOMPLETE_DIR;
    }

    if( ( fieldsToLoad & TR_FR_PIECE_TEMP_DIR )
        && tr_bencDictFindStr( &top, KEY_PIECE_TEMP_DIR, &str )
        && str && *str )
    {
        tr_free( tor->pieceTempDir );
        tor->pieceTempDir = tr_strdup( str );
        fieldsLoaded |= TR_FR_PIECE_TEMP_DIR;
    }

  if ((fieldsToLoad & (TR_FR_PROGRESS | TR_FR_GROUP))
      && (tr_bencDictFindStr (&top, KEY_GROUP, &str))
      && (str && *str))
    {
      if (tr_strcmp0 (str, tor->downloadGroup))
        {
          tr_free (tor->downloadGroup);
          tor->downloadGroup = tr_strdup (str);
        }
        fieldsLoaded |= TR_FR_GROUP;
    }


    if( ( fieldsToLoad & TR_FR_DOWNLOADED )
      && tr_bencDictFindInt( &top, KEY_DOWNLOADED, &i ) )
    {
        tor->downloadedPrev = i;
        fieldsLoaded |= TR_FR_DOWNLOADED;
    }

    if( ( fieldsToLoad & TR_FR_UPLOADED )
      && tr_bencDictFindInt( &top, KEY_UPLOADED, &i ) )
    {
        tor->uploadedPrev = i;
        fieldsLoaded |= TR_FR_UPLOADED;
    }

    if( ( fieldsToLoad & TR_FR_MAX_PEERS )
      && tr_bencDictFindInt( &top, KEY_MAX_PEERS, &i ) )
    {
        tor->maxConnectedPeers = i;
        fieldsLoaded |= TR_FR_MAX_PEERS;
    }

    if( ( fieldsToLoad & TR_FR_RUN )
      && tr_bencDictFindBool( &top, KEY_PAUSED, &boolVal ) )
    {
        tor->isRunning = !boolVal;
        fieldsLoaded |= TR_FR_RUN;
    }

    if( ( fieldsToLoad & TR_FR_PRIVATE_ENABLED )
      && tr_bencDictFindBool( &top, KEY_PRIVATEENABLED, &boolVal ) )
    {
        tor->info.isPrivate = boolVal;
        fieldsLoaded |= TR_FR_PRIVATE_ENABLED;
    }

    double cratio;
    if( ( fieldsToLoad & TR_FR_CHEAT_RATIO )
      && tr_bencDictFindReal( &top, KEY_CHEATRATIO, &cratio ) )
    {
        tor->cheatRatio = cratio;
        fieldsLoaded |= TR_FR_CHEAT_RATIO;
    }

    if( ( fieldsToLoad & TR_FR_ADDED_DATE )
      && tr_bencDictFindInt( &top, KEY_ADDED_DATE, &i ) )
    {
        tor->addedDate = i;
        fieldsLoaded |= TR_FR_ADDED_DATE;
    }

    if( ( fieldsToLoad & TR_FR_DONE_DATE )
      && tr_bencDictFindInt( &top, KEY_DONE_DATE, &i ) )
    {
        tor->doneDate = i;
        fieldsLoaded |= TR_FR_DONE_DATE;
    }

    if( ( fieldsToLoad & TR_FR_ACTIVITY_DATE )
      && tr_bencDictFindInt( &top, KEY_ACTIVITY_DATE, &i ) )
    {
        tr_torrentSetActivityDate( tor, i );
        fieldsLoaded |= TR_FR_ACTIVITY_DATE;
    }

    if( ( fieldsToLoad & TR_FR_TIME_SEEDING )
      && tr_bencDictFindInt( &top, KEY_TIME_SEEDING, &i ) )
    {
        tor->secondsSeeding = i;
        fieldsLoaded |= TR_FR_TIME_SEEDING;
    }

    if( ( fieldsToLoad & TR_FR_TIME_DOWNLOADING )
      && tr_bencDictFindInt( &top, KEY_TIME_DOWNLOADING, &i ) )
    {
        tor->secondsDownloading = i;
        fieldsLoaded |= TR_FR_TIME_DOWNLOADING;
    }

    if( ( fieldsToLoad & TR_FR_BANDWIDTH_PRIORITY )
      && tr_bencDictFindInt( &top, KEY_BANDWIDTH_PRIORITY, &i )
      && tr_isPriority( i ) )
    {
        tr_torrentSetPriority( tor, i );
        fieldsLoaded |= TR_FR_BANDWIDTH_PRIORITY;
    }

    if( fieldsToLoad & TR_FR_PEERS )
        fieldsLoaded |= loadPeers( &top, tor );

    if( fieldsToLoad & TR_FR_FILE_PRIORITIES )
        fieldsLoaded |= loadFilePriorities( &top, tor );

    if( fieldsToLoad & TR_FR_PROGRESS )
        fieldsLoaded |= loadProgress( &top, tor );

    if( fieldsToLoad & TR_FR_DND )
        fieldsLoaded |= loadDND( &top, tor );

    if( fieldsToLoad & TR_FR_SPEEDLIMIT )
        fieldsLoaded |= loadSpeedLimits( &top, tor );

    if( fieldsToLoad & TR_FR_RATIOLIMIT )
        fieldsLoaded |= loadRatioLimits( &top, tor );

    if( fieldsToLoad & TR_FR_IDLELIMIT )
        fieldsLoaded |= loadIdleLimits( &top, tor );

    if( fieldsToLoad & TR_FR_STREAMINGMODE )
        fieldsLoaded |= loadStreamingMode( &top, tor );

    if( fieldsToLoad & TR_FR_CHEATMODE )
        fieldsLoaded |= loadCheatMode( &top, tor );


    /* loading the resume file triggers of a lot of changes,
     * but none of them needs to trigger a re-saving of the
     * same resume information... */
    tor->isDirty = wasDirty;

    tr_bencFree( &top );
    tr_free( filename );
    return fieldsLoaded;
}

static uint64_t
setFromCtor( tr_torrent * tor, uint64_t fields, const tr_ctor * ctor, int mode )
{
    uint64_t ret = 0;

    if( fields & TR_FR_RUN )
    {
        bool isPaused;
        if( !tr_ctorGetPaused( ctor, mode, &isPaused ) )
        {
            tor->isRunning = !isPaused;
            ret |= TR_FR_RUN;
        }
    }

    if( fields & TR_FR_MAX_PEERS )
        if( !tr_ctorGetPeerLimit( ctor, mode, &tor->maxConnectedPeers ) )
            ret |= TR_FR_MAX_PEERS;

    if( fields & TR_FR_DOWNLOAD_DIR )
    {
        const char * path;
        if( !tr_ctorGetDownloadDir( ctor, mode, &path ) && path && *path )
        {
            ret |= TR_FR_DOWNLOAD_DIR;
            tr_free( tor->downloadDir );
            tor->downloadDir = tr_strdup( path );
        }
    }

    return ret;
}

static uint64_t
useManditoryFields( tr_torrent * tor, uint64_t fields, const tr_ctor * ctor )
{
    return setFromCtor( tor, fields, ctor, TR_FORCE );
}

static uint64_t
useFallbackFields( tr_torrent * tor, uint64_t fields, const tr_ctor * ctor )
{
    return setFromCtor( tor, fields, ctor, TR_FALLBACK );
}

uint64_t
tr_torrentLoadResume( tr_torrent *    tor,
                      uint64_t        fieldsToLoad,
                      const tr_ctor * ctor,
                      bool* didMigrateRename )
{
    uint64_t ret = 0;

    assert( tr_isTorrent( tor ) );

    ret |= useManditoryFields( tor, fieldsToLoad, ctor );
    fieldsToLoad &= ~ret;
    ret |= loadFromFile( tor, fieldsToLoad, didMigrateRename );
    fieldsToLoad &= ~ret;
    ret |= useFallbackFields( tor, fieldsToLoad, ctor );

    return ret;
}

void
tr_torrentRemoveResume( const tr_torrent * tor )
{
    char * filename;

    filename = getResumeFilename(tor, TR_METAINFO_BASENAME_NAME_AND_PARTIAL_HASH);
    unlink( filename );
    tr_free( filename );

    filename = getResumeFilename(tor, TR_METAINFO_BASENAME_HASH);
    unlink( filename );
    tr_free( filename );
}
