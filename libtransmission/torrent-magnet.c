/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: torrent-magnet.c 14664 2016-01-08 22:57:17Z jordan $
 */

#include <assert.h>
#include <stdio.h> /* remove() */
#include <string.h> /* memcpy(), memset(), memcmp() */

#include <event2/buffer.h>

#include "transmission.h"
#include "bencode.h"
#include "crypto.h" /* tr_sha1() */
#include "magnet.h"
#include "metainfo.h"
#include "resume.h"
#include "torrent.h"
#include "torrent-magnet.h"
#include "utils.h"
#include "web.h"

#define dbgmsg( tor, ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, tor->info.name, __VA_ARGS__ ); \
    } while( 0 )

/***
****
***/

enum
{
    /* don't ask for the same metadata piece more than this often */
    MIN_REPEAT_INTERVAL_SECS = 3
};

struct metadata_node
{
    time_t requestedAt;
    int piece;
};

struct tr_incomplete_metadata
{
    uint8_t * metadata;
    int metadata_size;
    int pieceCount;

    /** sorted from least to most recently requested */
    struct metadata_node * piecesNeeded;
    int piecesNeededCount;
    bool incomplete_metadata_failed;
    int bad_piece_count;
};

static void
incompleteMetadataFree( struct tr_incomplete_metadata * m )
{
    tr_free( m->metadata );
    tr_free( m->piecesNeeded );
    tr_free( m );
}

void
tr_torrentSetMetadataSizeHint( tr_torrent * tor, int size )
{
    if( !tr_torrentHasMetadata( tor ) )
    {
        if( tor->incompleteMetadata == NULL )
        {
            int i;
            struct tr_incomplete_metadata * m;
            int n = ( size + ( METADATA_PIECE_SIZE - 1 ) ) / METADATA_PIECE_SIZE;
            dbgmsg( tor, "metadata is %d bytes in %d pieces", size, n );

            m = tr_new( struct tr_incomplete_metadata, 1 );
            if( m == NULL )
                return;

            m->bad_piece_count = 0;
            m->metadata = NULL;
            m->piecesNeeded = NULL;

            if( ( n < 1 ) || ( size < 1 ) )
            {
                tr_err( "Metadata Size Hint failure size %d, n %d", size, n );
                n = 0;
                size = 0;
            }
            else
                m->incomplete_metadata_failed = false;

            m->metadata_size = size;
            m->piecesNeededCount = n;
            m->pieceCount = n;

            if( size )
                m->metadata = tr_new( uint8_t, size );
            if( m->metadata == NULL )
                m->incomplete_metadata_failed = true;

            if( n )
                m->piecesNeeded = tr_new( struct metadata_node, n );
            if( m->piecesNeeded == NULL )
                m->incomplete_metadata_failed = true;
            else for( i=0; i<n; ++i ) {
                m->piecesNeeded[i].piece = i;
                m->piecesNeeded[i].requestedAt = 0;
            }
            tor->incompleteMetadata = m;
        }
    }
}

static int
findInfoDictOffset( const tr_torrent * tor )
{
    size_t fileLen;
    uint8_t * fileContents;
    int offset = 0;

    /* load the file, and find the info dict's offset inside the file */
    if(( fileContents = tr_loadFile( tor->info.torrent, &fileLen )))
    {
        tr_benc top;

        if( !tr_bencParse( fileContents, fileContents + fileLen, &top, NULL ) )
        {
            tr_benc * infoDict;

            if( tr_bencDictFindDict( &top, "info", &infoDict ) )
            {
                int infoLen;
                char * infoContents = tr_bencToStr( infoDict, TR_FMT_INFO_DICT, &infoLen );
                const uint8_t * i = (const uint8_t*) tr_memmem( (char*)fileContents, fileLen, infoContents, infoLen );
                offset = i != NULL ? i - fileContents : 0;
                tr_free( infoContents );
            }

            tr_bencFree( &top );
        }

        tr_free( fileContents );
    }

    return offset;
}

static void
ensureInfoDictOffsetIsCached( tr_torrent * tor )
{
    assert( tr_torrentHasMetadata( tor ) );

    if( !tor->infoDictOffsetIsCached )
    {
        tor->infoDictOffset = findInfoDictOffset( tor );
        tor->infoDictOffsetIsCached = true;
    }
}

void*
tr_torrentGetMetadataPiece( tr_torrent * tor, int piece, int * len )
{
    char * ret = NULL;

    assert( tr_isTorrent( tor ) );
    assert( piece >= 0 );
    assert( len != NULL );

    if( tr_torrentHasMetadata( tor ) )
    {
        FILE * fp;

        ensureInfoDictOffsetIsCached( tor );

        assert( tor->infoDictLength > 0 );
        assert( tor->infoDictOffset >= 0 );

        fp = fopen( tor->info.torrent, "rb" );
        if( fp != NULL )
        {
            const int o = piece  * METADATA_PIECE_SIZE;

            if( !fseek( fp, tor->infoDictOffset + o, SEEK_SET ) )
            {
                const int l = o + METADATA_PIECE_SIZE <= tor->infoDictLength
                            ? METADATA_PIECE_SIZE
                            : tor->infoDictLength - o;

                if( 0<l && l<=METADATA_PIECE_SIZE )
                {
                    char * buf = tr_new( char, l );
                    const int n = fread( buf, 1, l, fp );
                    if( n == l )
                    {
                        *len = l;
                        ret = buf;
                        buf = NULL;
                    }

                    tr_free( buf );
                }
            }

            fclose( fp );
        }
    }

    return ret;
}

void
tr_torrentSetMetadataPiece( tr_torrent  * tor, int piece, const void  * data, int len, int64_t totalSize )
{
    int i;
    int64_t metadataSize;
    struct tr_incomplete_metadata * m;
    const int offset = piece * METADATA_PIECE_SIZE;

    assert( tr_isTorrent( tor ) );

    dbgmsg( tor, "got metadata piece %d", piece );

    /* are we set up to download metadata? */
    m = tor->incompleteMetadata;
    if( m == NULL )
        return;

    /* max set to zero or less disables magnet link torrents */
    if( ( tor->session->maxMagnetBadPiece < 1 )
         || ( ( tor->session->maxMagnetBadPiece == 1 ) && ( m->incomplete_metadata_failed ) ) )
    {
        incompleteMetadataFree( tor->incompleteMetadata );
        tor->incompleteMetadata = NULL;
        tr_torrentSetLocalError( tor, "%s", _( "Magnet torrents disabled. To enable set magnet-bad-piece-max to 2 or greater.-set-" ) );
        return;
    }

    if( m->incomplete_metadata_failed )
    {
        tr_err( "Incomplete metadata failed" );
        incompleteMetadataFree( tor->incompleteMetadata );
        tor->incompleteMetadata = NULL;
        return;
    }

    metadataSize = (int64_t)m->metadata_size;
    dbgmsg( tor, "metadata size %d    total size %d", m->metadata_size, (int)totalSize );

    /* does this data pass the smell test? */
    if( ( offset + len > m->metadata_size ) || ( metadataSize > totalSize ) )
    {
        if( ++m->bad_piece_count > tor->session->maxMagnetBadPiece )
        {
            incompleteMetadataFree( tor->incompleteMetadata );
            tor->incompleteMetadata = NULL;
            tr_torrentSetLocalError( tor, "%s", _( "Magnet bad piece count exceeded. To try again restart the torrent." ) );
        }
        return;
    }

    /* do we need this piece? */
    for( i=0; i<m->piecesNeededCount; ++i )
        if( m->piecesNeeded[i].piece == piece )
            break;
    if( i==m->piecesNeededCount )
        return;

    memcpy( m->metadata + offset, data, len );

    tr_removeElementFromArray( m->piecesNeeded, i,
                               sizeof( struct metadata_node ),
                               m->piecesNeededCount-- );

    dbgmsg( tor, "saving metainfo piece %d... %d remain", piece, m->piecesNeededCount );

    /* are we done? */
    if( m->piecesNeededCount == 0 )
    {
        bool success = false;
        bool validBlockSize = false;
        bool checksumPassed = false;
        bool metainfoParsed = false;
        uint8_t sha1[SHA_DIGEST_LENGTH];

        /* we've got a complete set of metainfo... see if it passes the checksum test */
        dbgmsg( tor, "metainfo piece %d was the last one", piece );
        tr_sha1( sha1, m->metadata, m->metadata_size, NULL );
        if(( checksumPassed = !memcmp( sha1, tor->info.hash, SHA_DIGEST_LENGTH )))
        {
            /* checksum passed; now try to parse it as benc */
            tr_benc infoDict;
            const int err = tr_bencLoad( m->metadata, m->metadata_size, &infoDict, NULL );
            dbgmsg( tor, "err is %d", err );
            if(( metainfoParsed = !err ))
            {
                /* yay we have bencoded metainfo... merge it into our .torrent file */
                tr_benc newMetainfo;
                char * path = tr_strdup( tor->info.torrent );

                if( !tr_bencLoadFile( &newMetainfo, TR_FMT_BENC, path ) )
                {
                    bool hasInfo;
                    tr_info info;
                    int infoDictLength;
                    char hexH[41];

                    tr_sha1_to_hex( hexH, sha1 );
                    tr_magBencMergeDicts( tr_bencDictAddDict( &newMetainfo, "info", 0 ), &infoDict );
                    tr_bencDictAddStr( &newMetainfo, "incoming_magnet_hash", hexH );

                    memset( &info, 0, sizeof( tr_info ) );
                    success = tr_metainfoParse( tor->session, &newMetainfo, &info, &hasInfo, &infoDictLength );
                    dbgmsg( tor, "Parsed completed metadata info -length- %d", infoDictLength );

                    if( success && !tr_getBlockSize( info.pieceSize ) )
                    {
                        validBlockSize = false;
                        tr_metainfoFree( &info );
                    }
                    else
                    {
                        validBlockSize = success;
                    }

                    if( validBlockSize )
                    {
                        /* remove any old .torrent and .resume files */
                        remove( path );
                        tr_torrentRemoveResume( tor );

                        dbgmsg( tor, "Saving completed metadata to \"%s\"", path );
                        /* keep the new info */
                        tor->info = info;
                        tor->infoDictLength = infoDictLength;

                        /* save the new .torrent file */
                        tr_bencToFile( &newMetainfo, TR_FMT_BENC_TORRENT, tor->info.torrent );
                        tr_sessionSetTorrentFile( tor->session, tor->info.hashString, tor->info.torrent );
                        tr_torrentGotNewInfoDict( tor );
                        tr_torrentSetDirty( tor );
                    }

                    tr_bencFree( &newMetainfo );
                }

                tr_bencFree( &infoDict );
                tr_free( path );
            }
        }

        if( validBlockSize )
        {
            incompleteMetadataFree( tor->incompleteMetadata );
            tor->incompleteMetadata = NULL;
            tor->isStopping = true;
            tor->magnetVerify = true;
            tor->startAfterVerify = !tor->preFetchMagnet;
            tor->preFetchMagnet = false;
        }
        else /* drat. */
        {
            if( !validBlockSize && success )
            {
                incompleteMetadataFree( tor->incompleteMetadata );
                tor->incompleteMetadata = NULL;
                tr_torrentSetLocalError( tor, "%s", _( "Magnet metadata unusable -- bad BlockSize from pieceSize" ) );
            }
            else if( checksumPassed )
            {
                incompleteMetadataFree( tor->incompleteMetadata );
                tor->incompleteMetadata = NULL;
                tr_torrentSetLocalError( tor, "%s", _( "Magnet metadata unusable -- parse fail with checksum pass" ) );
            }
            else if( ++m->bad_piece_count > tor->session->maxMagnetBadPiece )
            {
                incompleteMetadataFree( tor->incompleteMetadata );
                tor->incompleteMetadata = NULL;
                tr_torrentSetLocalError( tor, "%s", _( "Magnet bad piece count exceeded. To try again restart torrent." ) );
            }
            else
            {
                const int n = m->pieceCount;
                for( i=0; i<n; ++i )
                {
                    m->piecesNeeded[i].piece = i;
                    m->piecesNeeded[i].requestedAt = 0;
                }
                m->piecesNeededCount = n;
                dbgmsg( tor, "metadata error; trying again. %d pieces left", n );

                tr_err( "magnet status: checksum passed %d, metainfo parsed %d",
                        (int)checksumPassed, (int)metainfoParsed );
            }
        }
    }
}

bool
tr_torrentGetNextMetadataRequest( tr_torrent * tor, time_t now, int * setme_piece )
{
    bool have_request = false;
    struct tr_incomplete_metadata * m;

    assert( tr_isTorrent( tor ) );

    m = tor->incompleteMetadata;

    if( m == NULL )
        return have_request;

    /* max set to zero or less disables magnet link torrents */
    if( ( tor->session->maxMagnetBadPiece < 1 )
         || ( ( tor->session->maxMagnetBadPiece == 1 ) && ( m->incomplete_metadata_failed ) ) )
    {
        incompleteMetadataFree( tor->incompleteMetadata );
        tor->incompleteMetadata = NULL;
        tr_torrentSetLocalError( tor, "%s", _( "Magnet torrents disabled. To enable set magnet-bad-piece-max to 2 or greater." ) );
        return have_request;
    }

    if( m->incomplete_metadata_failed )
    {
        tr_err( "Incomplete metadata failed" );
        incompleteMetadataFree( tor->incompleteMetadata );
        tor->incompleteMetadata = NULL;
        return have_request;
    }

    if( ( m->piecesNeededCount > 0 ) && ( m->piecesNeeded[0].requestedAt + MIN_REPEAT_INTERVAL_SECS < now ) )
    {
        int i;
        const int piece = m->piecesNeeded[0].piece;

        tr_removeElementFromArray( m->piecesNeeded, 0,
                                   sizeof( struct metadata_node ),
                                   m->piecesNeededCount-- );

        i = m->piecesNeededCount++;
        m->piecesNeeded[i].piece = piece;
        m->piecesNeeded[i].requestedAt = now;

        dbgmsg( tor, "next piece to request: %d", piece );
        *setme_piece = piece;
        have_request = true;
    }

    return have_request;
}

double
tr_torrentGetMetadataPercent( const tr_torrent * tor )
{
    double ret;

    if( tr_torrentHasMetadata( tor ) )
        ret = 1.0;
    else {
        const struct tr_incomplete_metadata * m = tor->incompleteMetadata;
        if( !m || !m->pieceCount )
            ret = 0.0;
        else
            ret = (m->pieceCount - m->piecesNeededCount) / (double)m->pieceCount;
    }

    return ret;
}

char*
tr_torrentInfoGetMagnetLink( const tr_info * inf )
{
    int i;
    const char * name;
    struct evbuffer * s = evbuffer_new( );

    evbuffer_add_printf( s, "magnet:?xt=urn:btih:%s", inf->hashString );

    name = inf->name;
    if( name && *name )
    {
        evbuffer_add_printf( s, "%s", "&dn=" );
        tr_http_escape( s, name, -1, true );
    }

    for( i=0; i<inf->trackerCount; ++i )
    {
        evbuffer_add_printf( s, "%s", "&tr=" );
        tr_http_escape( s, inf->trackers[i].announce, -1, true );
    }

  for (i=0; i<inf->webseedCount; i++) 
    { 
      evbuffer_add_printf (s, "%s", "&ws="); 
      tr_http_escape (s, inf->webseeds[i], -1, true); 
    } 

    return evbuffer_free_to_str( s );
}
