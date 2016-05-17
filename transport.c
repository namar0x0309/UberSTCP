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

#define TCPHEADER_OFFSET    6
#define HEADER_LEN          sizeof( STCPHeader)
#define OPTIONS_LEN         40
#define CONGESTION_WIN_SIZE 3072
#define RECEIVER_WIN_SIZE   3072
#define SYN_REC_DATA        10
#define TIME_WAIT           4       // seconds
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
    unsigned int sender_win;
    
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
    int attempts;
    
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
        attempts = 0;
        
        while( ctx->connection_state != CSTATE_ESTABLISHED )  // loop where we wait for events
        {
            switch( ctx->connection_state )
            {
                // get syn and then send the syn_ack after.
                case CSTATE_WAIT_FOR_SYN:
                    if( attempts == 0 )
                    {
                        printf( "\nSYN?" );
                        wait_flags = 0 | NETWORK_DATA;
                        event = stcp_wait_for_event( sd, wait_flags, NULL ); // blocks; waits for network data
                    } 
                    else if( attempts == SYN_REC_DATA  )
                    {
                        errno = ECONNREFUSED; // we're receiving anything coherent.... 0.0
                        return;
                    } 
                    else
                        event = 0 | NETWORK_DATA;
                        
                    if( event & NETWORK_DATA )
                    {
                        printf( "\nSending syn-ack" );
                    
                        // header allocation
                        sgt_len = HEADER_LEN + OPTIONS_LEN;
                        sgt    = (char*)malloc( sgt_len * sizeof( char ) );      
                        assert( sgt );
                        
                        sgt_len = stcp_network_recv( sd, sgt, sgt_len );            // network fill the buffer
                        
                        rcv_h = (STCPHeader*)sgt;
                        
                        // SYN_ACK header
                        syn_ack_h = (STCPHeader*)calloc( 1, HEADER_LEN );
                        assert( syn_ack_h );
                        
                        // FILL HEADER
                        syn_ack_h->th_ack       = rcv_h->th_seq + 1;
                        syn_ack_h->th_seq       = ctx->initial_sequence_num;
                        syn_ack_h->th_flags     = 0 | TH_ACK | TH_SYN;
                        syn_ack_header->th_win  = CONGESTION_WIN_SIZE; 
                        syn_ack_header->th_off  = TCPHEADER_OFFSET - 1;
                        
                        // get initial sequence number
                        ctx->receiver_initial_seq_num = rcv_h->th_seq;
                        
                        printf( "\nSend SYN ACK" );
                        stcp_network_send( sd, syn_ack_h, HEADER_LEN, NULL );
                        ctx->connection_state = CSTATE_WAIT_FOR_ACK;
                        
                        rcv_h = NULL;
                        
                        if( sgt )
                        {
                            free( sgt );
                            sgt = NULL;
                        }
                        
                        if( syn_ack_h )
                        {
                            free( syn_ack_h );
                            syn_ack_h = NULL;
                        }
                    
                        ++attempts;
                    }
                    break;
                
                // after getting ack, continue in receiving data.
                case CSTATE_WAIT_FOR_ACK:
                    printf( "\nWhere's the ACK" );
                    
                    gettimeofday( &cur_time, NULL );
                    abs_time = (struct timespec* ) (&cur_time );
                    abs_time->tv_sec += TIME_WAIT; // wait for next packet
                    wait_flags = 0 | NETWORK_DATA;
                    
                    event = stcp_wait_for_event( ctx->sd, wait_flags, abs_time );
                    
                    if( event & NETWORK_DATA )
                    {
                        printf( "\nUnlocking the network data stream" );
                        
                        //allocation
                        sgt_len = HEADER_LEN + OPTIONS_LEN + STCP_MSS;
                        sgt    = (char*)malloc( sgt_len * sizeof( char ) );    // TODO: Circular Buffer  
                        assert( sgt );
                        
                        sgt_len = stcp_network_recv( sd, sgt, sgt_len );
                        
                        rcv_h = (STCPHeader*)sgt;
                        
                        app_data_len = get_size_of_app_data( sgt, sgt_len );
                        if( app_data_len >  0 )
                        {
                            printf( "\nReceived the ACK" );
                            handle_app_data( sgt, sgt_len, rcv_h , app_data_len );
                        }
                        
                        // update context
                        ctx->connection_state = CSTATE_ESTABLISHED;
                        rcv_h = NULL;
                        if( sgt )
                        {
                            free( sgt );
                            sgt =   NULL;
                        }
                        attempts = 0;
                    }
                    else
                        ctx->connection_state = CSTATE_WAIT_FOR_SYN;
                    break;
            }
        }
    }
    else // Active Connection
    {
        ctx->connection_state = CSTATE_SEND_SYN;
        attempts              = 0;
        
        while( ctx->connection_state != CSTATE_ESTABLISHED )
        {
            switch( ctx->connection_state )
            {
                case CSTATE_SEND_SYN:
                    if( attempts == SYN_REC_DATA  )
                    {
                        errno = ECONNREFUSED;
                        return;
                    }
                    
                    printf( "\nSending" );
                    syn_h = (STCPHeader*) calloc( 1, HEADER_LEN );
                    assert( syn_h );
                    
                    // syn header
                    syn_h->th_win   = RECEIVER_WIN_SIZE;
                    syn_h->th_flags = 0 | TH_SYN;
                    syn_h->th_seq   = ctx->initial_sequence_num;
                    syn_h->th_off   = TCPHEADER_OFFSET - 1;
                    
                    // send it
                    if( stcp_network_send( sd, syn_h, HEADER_LEN, NULL ) == -1  )
                        errno = ECONNREFUSED;
                    else
                        ctx->connection_state = CSTATE_WAIT_FOR_SYN_ACK;
                    
                    // deallocate variables
                    if( syn_h )
                    {
                        free( syn_h );
                        syn_h = NULL;
                    }
                    
                    printf( "\nSyn has been sent" );
                    ++attempts;
                break;
                
                case CSTATE_WAIT_FOR_SYN_ACK:
                    printf( "\nBlock until SYN-ACK received" );
                    
                    gettimeofday( &cur_time, NULL );
                    abs_time = (struct timespec* ) (&cur_time );
                    abs_time->tv_sec += TIME_WAIT; // wait for next packet
                    wait_flags = 0 | NETWORK_DATA;
                    
                    event = stcp_wait_for_event( ctx->sd, wait_flags, abs_time );
                    
                    if( event & NETWORK_DATA )
                    {
                        printf( "\nCooking the ACK" );
                        sgt_len = HEADER_LEN + OPTIONS_LEN;
                        sgt     = (char*)malloc( sgt_len * sizeof( char ) );
                        assert( sgt );
                        
                        sgt_len = stcp_network_recv( sd, sgt, sgt_len );
                       
                        // build ack header
                        ack_h = (STCPHeader*)calloc( 1, HEADER_LEN );
                        assert( ack_h );
                    
                        rcv_h = (STCPHeader*)sgt;
                            
                        // filling ack header
                        ack_h->th_off   = TCPHEADER_OFFSET - 1;
                        ack_h->th_ack   = rcv_h->th_seq + 1;
                        ack_h->th_seq   = ctx->initial_sequence_num;
                        ack_h->th_flags = 0 | TH_ACK; 
                        ack_h->th_win   = RECEIVER_WIN_SIZE;
                            
                        ctx->receiver_initial_seq_num = rcv_h->th_seq; // TODO: Update from received header
                        ctx->sender_win               = MIN( rcv_h->th_win, CONGESTION_WIN_SIZE );
                        
                        
                        if( stcp_network_send( sd, ack_h, HEADER_LEN, NULL ) == -1 )
                            errno = ECONNREFUSED;
                    
                        if( rcv_h->th_flags & TH_ACK )
                            ctx->connection_state = CSTATE_ESTABLISHED;
                            
                        // free up allocations
                        rcv_h = NULL;
                        if( ack_h )
                        {
                            free( ack_h );
                            ack_h = NULL;
                        }
                        
                        if( sgt )
                        {
                            free( sgt );
                            sgt = NULL;
                        }
                        attempts = 0;
                        printf( "\nWe have sent the ACK" );
                    } 
                    else
                        ctx->connection_state = CSTATE_SEND_SYN;
                break;
                
                default: 
                    printf("\nDefault case" ); 
                    break;
            } // Switch
        }// While loop
    } // else - (Active Connection)
    
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

        event = stcp_wait_for_event(sd, 0, NULL);

        if( event & TIMEOUT )
            continue;

        /* check whether it was the network, app, or a close request */
        if (event & APP_DATA) // Eliza
        {
            /* the application has requested that data be sent */
            /* see stcp_app_recv() */
        }
        
         if (event & NETWORK_DATA) // Kelly
        {
            if( header->th_flags * TH_FIN )
            {
                transport_close(); 
            }
        }

         if (event & APP_CLOSE_REQUESTED ) // Nassim
        {
            transport_close();
        }

        // FREE UP MEMORY
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



/************* TIMER ******************/
void timerOn() // TODO: refactor
{
    struct sigaction sa;

    /* set the signal handler */
    memset(&sa, 0, sizeof(sa));
    //sa.sa_handler = resend;  // Implement if needed (NASSIM)
    sigaction(SIGALRM, &sa, NULL);

    /* set the alarm */
    ctx->timer_running = TRUE;
    alarm(TIMEOUT_INTERVAL);
    printf("\nTimer started");
}

void timerOff() // TODO: refactor
{
    /* switch off the alarm */
    ctx->timer_running        = FALSE;
    ctx->retransmission_count = 0;
    alarm(0);
    printf("\nTimer stopped");
}

size_t appDataSize(char *segment, ssize_t segment_len) // TODO: refactor
{
    size_t app_data_len;

    assert(segment);

    if(TCP_OPTIONS_LEN(segment) == 0)
        app_data_len = segment_len - HEADER_SIZE;
    else
        app_data_len = segment_len - (HEADER_SIZE + TCP_OPTIONS_LEN(segment));

    return app_data_len;
}

void appDataProcess(char *segment, ssize_t segment_len, STCPHeader *header, size_t app_data_len) // TODO: refactor
{
    size_t data_offset;
    char *app_data;

    /* if the sequence number of the arrived segment is the expected sequence number */
    /* then extract the data within the receive window and deliver it to the application */
    /* update the window according to the value of the ACK field */
    if(header->th_seq == ctx->expected_sequence_num)
    {
        printf("\nIf sequence number received is the expected sequence number");

        /* extract app_data */
        data_offset = 0;
        app_data = dataGetFromSegment(segment, data_offset, app_data_len);
        printf("\nApplication data extracted from the segment");

        /* buffer whatever has been received */
        app_data_len = bufferReceiveData(ctx->expected_sequence_num_ptr, app_data, app_data_len);
        printf("\nReceived data buffered");

        /* deliver max possible data to the application */
        app_data_len = dataDeliverToApplication();
        printf("\nReceived data delivered to application");

        /* update the STCP state variables */
        ctx->expected_sequence_num_ptr = (ctx->expected_sequence_num_ptr + app_data_len) % WINDOW_SIZE;
        ctx->expected_sequence_num    += app_data_len;
        ctx->recv_window_size         += app_data_len;
        if(ctx->recv_window_size > WINDOW_SIZE)
            ctx->recv_window_size = WINDOW_SIZE;
        printf("\nSTCP state variables updated");

        /* send the ACK */
        headerSend(ctx->next_sequence_num);
        printf("\nACK sent");
    }
}

void transport_close() // Nassim

{
	
	
}

void bufferSendData(char *app_data, size_t app_data_len) // TODO: refactor
{
    size_t next_seq_num_ptr, i, j;

    next_seq_num_ptr = (ctx->send_base_ptr + (ctx->next_sequence_num - ctx->send_base)) % WINDOW_SIZE;

    for(i = next_seq_num_ptr, j = 0; j < app_data_len; i = (i + 1) % WINDOW_SIZE, j++)
        ctx->send_window[i] = app_data[j];
}

size_t bufferReceiveData(size_t start, char *app_data, size_t app_data_len) // TODO: refactor
{
    size_t i, j, bytes_delivered;

    start           = start % WINDOW_SIZE;
    bytes_delivered = 0;
    assert(app_data);

    printf("\n%u bytes to be buffered", app_data_len);
    for(i = start, j = 0; j < app_data_len; i = (i + 1) % WINDOW_SIZE, j++)
    {
        if(ctx->recv_window_lookup[i] == 0)
        {
            //printf("\nByte with window seq number %u has been buffered", i);
            ctx->recv_window[i]        = app_data[j];
            ctx->recv_window_lookup[i] = 1;
            bytes_delivered++;
        }
    }
    printf("\n%u bytes have been buffered", bytes_delivered);
    return bytes_delivered;
}


size_t windowSize() // TODO: refactor
{
    size_t curr_send_window_left;

    /* if the sequence number space has wrapped around */
    if(MAX_SEQUENCE_NUMBER - ctx->send_base < WINDOW_SIZE)
    {
        printf("\nWrap around!");
        /* if the send_base and next_sequence_number have not wrapped around */
        if(ctx->next_sequence_num > ctx->send_base)
            curr_send_window_left = (MAX_SEQUENCE_NUMBER - ctx->next_sequence_num) + (WINDOW_SIZE - (MAX_SEQUENCE_NUMBER - ctx->send_base));
        /* if the next_sequence_number has wrapped around */
        else
            curr_send_window_left = MAX_SEQUENCE_NUMBER - ctx->send_base + (ctx->next_sequence_num + 1);
    }
    /* no wrap around */
    else
    {
        printf("\nNo wrap around");
        curr_send_window_left = ctx->send_base + WINDOW_SIZE - ctx->next_sequence_num;
    }

    return curr_send_window_left;
}

void headerSend(tcp_seq seq_num) // TODO: refactor
{
    /* construct the header */
    STCPHeader *new_header = NULL;
    ssize_t bytes_sent;

    new_header = construct_header(seq_num);

    /* send ACK */
    do
    {
        bytes_sent = stcp_network_send(ctx->sd, new_header, HEADER_SIZE, NULL);
    }while(bytes_sent == -1);
    printf("\nACK %d sent to network layer", new_header->th_ack);

    /* free up memory */
    if(new_header)
    {
        free(new_header);
        new_header = NULL;
    }
}

size_t dataDeliverToApplication() // TODO: refactor
{
    size_t i, j, app_data_len;
    char *app_data;

    /* calculate the number of bytes that can be delivered */
    app_data_len = 0;
    i            = ctx->expected_sequence_num_ptr;
    while(ctx->recv_window_lookup[i] == 1 && app_data_len < WINDOW_SIZE)
    {
        i = (i + 1) % WINDOW_SIZE;
        app_data_len++;
    }

    /* create a buffer that can be used to deliver the data to application */
    app_data = (char *) malloc(app_data_len * sizeof(char));
    assert(app_data);

    /* store the data to be delivered in app_data */
    /* update the recv_buffer_lookup table */
    for(i = ctx->expected_sequence_num_ptr, j = 0; j < app_data_len; i = (i + 1) % WINDOW_SIZE, j++)
    {
        app_data[j]                = ctx->recv_window[i];
        ctx->recv_window_lookup[i] = 0;
    }

    /* deliver data to the application */
    stcp_app_send(ctx->sd, app_data, app_data_len);

    /* free up memory */
    if(app_data)
    {
        free(app_data);
        app_data = NULL;
    }
    printf("\n%u bytes delivered to application", app_data_len);
    return app_data_len;
}

char *dataGetFromSegment(char *segment, size_t data_offset, size_t app_data_len) // TODO: refactor
{
    size_t data_start_point;
    char *app_data;

    assert(segment);

    /* allocate memory to store the extracted application data */
    app_data = (char *) malloc(app_data_len * sizeof(char));
    assert(app_data);

    /* calculate the point in segment where to start the extraction from */
    data_start_point = TCP_DATA_START(segment) + data_offset;

    /* copy the application data from segment to app_data */
    memcpy(app_data, segment + data_start_point, app_data_len);

    printf("\nData extracted from byte number: %u", data_start_point);

    return app_data;
}