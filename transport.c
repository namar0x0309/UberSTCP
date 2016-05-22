/*
 * transport.c 
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. 
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

#define FIXED_INITNUM 	// debug: start seq numbering from 1

#define TCPHEADER_OFFSET    5
#define HEADER_LEN          sizeof( STCPHeader)
#define OPTIONS_LEN         40			// max options length
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
}; 


/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done;    /* TRUE once connection is closed */

    int connection_state;   /* state of the connection (established, etc.) */

    /* any other connection-wide global variables go here */
    mysocket_t sd;	/// TODO: do we need this?

    /* Send Sequence Variables; RFC 793 [Page 25] */
    tcp_seq snd_una;        // oldest unacknowledged sequence number
    tcp_seq snd_nxt;        // next sequence number to be sent
    unsigned int snd_wnd;   // send window
    tcp_seq iss;

    /* Receive Sequence Variables RFC 793 [Page 25] */
    tcp_seq rcv_nxt;        // next sequence number expected on an incoming segment, and is the left or lower edge of the receive window
    unsigned int rcv_wnd;   // receive window
    tcp_seq irs;            // initial receive sequence number
			
    // tcp_seq receiver_window;             /* receiver window size last advertised by the receiver or the other host */
	
	// buffers :
	// send buffer ...
	// receiver buffer ...
	
	// to avoid multiple reallocations		///TODO: use these instead of local variables
	STCPHeader snd_h;  // construct header to send; handle data separately
	
} context_t;

static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);
static void receiveNetworkSegment(mysocket_t sd, context_t *ctx);
void sendAppData(mysocket_t sd, context_t *ctx);
void setupSequence(mysocket_t sd, context_t *ctx, bool is_active);
void teardownSequence(mysocket_t sd, context_t *ctx, bool is_active);
void our_dprintf(const char *format,...);

// context_t *ctx;

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    fflush(stdout);
	printf( "\n%s", __FUNCTION__ );

    context_t *ctx;

    ctx = (context_t    *) calloc(1, sizeof(context_t));
    assert(ctx);
	
	ctx->sd = sd;
    ctx->rcv_wnd = RECEIVER_WIN_SIZE;

    /* XXX: after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */

	setupSequence(sd, ctx, is_active);
	
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
        unsigned int event = stcp_wait_for_event(sd, ANY_EVENT, NULL);

        /* check whether it was the network, app, or a close request */
        if (event & APP_DATA){
            /* the application has requested that data be sent */
            our_dprintf("\nApp data available to send");
            sendAppData(sd, ctx);
        }

         if (event & NETWORK_DATA){
            our_dprintf("\nNetwork data available to receive");
            receiveNetworkSegment(sd, ctx);
        }

         if (event & APP_CLOSE_REQUESTED ){
            teardownSequence(sd, ctx, true);
        }

        /// TODO: FREE UP MEMORY
    }
}

// /**************************** Event Handlers *********************************************/

 /* Process application data */
 /** todo: Sequence numbers need mod 2^32 arithmetic;  */
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
		 sgt = (char *) malloc((send_capacity) * sizeof(char));  /* why not permaently allocate 1-MSS buffer globally and reuse it? */
		 data = sgt + HEADER_LEN;
		 data_len = send_capacity - HEADER_LEN;
		
         /* get data*/
		 grabbed_bytes = stcp_app_recv(sd, data, data_len);
         printf("\nData accepted from application: %lu bytes", grabbed_bytes);
		
		 /* build header */
		 snd_h = (STCPHeader *)sgt;
		 memset(snd_h, 0, HEADER_LEN);
		 snd_h->th_seq = ctx->snd_nxt;
		 snd_h->th_win  = CONGESTION_WIN_SIZE;
		 snd_h->th_off  = HEADER_LEN;

		 /* push both header and data to network */
		 passed_bytes = stcp_network_send(sd, sgt, grabbed_bytes + HEADER_LEN, NULL);
		 if (passed_bytes < 0 ) { 
			 /** todo: error? or retry?ow namy times to retry? */
		 }
		 /*update next sequence number */
		 ctx->snd_nxt += grabbed_bytes;
		
		 /*free memory*/
		 free(sgt);
	 }
 }
///>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
// /* Process a segment received from the network */
 void receiveNetworkSegment(mysocket_t sd, context_t *ctx) 
 {
     STCPHeader *rcv_h, *snd_h;
	 snd_h = &(ctx->snd_h);
	 memset(snd_h, 0, HEADER_LEN);
	 
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
     ctx->snd_wnd = MIN( rcv_h->th_win, CONGESTION_WIN_SIZE ); /// TODO: This shouldn't go here in case it's a bad packet

     // ESTABLISHED, FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT, CLOSING, LAST-ACK, or TIME-WAIT state
     // RFC 793 [Page 69]
     {

         /* Check sequence number
          * If the segment contains data that comes after the next byte we're expecting,
          * send an ACK for the next expected byte and drop the packet (by returning) */
         if (seg_len > 0 && seg_seq > ctx->rcv_nxt) {
             // headerSend(ctx->rcv_nxt);   /// TODO: make sure headerSend() sends an ACK like the code implies
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
             /// TODO: set error? or ignore?
			 free(seg);
             return;
         }

         /* Check the ACK field; RFC 793 [Page 72] */
         if (rcv_h->th_flags & TH_ACK) {
             seg_ack = rcv_h->th_ack;
             printf("\nProcessing ACK %u", seg_ack);

             // If in the ESTABLISHED state
             if (ctx->connection_state == ESTABLISHED) {

                 // If the ACK is within the send window, update the last unacknowledged byte and send window
                 if (ctx->snd_una < seg_ack && seg_ack <= ctx->snd_nxt) {
                     printf("\nThe ACK is within the send window");
                     ctx->snd_una = seg_ack;
                     ctx->snd_wnd = rcv_h->th_win;

                 // If it's a duplicate of the most recent ACK, just update the send window
                 } else if (ctx->snd_una == seg_ack) {
                     ctx->snd_wnd = rcv_h->th_win;
                 }

             // If in FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT, CLOSING, LAST-ACK, or TIME-WAIT
             } else {
                 /// TODO: Handle these states
                 free(seg);
                 return;
             }
             /// TODO: stop the timer if it is running and start it if there are unACKed segments (I'm hoping someone else has already gotten a timer set up, otherwise I'll take care of it)

             printf("\nDone processing ACK");
         }

        /* Process the segment data; RFC 793 [Page 74] */ /// TODO: Handle according to state
        if (seg_len > 0) {
            printf("\nHandling received data beginning at sequence number %u,", seg_seq);
            /** TODO: See if Nassim has the handle_app_data() code available; otherwise
             * I'll write something to pass the data to the application */

            /* We've now taken responsibility for delivering the data to the user, so
             * we ACK receipt of the data and advance rcv_nxt over the data accepted */
            ctx->rcv_nxt += seg_len;
            snd_h->th_seq = ctx->snd_nxt; 
			snd_h->th_ack = ctx->rcv_nxt;
			snd_h->th_flags = 0 | TH_ACK;
			snd_h->th_win = ctx->rcv_wnd;
			snd_h->th_off = TCPHEADER_OFFSET;
			
			printf("\nSending SYNACK");
			stcp_network_send( sd, snd_h, HEADER_LEN, NULL );

         }

         if (rcv_h->th_flags *
             TH_FIN)  /// TODO: See if I need to handle any packets coming in during the FIN sequence
         {
             teardownSequence(sd, ctx, false);
         }
     }
     free(seg);
 }

void setupSequence(mysocket_t sd, context_t *ctx, bool is_active){
	STCPHeader *rcv_h, *snd_h;
    snd_h = &ctx->snd_h;
	memset(snd_h, 0, HEADER_LEN);
        
	generate_initial_seq_num(ctx);

    // Received OPEN call from application; RFC 793 [Page 54]
    if (is_active) {
        printf("\nActive: sending SYN");
        ctx->connection_state = CLOSED;

        // Set up SYN header
        snd_h->th_seq   = ctx->iss;
        snd_h->th_flags = 0 | TH_SYN;
        snd_h->th_win   = RECEIVER_WIN_SIZE;
        snd_h->th_off   = TCPHEADER_OFFSET;

        // Send the SYN
        if( stcp_network_send( sd, snd_h, HEADER_LEN, NULL ) == -1  )
            errno = ECONNREFUSED;
        else {
            ctx->snd_una = ctx->iss;
            ctx->snd_nxt = ctx->iss + 1;
            ctx->connection_state = SYN_SENT;
        }

        // Enter the syn-loop to wait for SYNACK
        printf( "\nActive: SYN has been sent. Entering the syn-loop to wait for SYNACK" );
        
    } else {
		// Connection inactive; enter syn-loop to wait for SYN");
        ctx->connection_state = LISTEN;
    }
	
	
	char *seg;
    ssize_t seg_len_incl_hdr= STCP_MSS;
    seg = (char*)malloc( seg_len_incl_hdr * sizeof( char ) );
    assert( seg );

    // Current Segment Variables; RFC 793 [Page 25]
    tcp_seq seg_seq;    // first sequence number of a segment
    tcp_seq seg_ack;    // acknowledgment from the receiving TCP (next sequence number expected by the receiving TCP)
    ssize_t seg_wnd;    // segment window


	// enter syn-loop
	while ( ctx->connection_state != ESTABLISHED){
		unsigned int event = stcp_wait_for_event(sd, ANY_EVENT, NULL);

		/* Receive the segment and extract the header from it */
		seg_len_incl_hdr = stcp_network_recv( sd, seg, seg_len_incl_hdr );
		rcv_h = (STCPHeader*)seg;
		printf("\nSegment received");

		/* Extract info from received header */
		seg_seq = rcv_h->th_seq;
		seg_ack = rcv_h->th_ack;
		seg_wnd = rcv_h->th_win;
		
        if (event & (APP_DATA | APP_CLOSE_REQUESTED)){ 
            /// TODO: set error
			
            break;
        }
        else if (event & NETWORK_DATA){
            
			 // RFC 793 [Page 65]
			if (ctx->connection_state == LISTEN) {

				 // Check for a SYN; Send SYNACK if received
				 if (rcv_h->th_flags & TH_SYN) {
					 printf("\nPassive: received SYN");

					 // Update context fields
					 ctx->rcv_nxt = seg_seq + 1;
					 ctx->irs = seg_seq;

					 // Set up the SYNACK
					 memset(snd_h, 0, HEADER_LEN);
					 snd_h->th_seq = ctx->iss;
					 snd_h->th_ack = ctx->rcv_nxt;
					 snd_h->th_flags = 0 | TH_ACK | TH_SYN;
					 snd_h->th_win = ctx->rcv_wnd;
					 snd_h->th_off = TCPHEADER_OFFSET;

					 // Send the SYNACK
					 printf("\nSending SYNACK");
					 stcp_network_send( sd, snd_h, HEADER_LEN, NULL );

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
			 } else if (ctx->connection_state == SYN_SENT) {

				 // If this is an ACK
				 if (rcv_h->th_flags & TH_ACK) {

					 // Ignore anything with an ack number outside the send window
					 if (seg_ack <= ctx->iss || seg_ack > ctx->snd_nxt || seg_ack < ctx->snd_una) {
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

					 // Otherwise, enter SYN_RECEIVED and send SYNACK
					 } else {

						 // Set up the SYNACK
						 memset(snd_h, 0, HEADER_LEN);
						 snd_h->th_seq = ctx->iss;
						 snd_h->th_ack = ctx->rcv_nxt;
						 snd_h->th_flags = 0 | TH_ACK | TH_SYN;
						 snd_h->th_win = ctx->rcv_wnd;
						 snd_h->th_off = TCPHEADER_OFFSET;

						 // Send the SYNACK
						 printf("\nSending SYNACK");
						 stcp_network_send( sd, snd_h, HEADER_LEN, NULL );

						 // Update the connection state (already set snd_next and snd_una when we sent the SYN)
						 ctx->connection_state = SYN_RECEIVED;
					}

				}

			} else {
				// If this is a SYN, ignore the packet; RFC 793 [Page 71]
				if (rcv_h->th_flags & TH_SYN) {
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

						// If the ACK is not acceptable, drop the packet and ignore
						} else {
							return;
						}
					}
				}
			}
			
        }

	}
	
///<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<	

}

void teardownSequence(mysocket_t sd, context_t *ctx, bool is_active){
	
}

 /****************************** Helper Functions ****************************************/
/* our_dour_dprintf
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