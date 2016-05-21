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
#include <sys/time.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"

#define TCPHEADER_OFFSET    5
#define HEADER_LEN          sizeof( STCPHeader)
#define OPTIONS_LEN         40
#define MAX_SEQUENCE_NUMBER 4294967296 /* this is 2^32 */ // REFACTOR
#define CONGESTION_WIN_SIZE 3072
#define RECEIVER_WIN_SIZE   3072
#define SENDER_WIN_SIZE   3072
#define MSS_LEN				536
#define SYN_REC_DATA        10
#define WAIT_TIME           4       // seconds


// RFC 793 [Page 21]
enum {
    LISTEN,         // Waiting for a cxn request from any remote TCP & port
    SYN_SENT,       // Waiting for a matching cxn request after having sent a cxn request
    SYN_RECEIVED,   // Waiting for a confirming cxn request ack after having both received and sent a cxn request
    ESTABLISHED,    // An open cnx; data received can be delivered to the user
    FIN_WAIT_1,     // Waiting for a cnx termination request from the remote TCP, or ACK of the cxn termination request previously sent
    FIN_WAIT_2,     // Waiting for a cnx termination request from the remote TCP
    CLOSE_WAIT,     // Waiting for a cnx termination request from the local user
    CLOSING,        // Waiting for a cnx termination request ACK from the remote TCP
    LAST_ACK,       // Waiting for an acknowledgment of the cnx termination request previously sent to the remote TCP
    TIME_WAIT,      // Waiting for enough time to pass to be sure the remote TCP received the ACK of its cxn termination request
    CLOSED,         // No connection
    CSTATE_SEND_SYN,
    CSTATE_WAIT_FOR_SYN,
    CSTATE_WAIT_FOR_SYN_ACK,
    CSTATE_WAIT_FOR_ACK,
    //CSTATE_SEND_FIN,        /* same as FIN-WAIT-1? */
    //CSTATE_FIN_RECVD,       /* same as CLOSING? */
    //CSTATE_WAIT_FOR_FIN,    /* same as FIN-WAIT-2? */
    //CSTATE_CLOSED
}; 


/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done;    /* TRUE once connection is closed */

    int connection_state;   /* state of the connection (established, etc.) */

    tcp_seq receiver_initial_seq_num;

    /* any other connection-wide global variables go here */
    mysocket_t sd;

    /* Send Sequence Variables; RFC 793 [Page 25] */
    tcp_seq snd_una;        // oldest unacknowledged sequence number
    tcp_seq snd_nxt;        // next sequence number to be sent
    unsigned int snd_wnd;   // send window
    tcp_seq iss;

    /* Receive Sequence Variables RFC 793 [Page 25] */
    tcp_seq rcv_nxt;        // next sequence number expected on an incoming segment, and is the left or lower edge of the receive window
    unsigned int rcv_wnd;   // receive window
    tcp_seq irs;            // initial receive sequence number
		
		// REfactor
    tcp_seq expected_sequence_num_ptr;   /* pointer to the recv_window corresponding to the receive window */
    int recvWindowLookup[RECEIVER_WIN_SIZE]; 
    tcp_seq send_base;                   /* first unACKed sequence number */
    tcp_seq send_base_ptr;               /* pointer to the send_window corresponding to the send window */
    tcp_seq expected_sequence_num;       /* expected sequence number from the sender or other host */
    char send_window[SENDER_WIN_SIZE];       /* send buffer of the host */
    char recv_window[RECEIVER_WIN_SIZE];       /* receive buffer of the host */
    tcp_seq recv_window_size;
  
} context_t;

// TODO: Camelify the source

static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);
static void receiveNetworkSegment(mysocket_t sd, context_t *ctx);
void sendAppData(mysocket_t sd, context_t *ctx);
STCPHeader *constructHeader(tcp_seq seq_num);
void headerSend(tcp_seq seq_num);
void transport_close();
size_t dataDeliverToApplication(); // TODO: refactor
char *dataGetFromSegment(char *segment, size_t data_offset, size_t app_data_len); // TODO: refactor
size_t bufferReceiveData(size_t start, char *app_data, size_t app_data_len); // TODO: refactor

static context_t *ctx;

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    fflush(stdout);
  
    printf( "\n%s", __FUNCTION__ );

    STCPHeader *syn_h;
    context_t *ctx;

    ctx = (context_t    *) calloc(1, sizeof(context_t));
    assert(ctx);

    syn_h = NULL;

    generate_initial_seq_num(ctx);
    ctx->sd = sd;
    ctx->rcv_wnd = RECEIVER_WIN_SIZE;

    /* XXX: you should send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */

    // Received OPEN call from application; RFC 793 [Page 54]
    if (is_active) {
        printf("\nActive: sending SYN");
        ctx->connection_state = CLOSED;

        // Set up SYN header
        syn_h = (STCPHeader*) calloc( 1, HEADER_LEN );
        assert( syn_h );
        syn_h->th_seq   = ctx->iss;
        syn_h->th_flags = 0 | TH_SYN;
        syn_h->th_win   = RECEIVER_WIN_SIZE;
        syn_h->th_off   = TCPHEADER_OFFSET;

        // Send the SYN
        if( stcp_network_send( sd, syn_h, HEADER_LEN, NULL ) == -1  )
            errno = ECONNREFUSED;
        else {
            ctx->snd_una = ctx->iss;
            ctx->snd_nxt = ctx->iss + 1;
            ctx->connection_state = SYN_SENT;
        }

        free(syn_h);

        // Enter the control loop to wait for SYNACK
        printf( "\nActive: SYN has been sent. Entering the control loop to wait for SYNACK" );
        control_loop(sd, ctx);

    // Connection inactive; enter the control loop to wait for SYN
    } else {
        printf("\nPassive connection; entering the control loop to wait for SYN");
        ctx->connection_state = LISTEN;
        control_loop(sd, ctx);
    }

    /* do any cleanup here */
    free(ctx);
}


/* generate random initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);

#ifdef FIXED_INITNUM
    /* please don't change this! */
    ctx->iss = 1;
#else
    /* you have to fill this up */
    /*ctx->iss =;*/
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

     while (!ctx->done) /* while ESTABLISHED */
     {
         unsigned int event;

         /* see stcp_api.h or stcp_api.c for details of this function */
         /* XXX: you will need to change some of these arguments! */

         event = stcp_wait_for_event(sd, ANY_EVENT, NULL);

         if( event & TIMEOUT ) /* should never get here */
             continue;

         /* check whether it was the network, app, or a close request */
         if (event & APP_DATA) // Eliza
         {
             /* the application has requested that data be sent */
             printf("\nApp data available to send");
             sendAppData(sd, ctx);
         }

          if (event & NETWORK_DATA) // Kelly
         {
             printf("\nNetwork data available to receive");
             receiveNetworkSegment(sd, ctx);

         }

          if (event & APP_CLOSE_REQUESTED ) // Nassim
         {
             transport_close();
         }

         // TODO: FREE UP MEMORY
     }
 }

// /****************************** Helper Functions ****************************************/
// /* our_dprintf
 // *
 // * Send a formatted message to stdout.
 // * 
 // * format               A printf-style format string.
 // *
 // * This function is equivalent to a printf, but may be
 // * changed to log errors to a file if desired.
 // *
 // * Calls to this function are generated by the dprintf amd
 // * dperror macros in transport.h
 // */
// void our_dprintf(const char *format,...)
// { 
    // va_list argptr;
    // char buffer[1024];

    // assert(format);
    // va_start(argptr, format);
    // vsnprintf(buffer, sizeof(buffer), format, argptr);
    // va_end(argptr);
    // fputs(buffer, stdout);
    // fflush(stdout);
// }



// /************* TIMER ******************/
// void timerOn() // TODO: refactor
// {
    // struct sigaction sa;

    // /* set the signal handler */
    // memset(&sa, 0, sizeof(sa));
    // //sa.sa_handler = resend;  // Implement if needed (NASSIM)
    // sigaction(SIGALRM, &sa, NULL);

    // /* set the alarm */
    // ctx->timer_running = TRUE;
    // alarm(TIMEOUT_INTERVAL);
    // printf("\nTimer started");
// }

// void timerOff() // TODO: refactor
// {
    // /* switch off the alarm */
    // ctx->timer_running        = FALSE;
    // ctx->retransmission_count = 0;
    // alarm(0);
    // printf("\nTimer stopped");
// }

size_t appDataSize(char *segment, ssize_t segment_len) // TODO: refactor
{
    size_t app_data_len;

    assert(segment);

    if(TCP_OPTIONS_LEN(segment) == 0)
        app_data_len = segment_len - HEADER_LEN;
    else
        app_data_len = segment_len - (HEADER_LEN + TCP_OPTIONS_LEN(segment));

    return app_data_len;
}

//
//void appDataProcess(char *segment, ssize_t segment_len, STCPHeader *header, size_t app_data_len) // TODO: refactor
//{
//    size_t data_offset;
//    char *app_data;
//
//    /* if the sequence number of the arrived segment is the expected sequence number */
//    /* then extract the data within the receive window and deliver it to the application */
//    /* update the window according to the value of the ACK field */
//    if(header->th_seq == ctx->expected_sequence_num)
//    {
//        printf("\nIf sequence number received is the expected sequence number");
//
//        /* extract app_data */
//        data_offset = 0;
//        app_data = dataGetFromSegment(segment, data_offset, app_data_len);
//        printf("\nApplication data extracted from the segment");
//
//        /* buffer whatever has been received */
//        app_data_len = bufferReceiveData(ctx->expected_sequence_num_ptr, app_data, app_data_len);
//        printf("\nReceived data buffered");
//
//        /* deliver max possible data to the application */
//        app_data_len = dataDeliverToApplication();
//        printf("\nReceived data delivered to application");
//
//        /* update the STCP state variables */
//        ctx->expected_sequence_num_ptr = (ctx->expected_sequence_num_ptr + app_data_len) % RECEIVER_WIN_SIZE;
//        ctx->expected_sequence_num    += app_data_len;
//        ctx->recv_window_size         += app_data_len;
//        if(ctx->recv_window_size > RECEIVER_WIN_SIZE)
//            ctx->recv_window_size = RECEIVER_WIN_SIZE;
//        printf("\nSTCP state variables updated");
//
//        /* send the ACK */
//        headerSend(ctx->rcv_nxt);
//        printf("\nACK sent");
//    }
//}
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
        ctx->expected_sequence_num_ptr = (ctx->expected_sequence_num_ptr + app_data_len) % RECEIVER_WIN_SIZE;
        ctx->expected_sequence_num    += app_data_len;
        ctx->recv_window_size         += app_data_len;
        if(ctx->recv_window_size > RECEIVER_WIN_SIZE)
            ctx->recv_window_size = RECEIVER_WIN_SIZE;
        printf("\nSTCP state variables updated");

        /* send the ACK */
        headerSend(ctx->rcv_nxt);
        printf("\nACK sent");
    }
}

// /**************************** Event Handlers *********************************************/


 /* Process application data */
 /*todo: Sequence numbers need mod 2^32 arithmetic;  */
 void sendAppData(mysocket_t sd, context_t *ctx)
 {
	 size_t grabbed_bytes; /* how many bytes actually read from application */
	 ssize_t passed_bytes; /* how many bytes were able to send to network */
	 char *sgt, *data;
	 size_t data_len;
	 STCPHeader *snd_h;
    
	 /* send data only if sender window is not full */
	 ssize_t send_capacity = ctx->snd_una + ctx->snd_wnd - 1 - ctx->snd_nxt;  /* rfc 793 [p. 83]*/
	 if( send_capacity > 0 ){
		 /* adjust max amount of data we can send*/
		 if (send_capacity > MSS_LEN) { send_capacity = MSS_LEN;}
     

		 /* allocate space for header and data */
		 sgt = (char *) malloc((send_capacity + HEADER_LEN) * sizeof(char));  /* why not permaently allocate 1-MSS buffer globally and reuse it? */
		 data = sgt + HEADER_LEN;
		 data_len = send_capacity;
		
         /* get data*/
		 grabbed_bytes = stcp_app_recv(sd, data, data_len);
         printf("\nData accepted from application: %d bytes", grabbed_bytes);
		
		 /* build header */
		 snd_h = (STCPHeader *)sgt;
		 memset(snd_h, 0, HEADER_LEN);
		 snd_h->th_seq = ctx->snd_nxt;
		 snd_h->th_win  = CONGESTION_WIN_SIZE;
		 snd_h->th_off  = HEADER_LEN;

		 /* push both header and data to network */
		 passed_bytes = stcp_network_send(sd, sgt, grabbed_bytes + HEADER_LEN, NULL);
		 if (passed_bytes < 0 ) { 
			 /*todo: error? or retry?ow namy times to retry? */
		 }
		 /*update next sequence number */
		 ctx->snd_nxt += grabbed_bytes;
		
		 /*free memory*/
		 free(sgt);
	 }
 }

// /* Process a segment received from the network */
 void receiveNetworkSegment(mysocket_t sd, context_t *ctx)
 {
     STCPHeader *rcv_h, *syn_ack_h;
     char *seg;
     ssize_t seg_len_incl_hdr;

     // Current Segment Variables; RFC 793 [Page 25]
     tcp_seq seg_seq;    // first sequence number of a segment
     tcp_seq seg_ack;    // acknowledgment from the receiving TCP (next sequence number expected by the receiving TCP)
     ssize_t seg_len;    // the number of octets occupied by the data in the segment (counting SYN and FIN)
     ssize_t seg_wnd;    // segment window

     /* Allocation */
     seg_len_incl_hdr = STCP_MSS;
     seg = (char*)malloc( seg_len_incl_hdr * sizeof( char ) );
     assert( seg );

     /* Receive the segment and extract the header from it */
     seg_len_incl_hdr = stcp_network_recv( sd, seg, seg_len_incl_hdr );
     rcv_h = (STCPHeader*)seg;
     printf("\nSegment received");

     /* Extract info from received header */
     seg_len = seg_len_incl_hdr - rcv_h->th_off;
     seg_seq = rcv_h->th_seq;
     seg_ack = rcv_h->th_ack;
     seg_wnd = rcv_h->th_win;
     ctx->snd_wnd = MIN( rcv_h->th_win, CONGESTION_WIN_SIZE ); // TODO: This shouldn't go here in case it's a bad packet

     // RFC 793 [Page 65]
     if (ctx->connection_state == LISTEN) {

         // Check for a SYN; Send SYNACK if received
         if (rcv_h->th_flags & TH_SYN) {
             printf("\nPassive: received SYN");

             // Update context fields
             ctx->rcv_nxt = seg_seq + 1;
             ctx->irs = seg_seq;

             // Set up the SYNACK
             syn_ack_h = (STCPHeader *) calloc(1, HEADER_LEN);
             assert(syn_ack_h);
             syn_ack_h->th_seq = ctx->iss;
             syn_ack_h->th_ack = ctx->rcv_nxt;
             syn_ack_h->th_flags = 0 | TH_ACK | TH_SYN;
             syn_ack_h->th_win = ctx->rcv_wnd;
             syn_ack_h->th_off = TCPHEADER_OFFSET;

             // Send the SYNACK
             printf("\nSending SYNACK");
             stcp_network_send( sd, syn_ack_h, HEADER_LEN, NULL );
             free(syn_ack_h);

             // Update sequence numbers and connection state
             ctx->snd_nxt = ctx->iss + 1;
             ctx->snd_una = ctx->iss;
             ctx->connection_state = SYN_RECEIVED;

         // Anything other than a SYN in the LISTEN state should be ignored in STCP
         } else {
             free(seg);
             return;
         }

     // RFC 793 [Page 66]
     } else if (ctx->connection_state = SYN_SENT) {

         // If this is an ACK
         if (rcv_h->th_flags & TH_ACK) {

             // Ignore anything with an ack number outside the send window
             if (seg_ack <= ctx->iss || seg_ack > ctx->snd_nxt || seg_ack < ctx->snd_una) {
                 free(seg);
                 return;
             }
         }

         // If this is a SYN (Simultaneous Connection)
         if (rcv_h->th_flags & TH_SYN) {
             ctx->rcv_nxt = seg_seq + 1;
             ctx->irs = seg_seq;

             if (seg_ack > ctx->snd_una) ctx->snd_una = seg_ack;

             // If our SYN has been ACKed, enter ESTABLISHED state
             if (ctx->snd_una > ctx->iss) {
                 ctx->connection_state = ESTABLISHED;
                 ctx->snd_wnd = seg_wnd;
                 stcp_unblock_application(sd);

             // Otherwise, enter SYN_RECEIVED and send SYNACK
             } else {

                 // Set up the SYNACK
                 syn_ack_h = (STCPHeader *) calloc(1, HEADER_LEN);
                 assert(syn_ack_h);
                 syn_ack_h->th_seq = ctx->iss;
                 syn_ack_h->th_ack = ctx->rcv_nxt;
                 syn_ack_h->th_flags = 0 | TH_ACK | TH_SYN;
                 syn_ack_h->th_win = ctx->rcv_wnd;
                 syn_ack_h->th_off = TCPHEADER_OFFSET;

                 // Send the SYNACK
                 printf("\nSending SYNACK");
                 stcp_network_send( sd, syn_ack_h, HEADER_LEN, NULL );
                 free(syn_ack_h);

                 // Update the connection state (already set snd_next and snd_una when we sent the SYN)
                 ctx->connection_state = SYN_RECEIVED;
             }

         }

     // SYN-RECEIVED, ESTABLISHED, FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT, CLOSING, LAST-ACK, or TIME-WAIT state
     // RFC 793 [Page 69]
     } else {

         /* Check sequence number
          * If the segment contains data that comes after the next byte we're expecting,
          * send an ACK for the next expected byte and drop the packet (by returning) */
         if (seg_len > 0 && seg_seq > ctx->rcv_nxt) {
             headerSend(
                     ctx->rcv_nxt);   // TODO: make sure headerSend() sends an ACK like the code implies
             free(seg);
             return;
         }

         /* Trim off any portion of the data that we've already received */
         if (seg_seq < ctx->rcv_nxt) {
             rcv_h->th_off -= (ctx->rcv_nxt - seg_seq);
             seg_len -= (ctx->rcv_nxt - seg_seq);
             seg_seq = ctx->rcv_nxt;
         }

         // If this is a SYN, ignore the packet; RFC 793 [Page 71]
         if (rcv_h->th_flags & TH_SYN) {
             free(seg);
             return;
         }

         /* Check the ACK field; RFC 793 [Page 72] */
         if (rcv_h->th_flags & TH_ACK) {
             seg_ack = rcv_h->th_ack;
             printf("\nProcessing ACK %u", seg_ack);

             // If in the SYN-RECEIVED state
             if (ctx->connection_state == SYN_RECEIVED) {

                 // If the ack is within the send window, enter ESTABLISHED state
                 if (ctx->snd_una <= seg_ack && seg_ack <= ctx->snd_nxt) {
                     ctx->connection_state = ESTABLISHED;
                     ctx->snd_wnd = seg_wnd;
                     stcp_unblock_application(sd);

                 // If the ACK is not acceptable, drop the packet and ignore
                 } else {
                     free(seg);
                     return;
                 }

             // If in the ESTABLISHED state
             } else if (ctx->connection_state == ESTABLISHED) {

                 // If the ACK is within the send window, update the last unacknowledged byte and send window
                 if (ctx->snd_una < seg_ack && seg_ack <= ctx->snd_nxt) {
                     printf("\nThe ACK is within the send window");
                     ctx->snd_una = seg_ack;
                     ctx->snd_wnd = rcv_h->th_win;

                 // If it's a duplicate of the most recent ACK, just update the send window
                 } else if (ctx->snd_una = seg_ack) {
                     ctx->snd_wnd = rcv_h->th_win;
                 }

             // If in FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT, CLOSING, LAST-ACK, or TIME-WAIT
             } else {
                 // TODO: Handle these states
                 free(seg);
                 return;
             }
             // TODO: stop the timer if it is running and start it if there are unACKed segments (I'm hoping someone else has already gotten a timer set up, otherwise I'll take care of it)

             printf("\nDone processing ACK");
         }

         /* Process the segment text; RFC 793 [Page 74] */ // TODO: Handle according to state
         if (seg_len > 0) {
             printf("\nHandling received data beginning at sequence number %u,",
                    seg_seq);
             /* TODO: See if Nassim has the handle_app_data() code available; otherwise
              * I'll write something to pass the data to the application */

             /* We've now taken responsibility for delivering the data to the user, so
              * we ACK receipt of the data and advance rcv_nxt over the data accepted */
             ctx->rcv_nxt += seg_len;
             headerSend(
                     ctx->rcv_nxt);   // TODO: make sure headerSend() sends an ACK like the code implies
         }

         if (rcv_h->th_flags *
             TH_FIN)  // TODO: See if I need to handle any packets coming in during the FIN sequence
         {
             transport_close();
         }
     }
     free(seg);
 }

void transport_close() // Nassim
{
  printf( "\nNassim get on this! 0.0" );
	
}

/* Process application data */
 /*todo: Sequence numbers need mod 2^32 arithmetic;  */
 void send_app_data(mysocket_t sd, context_t *ctx)
 {
	 size_t grabbed_bytes; /* how many bytes actually read from application */
	 ssize_t passed_bytes; /* how many bytes were able to send to network */
	 char *sgt, *data;
	 size_t data_len;
	 STCPHeader *snd_h;
    
	 /* send data only if sender window is not full */
	 ssize_t send_capacity = ctx->snd_una + ctx->snd_wnd - 1 - ctx->snd_nxt;  /* rfc 793 [p. 83]*/
	 if( send_capacity > 0 ){
		 /* adjust max amount of data we can send*/
		 if (send_capacity > MSS_LEN) { send_capacity = MSS_LEN;}
		
		 /* allocate space for header and data */
		 sgt = (char *) malloc(send_capacity * sizeof(char));  /* why not permaently allocate 1-MSS buffer globally and reuse it? */
		 data = sgt + HEADER_LEN;
		 data_len = send_capacity - HEADER_LEN;
		
         /* get data*/
		 grabbed_bytes = stcp_app_recv(sd, data, data_len);
         printf("\nData accepted from application: %d bytes", grabbed_bytes);
		
		 /* build header */
		 snd_h = (STCPHeader *)sgt;
		 memset(snd_h, 0, HEADER_LEN);
		 snd_h->th_seq = ctx->snd_nxt;
		 snd_h->th_win  = CONGESTION_WIN_SIZE;
		 snd_h->th_off  = HEADER_LEN;

		 /* push both header and data to network */
		 passed_bytes = stcp_network_send(sd, sgt, grabbed_bytes + HEADER_LEN, NULL);
		 if (passed_bytes < 0 ) { 
			 /*todo: error? or retry?ow namy times to retry? */
		 }
		 /*update next sequence number */
		 ctx->snd_nxt += grabbed_bytes;
		
		 /*free memory*/
		 free(sgt);
	 }
 }



// /***************************** More Helper Funcitons ****************************************/

 void bufferSendData(char *app_data, size_t app_data_len) // TODO: refactor
 {
     size_t next_seq_num_ptr, i, j;

     next_seq_num_ptr = (ctx->send_base_ptr + (ctx->rcv_nxt - ctx->send_base)) % RECEIVER_WIN_SIZE;

     for(i = next_seq_num_ptr, j = 0; j < app_data_len; i = (i + 1) % RECEIVER_WIN_SIZE, j++)
         ctx->send_window[i] = app_data[j];
 }

 size_t bufferReceiveData(size_t start, char *app_data, size_t app_data_len) // TODO: refactor
 {
     size_t i, j, bytes_delivered;

     start           = start % RECEIVER_WIN_SIZE;
     bytes_delivered = 0;
     assert(app_data);

     printf("\n%u bytes to be buffered", app_data_len);
     for(i = start, j = 0; j < app_data_len; i = (i + 1) % RECEIVER_WIN_SIZE, j++)
     {
         if(ctx->recvWindowLookup[i] == 0)
         {
             //printf("\nByte with window seq number %u has been buffered", i);
             ctx->recv_window[i]        = app_data[j];
             ctx->recvWindowLookup[i] = 1;
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
     if(MAX_SEQUENCE_NUMBER - ctx->send_base < RECEIVER_WIN_SIZE)
     {
         printf("\nWrap around!");
         /* if the send_base and rcv_nxtber have not wrapped around */
         if(ctx->rcv_nxt > ctx->send_base)
             curr_send_window_left = (MAX_SEQUENCE_NUMBER - ctx->rcv_nxt) + (RECEIVER_WIN_SIZE - (MAX_SEQUENCE_NUMBER - ctx->send_base));
         /* if the rcv_nxtber has wrapped around */
         else
             curr_send_window_left = MAX_SEQUENCE_NUMBER - ctx->send_base + (ctx->rcv_nxt + 1);
     }
     /* no wrap around */
     else
     {
         printf("\nNo wrap around");
         curr_send_window_left = ctx->send_base + RECEIVER_WIN_SIZE - ctx->rcv_nxt;
     }

     return curr_send_window_left;
 }

void headerSend(tcp_seq seq_num) // TODO: refactor
 {
     /* construct the header */
     STCPHeader *new_header = NULL;
     ssize_t bytes_sent;

     new_header = constructHeader(seq_num);

     /* send ACK */
     do
     {
         bytes_sent = stcp_network_send(ctx->sd, new_header, HEADER_LEN, NULL);
     }while(bytes_sent == -1);
     printf("\nACK %d sent to network layer", new_header->th_ack);

     /* free up memory */
     if(new_header)
     {
         free(new_header);
         new_header = NULL;
     }
 }

STCPHeader *constructHeader(tcp_seq seq_num) // TODO: Refactor
{
  STCPHeader *header = NULL;
  
  header = (STCPHeader *) malloc(HEADER_LEN);
  assert(header);
  assert(ctx);
  memset(header, 0, HEADER_LEN);
  
  header->th_seq   = seq_num;
  header->th_ack   = ctx->rcv_nxt;
  header->th_flags = 0 | TH_ACK;
  header->th_off   = TCPHEADER_OFFSET;
  header->th_win   = RECEIVER_WIN_SIZE;
  
  printf("\nNew ACK header constructed with sequence number: %u", seq_num);
  return header;
}


 size_t dataDeliverToApplication() // TODO: refactor
 {
     size_t i, j, app_data_len;
     char *app_data;

     /* calculate the number of bytes that can be delivered */
     app_data_len = 0;
     i            = ctx->expected_sequence_num_ptr;
     while(ctx->recvWindowLookup[i] == 1 && app_data_len < RECEIVER_WIN_SIZE)
     {
         i = (i + 1) % RECEIVER_WIN_SIZE;
         app_data_len++;
     }

     /* create a buffer that can be used to deliver the data to application */
     app_data = (char *) malloc(app_data_len * sizeof(char));
     assert(app_data);

     /* store the data to be delivered in app_data */
     /* update the recv_buffer_lookup table */
     for(i = ctx->expected_sequence_num_ptr, j = 0; j < app_data_len; i = (i + 1) % RECEIVER_WIN_SIZE, j++)
     {
         app_data[j]                = ctx->recv_window[i];
         ctx->recvWindowLookup[i] = 0;
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

