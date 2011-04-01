/*  =========================================================================
    zctx - working with 0MQ contexts

    -------------------------------------------------------------------------
    Copyright (c) 1991-2011 iMatix Corporation <www.imatix.com>
    Copyright other contributors as noted in the AUTHORS file.

    This file is part of zapi, the C binding for 0MQ: http://zapi.zeromq.org.

    This is free software; you can redistribute it and/or modify it under the
    terms of the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your option)
    any later version.

    This software is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABIL-
    ITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
    Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
    =========================================================================
*/

/*
@header
    The zctx class wraps 0MQ contexts. It manages open sockets in the context
    and automatically closes these before terminating the context. It provides
    a simple way to set the linger timeout on sockets, and configure contexts
    for number of I/O threads. Sets-up signal (interrrupt) handling for the
    process.

    The zctx class has these main features:

    * Tracks all open sockets and automatically closes them before calling
    zmq_term(). This avoids an infinite wait on open sockets.

    * Automatically configures sockets with a ZMQ_LINGER timeout you can
    define, and which defaults to zero. The default behavior of zctx is
    therefore like 0MQ/2.0, immediate termination with loss of any pending
    messages. You can set any linger timeout you like by calling the
    zctx_set_linger() method.

    * Moves the iothreads configuration to a separate method, so that default
    usage is 1 I/O thread. Lets you configure this value.

    * Sets up signal (SIGINT and SIGTERM) handling so that blocking calls
    such as zmq_recv() and zmq_poll() will return when the user presses
    Ctrl-C.

    * Provides API to create child threads with a pipe (PAIR socket) to talk
    to them.
@discuss
    One problem is when our application needs child threads. If we simply
    use pthreads_create() we're faced with several issues. First, it's not
    portable to legacy OSes like win32. Second, how can a child thread get
    access to our zctx object? If we just pass it around, we'll end up
    sharing the pipe socket (which we use to talk to the agent) between
    threads, and that will then crash 0MQ. Sockets cannot be used from more
    than one thread at a time.

    So each child thread needs its own pipe to the agent. For the agent,
    this is fine, it can talk to a million threads. But how do we create
    those pipes in the child thread? We can't, not without help from the
    main thread. The solution is to wrap thread creation, like we wrap
    socket creation. To create a new thread, the app calls zctx_thread_new()
    and this method creates a dedicated zctx object, with a pipe, and then
    it passes that object to the newly minted child thread.

    The neat thing is we can hide non-portable aspects. Windows is really a
    mess when it comes to threads. Three different APIs, none of which is
    really right, so you have to do rubbish like manually cleaning up when
    a thread finishes. Anyhow, it's hidden in this class so you don't need
    to worry.

    Second neat thing about wrapping thread creation is we can make it a
    more enriching experience for all involved. One thing I do often is use
    a PAIR-PAIR pipe to talk from a thread to/from its parent. So this class
    will automatically create such a pair for each thread you start.
@end
*/

#include "../include/zapi_prelude.h"
#include "../include/zlist.h"
#include "../include/zstr.h"
#include "../include/zframe.h"
#include "../include/zctx.h"

//  Structure of our class

struct _zctx_t {
    void *context;              //  Our 0MQ context
    zlist_t *sockets;           //  Sockets held by this thread
    Bool main;                  //  TRUE if we're the main thread
    int iothreads;              //  Number of IO threads, default 1
    int linger;                 //  Linger timeout, default 0
};


//  ---------------------------------------------------------------------
//  Signal handling
//

//  This is a global variable accessible to zapi application code
int zctx_interrupted = 0;
static void s_signal_handler (int signal_value)
{
    zctx_interrupted = 1;
}


//  --------------------------------------------------------------------------
//  Constructor

zctx_t *
zctx_new (void)
{
    zctx_t
        *self;

    self = (zctx_t *) zmalloc (sizeof (zctx_t));
    self->sockets = zlist_new ();
    self->iothreads = 1;
    self->main = TRUE;

    //  Install signal handler for SIGINT and SIGTERM
    struct sigaction action;
    action.sa_handler = s_signal_handler;
    action.sa_flags = 0;
    sigemptyset (&action.sa_mask);
    sigaction (SIGINT, &action, NULL);
    sigaction (SIGTERM, &action, NULL);

    return self;
}


//  --------------------------------------------------------------------------
//  Destructor

void
zctx_destroy (zctx_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zctx_t *self = *self_p;
        while (zlist_size (self->sockets)) {
            void *socket = zlist_first (self->sockets);
            zmq_setsockopt (socket, ZMQ_LINGER, &self->linger, sizeof (int));
            zmq_close (socket);
            zlist_remove (self->sockets, socket);
        }
        zlist_destroy (&self->sockets);
        if (self->main && self->context)
            zmq_term (self->context);
        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Configure number of I/O threads in context, only has effect if called
//  before creating first socket. Default I/O threads is 1, sufficient for
//  all except very high volume applications.

void
zctx_set_iothreads (zctx_t *self, int iothreads)
{
    assert (self);
    self->iothreads = iothreads;
}


//  --------------------------------------------------------------------------
//  Configure linger timeout in msecs. Call this before destroying sockets or
//  context. Default is no linger, i.e. any pending messages or connects will
//  be lost.

void
zctx_set_linger (zctx_t *self, int linger)
{
    assert (self);
    self->linger = linger;
}


//  --------------------------------------------------------------------------
//  Create new 0MQ socket and register for this context. Use this instead of
//  the 0MQ core API method if you want automatic management of the socket
//  at shutdown. If the context has not yet been initialized, this method will
//  initialize it.

void *
zctx_socket_new (zctx_t *self, int type)
{
    assert (self);
    //  Initialize context now if necessary
    if (self->context == NULL)
        self->context = zmq_init (self->iothreads);
    assert (self->context);
    //  Create and register socket
    void *socket = zmq_socket (self->context, type);
    assert (socket);
    zlist_push (self->sockets, socket);
    return socket;
}


//  --------------------------------------------------------------------------
//  Destroy the socket. You must use this for any socket created via the
//  zctx_socket_new method.

void
zctx_socket_destroy (zctx_t *self, void *socket)
{
    assert (self);
    assert (socket);
    zmq_setsockopt (socket, ZMQ_LINGER, &self->linger, sizeof (int));
    zmq_close (socket);
    zlist_remove (self->sockets, socket);
}


//  --------------------------------------------------------------------------
//  Thread creation code, taken from ZFL's zfl_thread class and customized.

typedef struct {
    void *(*thread_fn) (void *);
    void *args;
#if defined (__WINDOWS__)
    HANDLE handle;
#endif
} shim_t;

#if defined (__UNIX__)
//  Thread shim for UNIX calls the real thread and cleans up afterwards.

void *
s_call_thread_fn (void *args)
{
    assert (args);
    shim_t *shim = (shim_t *) args;
    shim->thread_fn (shim->args);

    zthread_t *zthread = (zthread_t *) shim->args;
    zctx_destroy (&zthread->ctx);
    free (zthread);
    free (shim);
    return NULL;
}

#elif defined (__WINDOWS__)
//  Thread shim for Windows that wraps a POSIX-style thread handler
//  and does the _endthreadex for us automatically.

unsigned __stdcall
s_call_thread_fn (void *args)
{
    assert (args);
    shim_t *shim = (shim_t *) args;
    shim->thread_fn (shim->args);
    _endthreadex (0);
    CloseHandle (shim->handle);

    zthread_t *zthread = (zthread_t *) shim->args;
    zctx_destroy (&zthread->ctx);
    free (zthread);
    free (shim);
    return 0;
}
#endif


//  --------------------------------------------------------------------------
//  Create a child thread able to speak to this thread over inproc sockets.
//  The child thread receives a zthread_t structure as argument. Returns a
//  PAIR socket that is connected to the child thread. You can ignore the
//  socket if you don't need it.

void *
zctx_thread_new (zctx_t *self, void *(*thread_fn) (void *), void *arg)
{
    //  This is what we're going to pass to the child thread
    zthread_t *args = (zthread_t *) zmalloc (sizeof (zthread_t));
    args->arg = arg;
    args->pipe = zctx_socket_new (self, ZMQ_PAIR);
    args->ctx = (zctx_t *) zmalloc (sizeof (zctx_t));
    args->ctx->context = self->context;
    args->ctx->sockets = zlist_new ();

    //  Create pipe from our thread to child
    void *pipe = zctx_socket_new (self, ZMQ_PAIR);
    char endpoint [64];
    int rc = snprintf (endpoint, 64, "inproc://zctx-pipe-%p", pipe);
    assert (rc < 64);
    rc = zmq_bind (pipe, endpoint);
    assert (rc == 0);
    rc = zmq_connect (args->pipe, endpoint);
    assert (rc == 0);

    //  Now start child thread, passing our arguments
    shim_t *shim = (shim_t *) zmalloc (sizeof (shim_t));
    shim->thread_fn = thread_fn;
    shim->args = args;

#if defined (__UNIX__)
    pthread_t thread;
    pthread_create (&thread, NULL, s_call_thread_fn, shim);
    pthread_detach (thread);

#elif defined (__WINDOWS__)
    shim->handle = (HANDLE)_beginthreadex(
        NULL,                   //  Handle is private to this process
        0,                      //  Use a default stack size for new thread
        &s_call_thread_fn,      //  Start real thread function via this shim
        shim,                   //  Which gets the current object as argument
        CREATE_SUSPENDED,       //  Set thread priority before starting it
        NULL);                  //  We don't use the thread ID

    assert (shim->handle);
    //  Set child thread priority to same as current
    int priority = GetThreadPriority (GetCurrentThread ());
    SetThreadPriority (shim->handle, priority);
    //  Now start thread
    ResumeThread (shim->handle);
#endif

    return pipe;
}


//  --------------------------------------------------------------------------
//  Selftest

//  @selftest
static void *
s_test_thread (void *args_ptr)
{
    zthread_t *args = (zthread_t *) args_ptr;

    //  Create a socket to check it'll be properly deleted at exit
    zctx_socket_new (args->ctx, ZMQ_PUSH);

    //  Wait for our parent to ping us, and pong back
    char *ping = zstr_recv (args->pipe);
    free (ping);
    zstr_send (args->pipe, "pong");
    return NULL;
}

//  @end

int
zctx_test (Bool verbose)
{
    printf (" * zctx: ");

    //  @selftest
    //  Create and destroy a context without using it
    zctx_t *ctx = zctx_new ();
    assert (ctx);
    zctx_destroy (&ctx);
    assert (ctx == NULL);

    //  Create a context with many busy sockets, destroy it
    ctx = zctx_new ();
    zctx_set_iothreads (ctx, 1);
    zctx_set_linger (ctx, 5);       //  5 msecs
    void *s1 = zctx_socket_new (ctx, ZMQ_PAIR);
    void *s2 = zctx_socket_new (ctx, ZMQ_XREQ);
    void *s3 = zctx_socket_new (ctx, ZMQ_REQ);
    void *s4 = zctx_socket_new (ctx, ZMQ_REP);
    void *s5 = zctx_socket_new (ctx, ZMQ_PUB);
    void *s6 = zctx_socket_new (ctx, ZMQ_SUB);
    zmq_connect (s1, "tcp://127.0.0.1:5555");
    zmq_connect (s2, "tcp://127.0.0.1:5555");
    zmq_connect (s3, "tcp://127.0.0.1:5555");
    zmq_connect (s4, "tcp://127.0.0.1:5555");
    zmq_connect (s5, "tcp://127.0.0.1:5555");
    zmq_connect (s6, "tcp://127.0.0.1:5555");

    //  Create a child thread, check it's safely alive
    void *pipe = zctx_thread_new (ctx, s_test_thread, NULL);
    zstr_send (pipe, "ping");
    char *pong = zstr_recv (pipe);
    assert (streq (pong, "pong"));
    free (pong);

    //  Everything should be cleanly closed now
    zctx_destroy (&ctx);
    //  @end

    printf ("OK\n");
    return 0;
}
