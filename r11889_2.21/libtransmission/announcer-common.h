/******************************************************************************
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

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#ifndef _TR_ANNOUNCER_COMMON_H_
#define _TR_ANNOUNCER_COMMON_H_

enum
{
    /* unless the tracker says otherwise, rescrape this frequently */
    DEFAULT_SCRAPE_INTERVAL_SEC = ( 60 * 30 ),

    /* unless the tracker says otherwise, this is the announce interval */
    DEFAULT_ANNOUNCE_INTERVAL_SEC = ( 60 * 10 ),

    /* unless the tracker says otherwise, this is the announce min_interval */
    DEFAULT_ANNOUNCE_MIN_INTERVAL_SEC = ( 60 * 2 ),

    /* the length of the 'key' argument passed in tracker requests */
    KEYLEN = 8,

    /* how many web tasks we allow at one time */
    MAX_CONCURRENT_TASKS = 48,

    /* if a tracker takes more than this long to respond,
     * we treat it as nonresponsive */
    MAX_TRACKER_RESPONSE_TIME_SECS = ( 60 * 2 ),

    /* the value of the 'numwant' argument passed in tracker requests. */
    NUMWANT = 80,

    UPKEEP_INTERVAL_SECS = 1,

    /* this is an upper limit for the frequency of LDS announces */
    LPD_HOUSEKEEPING_INTERVAL_SECS = 30

};

/***
****
***/

struct au_context;

/**
 * "global" (per-tr_session) fields
 */
typedef struct tr_announcer
{
    tr_ptrArray stops; /* struct stop_message */
    tr_session * session;
    struct au_context * udpctx;
    struct event * upkeepTimer;
    int slotsAvailable;
    time_t lpdHouseKeepingAt;
}
tr_announcer;

typedef enum tr_tracker_types
{
    TR_TRACKER_TYPE_WEB,
    TR_TRACKER_TYPE_UDP
}
tr_tracker_type;

/* a row in tr_tier's list of trackers */
typedef struct
{
    tr_tracker_type type;
    char * hostname;
    char * announce;
    char * scrape;

    char * tracker_id;

    int seederCount;
    int leecherCount;
    int downloadCount;
    int downloaderCount;

    int consecutiveAnnounceFailures;

    uint32_t id;

    /* sent as the "key" argument in tracker requests
     * to verify us if our IP address changes.
     * This is immutable for the life of the tracker object.
     * The +1 is for '\0' */
    unsigned char key_param[KEYLEN + 1];
}
tr_tracker_item;

struct tr_torrent_tiers;

/** @brief A group of trackers in a single tier, as per the multitracker spec */
typedef struct
{
    /* number of up/down/corrupt bytes since the last time we sent an
     * "event=stopped" message that was acknowledged by the tracker */
    uint64_t byteCounts[3];

    tr_ptrArray trackers; /* tr_tracker_item */
    tr_tracker_item * currentTracker;
    int currentTrackerIndex;

    tr_torrent * tor;

    time_t scrapeAt;
    time_t lastScrapeStartTime;
    time_t lastScrapeTime;
    tr_bool lastScrapeSucceeded;
    tr_bool lastScrapeTimedOut;

    time_t announceAt;
    time_t manualAnnounceAllowedAt;
    time_t lastAnnounceStartTime;
    time_t lastAnnounceTime;
    tr_bool lastAnnounceSucceeded;
    tr_bool lastAnnounceTimedOut;

    tr_ptrArray announceEvents; /* const char* */

    /* unique lookup key */
    int key;

    int scrapeIntervalSec;
    int announceIntervalSec;
    int announceMinIntervalSec;

    int lastAnnouncePeerCount;

    tr_bool isRunning;
    tr_bool isAnnouncing;
    tr_bool isScraping;
    tr_bool wasCopied;

    char lastAnnounceStr[128];
    char lastScrapeStr[128];
}
tr_tier;

void publishErrorClear( tr_tier * tier );
void publishMessage( tr_tier * tier, const char * msg, int type );
size_t publishPeersCompact( tr_tier * tier, int seeds, int leechers,
                            const void * compact, int compactLen );


#endif /* _TR_ANNOUNCER_COMMON_H_ */
