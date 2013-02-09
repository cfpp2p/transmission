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

#ifndef _TR_RESOLVER_H_
#define _TR_RESOLVER_H_

/**
 * If the address resolution fails, @a err will be a string
 * description of the error. Otherwise, @a err will be NULL
 * and @a addr will contain the resolved address.
 */
typedef void ( * tr_resolver_callback )( const char       * err,
                                         const tr_address * addr,
                                         void             * user_data );

/**
 * Resolve a hostname asynchronously by calling getaddrinfo(3) in
 * another thread. If you do not care about blocking the current
 * thread, you can just use tr_netGetAddress().
 *
 * @param session @a callback will be run in the event thread of this
 *                session.
 * @param node The name of the node to resolve. This will generally
 *             just be the hostname.
 * @param service Same meaning as for getaddrinfo(3). Usually you
 *                can just set this to NULL.
 * @param type The address type to prefer, either @a TR_AF_INET or
 *             @a TR_AF_INET6. Any other value will cause no particular
 *             type to be preferred and so the first valid address found
 *             will be passed to @a callback.
 * @param callback Function to call with the result (or an error message).
 *                 It will be run in the event thread of @a session by
 *                 tr_runInEventThread().
 * @param user_data User data to pass to @a callback.
 *
 * @see tr_netGetAddress()
 * @see tr_runInEventThread()
 */
void tr_resolve_address( tr_session           * session,
                         const char           * node,
                         const char           * service,
                         int                    type,
                         tr_resolver_callback   callback,
                         void                 * user_data );

#endif /* _TR_RESOLVER_H_ */
