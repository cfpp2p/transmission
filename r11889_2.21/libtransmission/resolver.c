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

#include "transmission.h"
#include "list.h"
#include "net.h"
#include "platform.h"
#include "session.h"
#include "utils.h"
#include "trevent.h"
#include "resolver.h"

/* If the number of tasks waiting in the queue divided
 * by the current number of workers is greater than this
 * number, a new worker thread is created. */
#define WORKER_LOAD 5

/* Never create more worker threads than this. */
#define WORKER_MAX  10

typedef struct
{
    tr_session * session;
    char * node;
    char * service;
    int type;
    tr_resolver_callback callback;
    void * user_data;
}
resolver_task;

typedef struct
{
    const char * err;
    tr_address addr;
    tr_resolver_callback callback;
    void * user_data;
}
resolver_result;

static tr_list * queue; /* resolver_task */
static tr_lock * lock;
static int workers, tasks;

static void
notify( void * vres )
{
    resolver_result * res = vres;
    res->callback( res->err, &res->addr, res->user_data );
    tr_free( res );
}

static void
worker( void * varg UNUSED )
{
    while( 1 )
    {
        resolver_task * task;
        resolver_result * res;

        tr_lockLock( lock );
        if( !queue )
        {
            tr_lockUnlock( lock );
            break;
        }
        task = tr_list_pop_front( &queue );
        tasks--;
        tr_lockUnlock( lock );

        res = tr_new0( resolver_result, 1 );
        res->addr.type = task->type;
        res->err = tr_netGetAddress( task->node, task->service, &res->addr );
        res->callback = task->callback;
        res->user_data = task->user_data;

        tr_runInEventThread( task->session, notify, res );
        tr_free( task->node );
        tr_free( task->service );
        tr_free( task );
    }

    tr_lockLock( lock );
    workers--;
    tr_lockUnlock( lock );
}

static void
spawn_workers( void )
{
    tr_lockLock( lock );
    if( queue && ( workers < 1 || tasks / workers > WORKER_LOAD )
        && workers < WORKER_MAX )
    {
        workers++;
        tr_threadNew( worker, NULL );
    }
    tr_lockUnlock( lock );
}

void
tr_resolve_address( tr_session           * session,
                    const char           * node,
                    const char           * service,
                    int                    type,
                    tr_resolver_callback   callback,
                    void                 * user_data )
{
    resolver_task * task;

    assert( callback != NULL );

    task = tr_new0( resolver_task, 1 );
    task->session = session;
    task->node = tr_strdup( node );
    task->service = tr_strdup( service );
    task->type = type;
    task->callback = callback;
    task->user_data = user_data;

    if( !lock )
    {
        assert( tr_amInEventThread( session ) );
        lock = tr_lockNew( );
    }
    tr_lockLock( lock );
    tr_list_append( &queue, task );
    tasks++;
    tr_lockUnlock( lock );

    spawn_workers( );
}
