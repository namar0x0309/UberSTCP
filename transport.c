/*
 * transport.c 
 *
 * COS461: Assignment 3 (STCP)
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file. 
 *
 */


#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"

#define TCPHEADER_OFFSET 6

/*
SYN-RECEIVED STATE
      FIN-WAIT-1 STATE
      FIN-WAIT-2 STATE
      CLOSE-WAIT STATE
      LAST-ACK STATE
      TIME-WAIT STATE
*/
enum { 
    CSTATE_SEND_SYN,
    CSTATE_WAIT_FOR_SYN,
    CSTATE_WAIT_FOR_SYN_ACK,
    CSTATE_WAIT_FOR_ACK,
    CSTATE_ESTABLISHED,
    CSTATE_SEND_FIN,
    CSTATE_FIN_RECVD,
    CSTATE_WAIT_FOR_FIN,
    CSTATE_CLOSED 
};    /* you should have more states */


/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done;    /* TRUE once connection is closed */

    int connection_state;   /* state of the connection (established, etc.) */
    tcp_seq initial_sequence_num;

    /* any other connection-wide global variables go here */
    mysocket_t sd;
    
} context_t;


static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);


/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    unsigned int event, wait_flags;
    STCPHeader *recv_h, *syn_h, *syn_ack_h, *ack_h;
    char *segt, *app_data;
    ssize_t sgt_len; 
    size_t app_data_len;
    struct timespec *abs_time
    struct timeval  cur_time;
    int count;
    
    context_t *ctx;

    ctx = (context_t    *) calloc(1, sizeof(context_t));
    assert(ctx);

    generate_initial_seq_num(ctx);
    ctx->sd = sd;
    
    app_data = sgt = NULL;
    recv_h = syn_h = syn_ack_h = ack_h = NULL;
    

    /* XXX: you should send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */

    if( !is_active )
    {
        ctx->connection_state = CSTATE_WAIT_FOR_SYN;
        count = 0;
        
        while( ctx->connection_state != CSTATE_ESTABLISHED )  // loop where we wait for events
        {
            switch( ctx->connection_state )
            {
                case CSTATE_WAIT_FOR_SYN:
                    if( count == 0 )
                    {
                        printf( "\nSYN?" );
                        wait_flags = 0 | NETWORK_DATA;
                        event = stcp_wait_for_event( sd, wait_flags, NULL ); // blocks; waits for network data
                    } 
                    else if( count == TCPHEADER_OFFSET )
                    {
                        errno = ECONNREFUSED; // we're receiving anything coherent.... 0.0
                        return;
                    } 
                    else
                        event = 0 | NETWORK_DATA;
                        
                    if( event & NETWORK_DATA )
                    {
                        
                        
                    }
                    break;
                
            }
        }
    }
    else // Active Connection
    {
        
        
    }
    
    // after loop
    ctx->connection_state = CSTATE_ESTABLISHED;
    stcp_unblock_application(sd);
    control_loop(sd, ctx);

    /* do any cleanup here */
    free(ctx);
}


/* generate random initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);

#ifdef FIXED_INITNUM
    /* please don't change this! */
    ctx->initial_sequence_num = 1;
#else
    /* you have to fill this up */
    /*ctx->initial_sequence_num =;*/
#endif
}


/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx)
{
    assert(ctx);
    assert(!ctx->done);

    while (!ctx->done)
    {
        unsigned int event;

        /* see stcp_api.h or stcp_api.c for details of this function */
        /* XXX: you will need to change some of these arguments! */

        // We either ask the api,
        // (1) is there a read to handle?,
        // (2) is there a write to handle?,
        // (3) is there a connection shutdown to handle? or
        // (4) is there any event to handle?
        event = stcp_wait_for_event(sd, 0, NULL);


        /* check whether it was the network, app, or a close request */
        if (event & APP_DATA)
        {
            /* the application has requested that data be sent */
            /* see stcp_app_recv() */
        }

        /* etc. */
    }
}


/**********************************************************************/
/* our_dprintf
 *
 * Send a formatted message to stdout.
 * 
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void our_dprintf(const char *format,...)
{
    va_list argptr;
    char buffer[1024];

    assert(format);
    va_start(argptr, format);
    vsnprintf(buffer, sizeof(buffer), format, argptr);
    va_end(argptr);
    fputs(buffer, stdout);
    fflush(stdout);
}



