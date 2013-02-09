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

#ifndef _TR_ANNOUNCER_UDP_H_
#define _TR_ANNOUNCER_UDP_H_

typedef struct au_context au_context;
au_context * au_context_new( tr_session * );
void au_context_free( au_context * );
void au_context_periodic( au_context * );

struct evbuffer * au_create_stop( tr_announcer *,
                                  const tr_torrent *,
                                  const tr_tier * );
/** @note Takes ownership of the @a evbuffer. */
void au_send_stop( tr_announcer *, const char *, struct evbuffer * );
void au_send_announce( tr_announcer *, const tr_torrent *,
                       const tr_tier *, const char *,
                       tr_web_done_func *, void * );
void au_send_scrape( tr_announcer *, const tr_torrent *,
                     const tr_tier *, tr_web_done_func *, void * );
tr_bool au_parse_announce( tr_tier *, const char *, size_t, tr_bool * );
tr_bool au_parse_scrape( tr_tier *, const char *, size_t, char *, size_t );

#endif /* _TR_ANNOUNCER_UDP_H_ */
