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
#include <stdbool.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"

//#define ENDIAN_CALIBRATE
#define FIXED_INITNUM 	// debug: start seq numbering from 1
#define DEBUG

#define TCPHEADER_OFFSET    5
#define HEADER_LEN          sizeof( STCPHeader)
#define MAX_SEQUENCE_NUMBER 4294967296 /* this is 2^32 */ // REFACTOR
#define CONGESTION_WIN_SIZE 3072
#define RECEIVER_WIN_SIZE   3072
#define SENDER_WIN_SIZE   	3072
#define WAIT_TIME           4       // seconds


// RFC 793 [Page 21]
enum {
    LISTEN,         // Waiting for a cxn request from any remote TCP & port
    SYN_SENT,       // Waiting for SYN-ACK after having sent a SYN
    SYN_RECEIVED,   // Waiting for ACK after receiving SYN and sending SYN-ACK or SYN
    ESTABLISHED,    // An open cnx; data received can be delivered to the user
    FIN_WAIT_1,     // Initiated close; now waiting for FIN-ACK, or FIN if simult close
    FIN_WAIT_2,     // Initiated close; now waiting for FIN from the remote TCP
    CLOSE_WAIT,     // Passive closer waiting for a close request from the app
    CLOSING,        // Simultaneous close - waiting for FIN-ACK from the remote TCP
    LAST_ACK,       // Passive closer waiting for last FIN-ACK
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
    tcp_seq rcv_nxt;        // next seq num expected on rcvd sgmt; left edge of rcv wnd
    unsigned int rcv_wnd;   // receive window
    tcp_seq irs;            // initial receive sequence number

	// buffers :
	char buf_snd[SENDER_WIN_SIZE];
	char buf_rcv[RECEIVER_WIN_SIZE];
	
	// to avoid multiple reallocations, use these instead of local variables
	STCPHeader snd_h;  // construct header to send; handle data separately
} context_t;

static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);
static void receiveNetworkSegment(mysocket_t sd, context_t *ctx);
void sendAppData(mysocket_t sd, context_t *ctx);
void setupSequence(mysocket_t sd, context_t *ctx, bool is_active);
void teardownSequence(mysocket_t sd, context_t *ctx, bool app_close);
void fillHeader(STCPHeader* snd_h, context_t *ctx, int flags);
void forcePrintf(const char *format,...);
void 	calibrateEndianness( char *seg, ssize_t seg_len, bool isFromNetwork );	

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    fflush(stdout);
	forcePrintf( "\n%s", __FUNCTION__ );

    context_t *ctx;

    ctx = (context_t    *) calloc(1, sizeof(context_t));
    assert(ctx);
	
		ctx->sd = sd;
    ctx->rcv_wnd = RECEIVER_WIN_SIZE;
		
    /** XXX: 
     * to communicate an error condition set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before unblocking.
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
    /** Todo:  change this to random? */
    ctx->iss =1;
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
	forcePrintf( "\n%s", __FUNCTION__ );
    assert(ctx);
    assert(!ctx->done);

    while (!ctx->done) /// todo: change to while not CLOSED -- I think it should be while not CLOSED or LAST-ACK
    {
		forcePrintf("\n\nNew event (snd_una | snd_nxt, rcv_nxt): (%u | %u, %u)",
					ctx->snd_una, ctx->snd_nxt, ctx->rcv_nxt);
		forcePrintf("\n\t[snd_wnd,rcv_wnd] = [%u,%u]", ctx->snd_wnd, ctx->rcv_wnd);
	    unsigned int event = stcp_wait_for_event(sd, ANY_EVENT, NULL);

        /* check whether it was the network, app, or a close request */
        if (event & APP_DATA){
            /* the application has requested that data be sent */
            forcePrintf("\nSend triggered ");
            sendAppData(sd, ctx);
        }

         if (event & NETWORK_DATA){
            forcePrintf("\nReceive triggered ");
            receiveNetworkSegment(sd, ctx);
        }

         if (event & APP_CLOSE_REQUESTED ){
			 forcePrintf("\nTeardown triggered - app close requested ");
			 teardownSequence(sd, ctx, true);
        }
    }
}

// /**************************** Event Handlers *********************************************/

/* Process application data */
/** todo: Sequence numbers need mod 2^32 arithmetic;  */
void sendAppData(mysocket_t sd, context_t *ctx)
{
	forcePrintf( "---> %s", __FUNCTION__ );
	char *payload;
	size_t payload_len; /* how many bytes actually read from application */
	ssize_t passed_bytes; /* how many bytes were able to send to network */
	STCPHeader *snd_h = &ctx->snd_h;

	/* send data only if sender window is not full */
	ssize_t send_capacity = ctx->snd_una + ctx->snd_wnd - ctx->snd_nxt;  /* rfc 793 [p. 83]*/
	if( send_capacity > 0 ){
		forcePrintf("\n\t Send - (snd_una | snd_nxt, rcv_nxt): (%u | %u, %u)",
					ctx->snd_una, ctx->snd_nxt, ctx->rcv_nxt);
		forcePrintf("\n\t Send - [snd_wnd,rcv_wnd]: [%u,%u]", ctx->snd_wnd, ctx->rcv_wnd);

		/* adjust max amount of data we can send*/
		if (send_capacity > STCP_MSS) { send_capacity = STCP_MSS;}

		/* allocate space for header and data */
		payload = (char *) malloc((send_capacity) * sizeof(char));  /* why not permaently allocate 1-MSS buffer globally and reuse it? */
		assert(payload);

		/* get data*/
		payload_len = stcp_app_recv(sd, payload, send_capacity);
		/// ^ had a problem with payload_len randomly returning huge (garbage) values. it works now.

		/* build header */
		fillHeader(snd_h, ctx, 0);
		
		/* push both header and data to network */
		calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
		calibrateEndianness( payload, payload_len, false );
		passed_bytes = stcp_network_send(sd, snd_h, HEADER_LEN, payload, payload_len, NULL);
		if (passed_bytes < 0 ) { 
			/// todo: error, network send failed
			errno = ENETDOWN;
		}
		/*update next sequence number */
		ctx->snd_nxt += payload_len;

		/*free memory*/
		free(payload);
	 }
}
///>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
// /* Process a segment received from the network */
void receiveNetworkSegment(mysocket_t sd, context_t *ctx)
{
	forcePrintf( "---> %s", __FUNCTION__ );
	STCPHeader *rcv_h, *snd_h;
	snd_h = &(ctx->snd_h);

	char *seg, *payload;
	ssize_t seg_len_incl_hdr;

	// Current Segment Variables; RFC 793 [Page 25]
	tcp_seq seg_seq;    // first sequence number of a segment
	tcp_seq seg_ack;    // ack from the receiving TCP (next seq num they expect)
	ssize_t seg_len;    // bytes of data in the segment (counting SYN and FIN)
	ssize_t seg_wnd;    // segment window

	/* Allocation */
	seg_len_incl_hdr = STCP_MSS + HEADER_LEN;
	seg = (char *) malloc(seg_len_incl_hdr * sizeof(char));
	assert(seg);

	// Receive the segment
	seg_len_incl_hdr = stcp_network_recv(sd, seg, seg_len_incl_hdr);
	calibrateEndianness( seg, seg_len_incl_hdr, true );	
	if (seg_len_incl_hdr <= 0) {
		/** error: connection terminated by remote host msg? */
    errno = ECONNREFUSED;
		free(seg);
		return;
	}

	rcv_h = (STCPHeader *) seg;
	
	// Extract info from received header
	seg_len = seg_len_incl_hdr - TCP_DATA_START(seg);
	seg_seq = rcv_h->th_seq;
	seg_ack = rcv_h->th_ack;
	seg_wnd = rcv_h->th_win;

	forcePrintf("\n\tReceived (len, seq): (%ld, %u)", seg_len, seg_seq);

	// If sequence number comes after next expected byte, send error
	if (seg_len > 0 && seg_seq > ctx->rcv_nxt) {
		// TODO: send error, received packet out of order
		forcePrintf("\nSeq num: %u past next expected: %u.", seg_seq,  ctx->rcv_nxt);
		free(seg);
		return;
	}

	// Trim off any portion of the data that we've already received
	if (seg_len > 0 && seg_seq < ctx->rcv_nxt) {
		forcePrintf("\n\tTrimming off previously received data %u", seg_ack);
		// rcv_h->th_off += (ctx->rcv_nxt - seg_seq);
		seg_len -= (ctx->rcv_nxt - seg_seq);
		seg_seq = ctx->rcv_nxt;
	}

	// If this is a SYN, ignore the packet; RFC 793 [Page 71]
	if (rcv_h->th_flags & TH_SYN) {
		forcePrintf("\n\tPacket is a SYN, which shouldn't be coming in here.");
		// TODO: set error, received SYN when payload expected
		errno = ECONNRESET; // assuming connection has been reset due to new SYN packet
		free(seg);
		return;
	}

	// Check the ACK field; RFC 793 [Page 72]
	if (rcv_h->th_flags & TH_ACK) {
		seg_ack = rcv_h->th_ack;
		forcePrintf("\n\tReceived ACK %u", seg_ack);

		// If ACK is within the send wnd, update last unACKed byte and send wnd
		if (ctx->snd_una < seg_ack && seg_ack <= ctx->snd_nxt) {
			ctx->snd_una = seg_ack;
			ctx->snd_wnd = MIN(seg_wnd, CONGESTION_WIN_SIZE);

			// If FIN-WAIT-1 & ACK is for our FIN, enter FIN-WAIT-2; RFC 793 [Page 73]
			// It's a FIN ACK if ACK num equals our send-next num; RFC 793 [Page 39]
			if (ctx->connection_state == FIN_WAIT_1 &&
				seg_ack == ctx->snd_nxt) {
				ctx->connection_state = FIN_WAIT_2;

				// IF CLOSING or LAST-ACK and this ACK is for our FIN, we're done (no
				// TIME-WAIT in STCP); RFC 793 [Page 73]
			} else if ((ctx->connection_state == CLOSING ||
						ctx->connection_state == LAST_ACK) &&
					   seg_ack == ctx->snd_nxt) {
				ctx->connection_state = CLOSED;
				free(seg);
				return;
			}
		} else {
			// TODO: set error? ignore the ACK?
			errno = ECONNRESET; // for all intents and purposes we lost context with peer, so akin to a reset. Must restart and rebuild context.
			forcePrintf("\n\tThis ACK is outside the send window");
		}
	}

	// Process the segment data; RFC 793 [Page 74]
	if (seg_len > 0 && (
			ctx->connection_state == ESTABLISHED ||
			ctx->connection_state == FIN_WAIT_1 ||
			ctx->connection_state == FIN_WAIT_2)) {
		forcePrintf("\n\tReceived data processing: (%ld, %u)", seg_len, seg_seq);

		// assume window sizes were respected
		payload = seg + TCP_DATA_START(seg);
		stcp_app_send(sd, payload, seg_len);

		/* We've now taken responsibility for delivering the data to the user, so
         * we ACK receipt of the data and advance rcv_nxt over the data accepted */
		ctx->rcv_nxt += seg_len;
		fillHeader(snd_h, ctx, TH_ACK);

		forcePrintf("\n\tReceive complete. Sending ACK %u", ctx->rcv_nxt);
		calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
		stcp_network_send(sd, snd_h, HEADER_LEN, NULL);
	}

	if (rcv_h->th_flags & TH_FIN) {
		teardownSequence(sd, ctx, false);
	}

	free(seg);
}

void setupSequence(mysocket_t sd, context_t *ctx, bool is_active){
	forcePrintf( "\n%s", __FUNCTION__ );

	STCPHeader *rcv_h, *snd_h;
    snd_h = &ctx->snd_h;
	    
	generate_initial_seq_num(ctx);
	ctx->rcv_wnd = MIN(RECEIVER_WIN_SIZE, CONGESTION_WIN_SIZE);
	
    // Received OPEN call from application; RFC 793 [Page 54]
    if (is_active) {
		forcePrintf("\n\tSetup - active: sending SYN");
        ctx->connection_state = CLOSED;

        // Set up SYN header
        fillHeader(snd_h, ctx, TH_SYN);
        
        // Send the SYN
				calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
        if( stcp_network_send( sd, snd_h, HEADER_LEN, NULL ) == -1  )
            errno = ECONNREFUSED;
        else {
            ctx->snd_una = ctx->iss;
            ctx->snd_nxt = ctx->iss + 1;
            ctx->connection_state = SYN_SENT;
        }
        // Procede to enter the syn-loop to wait for SYNACK
		
    } else {
		// Connection inactive; enter syn-loop to wait for SYN");
        ctx->connection_state = LISTEN;
    }
	
	
	char *seg;
    ssize_t seg_len_incl_hdr= STCP_MSS + HEADER_LEN;
    seg = (char*)malloc( seg_len_incl_hdr * sizeof( char ) );
    assert( seg );

    // Current Segment Variables; RFC 793 [Page 25]
    tcp_seq seg_seq;    // first sequence number of a segment
    tcp_seq seg_ack;    // ack from the receiving TCP (next seq num they expect)
    ssize_t seg_wnd;    // segment window

	// enter syn-loop
	while ( ctx->connection_state != ESTABLISHED){
		unsigned int event = stcp_wait_for_event(sd, ANY_EVENT, NULL);

		/* Receive the segment and extract the header from it */
		seg_len_incl_hdr = stcp_network_recv( sd, seg, seg_len_incl_hdr );
		calibrateEndianness( seg, seg_len_incl_hdr, true );	
		
		rcv_h = (STCPHeader*)seg;
		forcePrintf("\n\tSetup - packet received");

		/* Extract info from received header */
		seg_seq = rcv_h->th_seq;
		seg_ack = rcv_h->th_ack;
		seg_wnd = rcv_h->th_win;

		if (event & (APP_DATA | APP_CLOSE_REQUESTED)){
			/// TODO: set error, wrong event flag in Syn-loop
			errno = EISCONN;
			break;
		}
		else if (event & NETWORK_DATA){

			// RFC 793 [Page 65]
			if (ctx->connection_state == LISTEN) {

				// Check for a SYN; Send SYNACK if received
				if (rcv_h->th_flags & TH_SYN) {
					forcePrintf("\n\tSetup - passive: received SYN");

					// Update context fields
					ctx->rcv_nxt = seg_seq + 1;
					ctx->irs = seg_seq;

					// Set up the SYNACK
					fillHeader(snd_h, ctx, TH_ACK | TH_SYN);

					// Send the SYNACK
					forcePrintf("\n\tSetup - sending SYNACK"); fflush(stdout);
					calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
					stcp_network_send( sd, snd_h, HEADER_LEN, NULL );

					// Update sequence numbers and connection state
					ctx->snd_nxt = ctx->iss + 1;
					ctx->snd_una = ctx->iss;
					ctx->connection_state = SYN_RECEIVED;

					// Anything other than a SYN in LISTEN state should be ignored in STCP
				} else {
					/// ToDO set error? got non-SYN packet in LISTEN
					errno = ECONNRESET; // for all intents and purposes we lost context with peer, so akin to a reset. Must restart and rebuild context.
					break;
				}

				// RFC 793 [Page 66]
			} else if (ctx->connection_state == SYN_SENT) {

				// If this is an ACK
				if (rcv_h->th_flags & TH_ACK) {

					// Ignore anything with an ack number outside the send window
					if (seg_ack <= ctx->iss || seg_ack > ctx->snd_nxt || seg_ack < ctx->snd_una) {
						/// todo: error, out-of-order packet in SYN_SENT
						break;
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
						ctx->snd_wnd = MIN(seg_wnd, CONGESTION_WIN_SIZE);
						forcePrintf("\n\tSetup - ESTABLISHED; ISS: %u", ctx->iss);

						// ACK the SYN/SYNACK just received
						fillHeader(snd_h, ctx, TH_ACK);

						// Send the ACK
						forcePrintf("\n\tSetup - sending ACK");
						calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
						stcp_network_send( sd, snd_h, HEADER_LEN, NULL );

						// Otherwise, enter SYN_RECEIVED and send SYNACK
					} else {

						// Set up the SYNACK
						fillHeader(snd_h, ctx, TH_ACK | TH_SYN);

						// Send the SYNACK
						forcePrintf("\n\tSetup - sending SYNACK");
						calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
						stcp_network_send( sd, snd_h, HEADER_LEN, NULL );

						// Update the connection state (already set snd_next
						// and snd_una when we sent the SYN)
						ctx->connection_state = SYN_RECEIVED;
					}

				}

			} else { //SYN_RECEIVED
				// If this is a SYN, ignore the packet; RFC 793 [Page 71]
				if (rcv_h->th_flags & TH_SYN) {
					/// error?? got duplicate SYN in SYN_RECEIVED
					errno = ECONNRESET; // for all intents and purposes we lost context with peer, so akin to a reset. Must restart and rebuild context.
					continue;
				}

				/* Check the ACK field; RFC 793 [Page 72] */
				if (rcv_h->th_flags & TH_ACK) {
					forcePrintf("\n\tSetup - processing ACK %u", seg_ack);
					seg_ack = rcv_h->th_ack;

					// If the ack is within the send window, enter ESTABLISHED state
					if (ctx->snd_una < seg_ack && seg_ack <= ctx->snd_nxt) {
						ctx->connection_state = ESTABLISHED;
						forcePrintf("\n\tSetup - ESTABLISHED; ISS: %u", ctx->iss);
						ctx->snd_wnd = MIN(seg_wnd, CONGESTION_WIN_SIZE);
						ctx->snd_una = seg_ack;
						// If the ACK is not acceptable, drop the packet and ignore
					} else {
						/// error: got out-of-order ACK in SYN_RECEIVED
						errno = ECONNRESET; // for all intents and purposes we lost context with peer, so akin to a reset. Must restart and rebuild context.
						continue;
					}

				}
			}
			
        }

	}
	
///<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<	
	free(seg);
}

void teardownSequence(mysocket_t sd, context_t *ctx, bool app_close){

	forcePrintf( "---> %s", __FUNCTION__ );

	STCPHeader *snd_h;
	snd_h = &(ctx->snd_h);

	// Received CLOSE call from app; begin or advance through FIN seq; RFC 793 [Page 60]
	if (app_close) {
		if (ctx->connection_state == ESTABLISHED ||
			ctx->connection_state == CLOSE_WAIT) {

			// All data from app has been sent; form FIN seg and send; RFC 793 [Page 39]
			fillHeader(snd_h, ctx, TH_FIN | TH_ACK);
			forcePrintf("\n\tTeardown - sending FIN, %u", ctx->rcv_nxt);
			calibrateEndianness( (char*)snd_h, HEADER_LEN, false );	
			stcp_network_send(sd, snd_h, HEADER_LEN, NULL);

			// Update the state
			if (ctx->connection_state == ESTABLISHED) {
				ctx->connection_state = FIN_WAIT_1;
			} else {
				ctx->connection_state = LAST_ACK;
			}
		} else {
			// TODO: error in any other state
		}

		// Received packet with FIN bit set; advance through FIN seq; RFC 793 [Page 75]
	} else {

		forcePrintf("\n\tTeardown - received FIN");

		// Advance rcv_next over the FIN
		ctx->rcv_nxt++;

		// Update the state
		if (ctx->connection_state == ESTABLISHED) {
			ctx->connection_state = CLOSE_WAIT;
		} else if (ctx->connection_state == FIN_WAIT_1 ||
				ctx->connection_state == FIN_WAIT_2) {
			ctx->connection_state = CLOSING;
		}
	}
}

/****************************** Helper Functions ****************************************/
void fillHeader(STCPHeader* snd_h, context_t *ctx, int flags){
	memset(snd_h, 0, HEADER_LEN);
	
	if (flags & TH_SYN)
		snd_h->th_seq = ctx->iss;
	else //if (!flags || flags & TH_FIN) // flags == 0
		snd_h->th_seq = ctx->snd_nxt;
		
	if (flags & TH_ACK)
		snd_h->th_ack = ctx->rcv_nxt;
		
	snd_h->th_flags = 0 | flags;
	snd_h->th_win = ctx->rcv_wnd;
	snd_h->th_off = TCPHEADER_OFFSET;
}

/*
	htons()	Host to Network Short
	htonl()	Host to Network Long
	ntohl()	Network to Host Long
	ntohs()	Network to Host Short
*/
void 	calibrateEndianness( char *seg, ssize_t seg_len, bool isFromNetwork )	
{
#ifdef ENDIAN_CALIBRATE
	if( isFromNetwork )
	{
		for( int i = 0; i < seg_len; ++i )
		{
			seg[ i ] = ntohl( seg[ i ] );
		}
	} 
	else
	{
		for( int i = 0; i < seg_len; ++i )
		{
			seg[ i ] = htonl( seg[ i ] );
		}
	}
#endif
}

// Forces stdout output
void forcePrintf(const char *format,...)
{
	if( !format ) return;
#ifdef DEBUG
    va_list valistArgs;
    char buffer[1024];

    va_start(valistArgs, format);
    
		vsnprintf(buffer, sizeof(buffer), format, valistArgs);
    
		va_end(valistArgs);
		
    fputs(buffer, stdout);
		
    fflush(stdout);
#endif
}