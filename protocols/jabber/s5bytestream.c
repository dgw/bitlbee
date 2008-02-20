/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Jabber module - SOCKS5 Bytestreams ( XEP-0065 )                          *
*                                                                           *
*  Copyright 2007 Uli Meis <a.sporto+bee@gmail.com>                         *
*                                                                           *
*  This program is free software; you can redistribute it and/or modify     *
*  it under the terms of the GNU General Public License as published by     *
*  the Free Software Foundation; either version 2 of the License, or        *
*  (at your option) any later version.                                      *
*                                                                           *
*  This program is distributed in the hope that it will be useful,          *
*  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
*  GNU General Public License for more details.                             *
*                                                                           *
*  You should have received a copy of the GNU General Public License along  *
*  with this program; if not, write to the Free Software Foundation, Inc.,  *
*  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.              *
*                                                                           *
\***************************************************************************/

#include "jabber.h"
#include "sha1.h"
#include <poll.h>

struct bs_transfer {

	struct jabber_transfer *tf;

	jabber_streamhost_t *sh;
	GSList *streamhosts;

	enum 
	{ 
		BS_PHASE_CONNECT, 
		BS_PHASE_CONNECTED, 
		BS_PHASE_REQUEST, 
		BS_PHASE_REPLY
	} phase;

	/* SHA1( SID + Initiator JID + Target JID) */
	char *pseudoadr;

	gint connect_timeout;
};

struct socks5_message
{
	unsigned char ver;
	union
	{
		unsigned char cmd;
		unsigned char rep;
	} cmdrep;
	unsigned char rsv;
	unsigned char atyp;
	unsigned char addrlen;
	unsigned char address[40];
	in_port_t port;
} __attribute__ ((packed)); 

/* connect() timeout in seconds. */
#define JABBER_BS_CONTIMEOUT 15
/* listen timeout */
#define JABBER_BS_LISTEN_TIMEOUT  90

/* very useful */
#define ASSERTSOCKOP(op, msg) \
	if( (op) == -1 ) \
		return jabber_bs_abort( bt , msg ": %s", strerror( errno ) );

gboolean jabber_bs_abort( struct bs_transfer *bt, char *format, ... );
void jabber_bs_canceled( file_transfer_t *ft , char *reason );
void jabber_bs_free_transfer( file_transfer_t *ft );
gboolean jabber_bs_connect_timeout( gpointer data, gint fd, b_input_condition cond );
gboolean jabber_bs_poll( struct bs_transfer *bt, int fd, short *revents );
gboolean jabber_bs_peek( struct bs_transfer *bt, void *buffer, int buflen );

void jabber_bs_recv_answer_request( struct bs_transfer *bt );
gboolean jabber_bs_recv_read( gpointer data, gint fd, b_input_condition cond );
gboolean jabber_bs_recv_write_request( file_transfer_t *ft );
gboolean jabber_bs_recv_handshake( gpointer data, gint fd, b_input_condition cond );
gboolean jabber_bs_recv_handshake_abort( struct bs_transfer *bt, char *error );
int jabber_bs_recv_request( struct im_connection *ic, struct xt_node *node, struct xt_node *qnode );

gboolean jabber_bs_send_handshake_abort( struct bs_transfer *bt, char *error );
gboolean jabber_bs_send_request( struct jabber_transfer *tf, GSList *streamhosts );
gboolean jabber_bs_send_handshake( gpointer data, gint fd, b_input_condition cond );
gboolean jabber_bs_send_listen( struct bs_transfer *bt, struct sockaddr_storage *saddr, char *host, char *port );
static xt_status jabber_bs_send_handle_activate( struct im_connection *ic, struct xt_node *node, struct xt_node *orig );
void jabber_bs_send_activate( struct bs_transfer *bt );

/*
 * Frees a bs_transfer struct and calls the SI free function
 */
void jabber_bs_free_transfer( file_transfer_t *ft) {
	struct jabber_transfer *tf = ft->data;
	struct bs_transfer *bt = tf->streamhandle;
	jabber_streamhost_t *sh;

	if ( bt->connect_timeout )
	{
		b_event_remove( bt->connect_timeout );
		bt->connect_timeout = 0;
	}

	if ( tf->watch_in )
		b_event_remove( tf->watch_in );
	
	if( tf->watch_out )
		b_event_remove( tf->watch_out );
	
	g_free( bt->pseudoadr );

	while( bt->streamhosts )
	{
		sh = bt->streamhosts->data;
		bt->streamhosts = g_slist_remove( bt->streamhosts, sh );
		g_free( sh->jid );
		g_free( sh->host );
		g_free( sh );
	}
	
	g_free( bt );

	jabber_si_free_transfer( ft );
}

/*
 * Checks if buflen data is available on the socket and
 * writes it to buffer if that's the case.
 */
gboolean jabber_bs_peek( struct bs_transfer *bt, void *buffer, int buflen )
{
	int ret;
	int fd = bt->tf->fd;

	ASSERTSOCKOP( ret = recv( fd, buffer, buflen, MSG_PEEK ), "MSG_PEEK'ing" );

	if( ret == 0 )
		return jabber_bs_abort( bt, "Remote end closed connection" );
		
	if( ret < buflen )
		return ret;

	ASSERTSOCKOP( ret = recv( fd, buffer, buflen, 0 ), "Dequeuing after MSG_PEEK" );

	if( ret != buflen )
		return jabber_bs_abort( bt, "recv returned less than previous recv with MSG_PEEK" );
	
	return ret;
}


/* 
 * This function is scheduled in bs_handshake via b_timeout_add after a (non-blocking) connect().
 */
gboolean jabber_bs_connect_timeout( gpointer data, gint fd, b_input_condition cond )
{
	struct bs_transfer *bt = data;

	bt->connect_timeout = 0;

	jabber_bs_abort( bt, "no connection after %d seconds", bt->tf->ft->sending ? JABBER_BS_LISTEN_TIMEOUT : JABBER_BS_CONTIMEOUT );

	return FALSE;
}

/* 
 * Polls the socket, checks for errors and removes a connect timer
 * if there is one.
 */
gboolean jabber_bs_poll( struct bs_transfer *bt, int fd, short *revents )
{
	struct pollfd pfd = { .fd = fd, .events = POLLHUP|POLLERR };
	
	if ( bt->connect_timeout )
	{
		b_event_remove( bt->connect_timeout );
		bt->connect_timeout = 0;
	}

	ASSERTSOCKOP( poll( &pfd, 1, 0 ), "poll()" )

	if( pfd.revents & POLLERR )
	{
		int sockerror;
		socklen_t errlen = sizeof( sockerror );

		if ( getsockopt( fd, SOL_SOCKET, SO_ERROR, &sockerror, &errlen ) )
			return jabber_bs_abort( bt, "getsockopt() failed, unknown socket error during SOCKS5 handshake (weird!)" );

		if ( bt->phase == BS_PHASE_CONNECTED )
			return jabber_bs_abort( bt, "connect failed: %s", strerror( sockerror ) );

		return jabber_bs_abort( bt, "Socket error during SOCKS5 handshake(weird!): %s", strerror( sockerror ) );
	}

	if( pfd.revents & POLLHUP )
		return jabber_bs_abort( bt, "Remote end closed connection" );
	
	*revents = pfd.revents;
	
	return TRUE;
}

/*
 * Used for receive and send path.
 */
gboolean jabber_bs_abort( struct bs_transfer *bt, char *format, ... )
{
	va_list params;
	va_start( params, format );
	char error[128];

	if( vsnprintf( error, 128, format, params ) < 0 )
		sprintf( error, "internal error parsing error string (BUG)" );
	va_end( params );
	if( bt->tf->ft->sending )
		return jabber_bs_send_handshake_abort( bt, error );
	else
		return jabber_bs_recv_handshake_abort( bt, error );
}

/* Bad luck */
void jabber_bs_canceled( file_transfer_t *ft , char *reason )
{
	struct jabber_transfer *tf = ft->data;

	imcb_log( tf->ic, "File transfer aborted: %s", reason );
}

/*
 * Parses an incoming bytestream request and calls jabber_bs_handshake on success.
 */
int jabber_bs_recv_request( struct im_connection *ic, struct xt_node *node, struct xt_node *qnode)
{
	char *sid, *ini_jid, *tgt_jid, *mode, *iq_id;
	struct jabber_data *jd = ic->proto_data;
	struct jabber_transfer *tf = NULL;
	GSList *tflist;
	struct bs_transfer *bt;
	GSList *shlist=NULL;
	struct xt_node *shnode;

	sha1_state_t sha;
	char hash_hex[41];
	unsigned char hash[20];
	int i;
	
	if( !(iq_id   = xt_find_attr( node, "id" ) ) ||
	    !(ini_jid = xt_find_attr( node, "from" ) ) ||
	    !(tgt_jid = xt_find_attr( node, "to" ) ) ||
	    !(sid     = xt_find_attr( qnode, "sid" ) ) )
	{
		imcb_log( ic, "WARNING: Received incomplete SI bytestream request");
		return XT_HANDLED;
	}

	if( ( mode = xt_find_attr( qnode, "mode" ) ) &&
	      ( strcmp( mode, "tcp" ) != 0 ) ) 
	{
		imcb_log( ic, "WARNING: Received SI Request for unsupported bytestream mode %s", xt_find_attr( qnode, "mode" ) );
		return XT_HANDLED;
	}

	shnode = qnode->children;
	while( ( shnode = xt_find_node( shnode, "streamhost" ) ) )
	{
		char *jid, *host;
		int port;
		if( ( jid = xt_find_attr( shnode, "jid" ) ) &&
		    ( host = xt_find_attr( shnode, "host" ) ) &&
		    ( ( port = atoi( xt_find_attr( shnode, "port" ) ) ) ) )
		{
			jabber_streamhost_t *sh = g_new0( jabber_streamhost_t, 1 );
			sh->jid = g_strdup(jid);
			sh->host = g_strdup(host);
			sprintf( sh->port, "%u", port );
			shlist = g_slist_append( shlist, sh );
		}
		shnode = shnode->next;
	}
	
	if( !shlist )
	{
		imcb_log( ic, "WARNING: Received incomplete SI bytestream request, no parseable streamhost entries");
		return XT_HANDLED;
	}

	/* Let's see if we can find out what this bytestream should be for... */

	for( tflist = jd->filetransfers ; tflist; tflist = g_slist_next(tflist) )
	{
		struct jabber_transfer *tft = tflist->data;
		if( ( strcmp( tft->sid, sid ) == 0 ) &&
		    ( strcmp( tft->ini_jid, ini_jid ) == 0 ) &&
		    ( strcmp( tft->tgt_jid, tgt_jid ) == 0 ) )
		{
		    	tf = tft;
			break;
		}
	}

	if (!tf) 
	{
		imcb_log( ic, "WARNING: Received bytestream request from %s that doesn't match an SI request", ini_jid );
		return XT_HANDLED;
	}

	/* iq_id and canceled can be reused since SI is done */
	g_free( tf->iq_id );
	tf->iq_id = g_strdup( iq_id );

	tf->ft->canceled = jabber_bs_canceled;

	/* SHA1( SID + Initiator JID + Target JID ) is given to the streamhost which it will match against the initiator's value */
	sha1_init( &sha );
	sha1_append( &sha, (unsigned char*) sid, strlen( sid ) );
	sha1_append( &sha, (unsigned char*) ini_jid, strlen( ini_jid ) );
	sha1_append( &sha, (unsigned char*) tgt_jid, strlen( tgt_jid ) );
	sha1_finish( &sha, hash );
	
	for( i = 0; i < 20; i ++ )
		sprintf( hash_hex + i * 2, "%02x", hash[i] );
		
	bt = g_new0( struct bs_transfer, 1 );
	bt->tf = tf;
	bt->streamhosts = shlist;
	bt->sh = shlist->data;
	bt->phase = BS_PHASE_CONNECT;
	bt->pseudoadr = g_strdup( hash_hex );
	tf->streamhandle = bt;
	tf->ft->free = jabber_bs_free_transfer;

	jabber_bs_recv_handshake( bt, 0, 0 ); 

	return XT_HANDLED;
}

/*
 * This is what a protocol handshake can look like in cooperative multitasking :)
 * Might be confusing at first because it's called from different places and is recursing.
 * (places being the event thread, bs_request, bs_handshake_abort, and itself)
 *
 * All in all, it turned out quite nice :)
 */
gboolean jabber_bs_recv_handshake( gpointer data, gint fd, b_input_condition cond )
{

	struct bs_transfer *bt = data;
	short revents;

	if ( ( fd != 0 ) && !jabber_bs_poll( bt, fd, &revents ) )
		return FALSE;
	
	switch( bt->phase ) 
	{
	case BS_PHASE_CONNECT:
		{
			struct addrinfo hints, *rp;

			memset( &hints, 0, sizeof( struct addrinfo ) );
			hints.ai_socktype = SOCK_STREAM;

			if ( getaddrinfo( bt->sh->host, bt->sh->port, &hints, &rp ) != 0 )
				return jabber_bs_abort( bt, "getaddrinfo() failed: %s", strerror( errno ) );

			ASSERTSOCKOP( bt->tf->fd = fd = socket( rp->ai_family, rp->ai_socktype, 0 ), "Opening socket" );

			sock_make_nonblocking( fd );

			imcb_log( bt->tf->ic, "File %s: Connecting to streamhost %s:%s", bt->tf->ft->file_name, bt->sh->host, bt->sh->port );

			if( ( connect( fd, rp->ai_addr, rp->ai_addrlen ) == -1 ) &&
			    ( errno != EINPROGRESS ) )
				return jabber_bs_abort( bt , "connect() failed: %s", strerror( errno ) );

			freeaddrinfo( rp );

			bt->phase = BS_PHASE_CONNECTED;
			
			bt->tf->watch_out = b_input_add( fd, GAIM_INPUT_WRITE, jabber_bs_recv_handshake, bt );

			/* since it takes forever(3mins?) till connect() fails on itself we schedule a timeout */
			bt->connect_timeout = b_timeout_add( JABBER_BS_CONTIMEOUT * 1000, jabber_bs_connect_timeout, bt );

			bt->tf->watch_in = 0;
			return FALSE;
		}
	case BS_PHASE_CONNECTED:
		{
			struct {
				unsigned char ver;
				unsigned char nmethods;
				unsigned char method;
			} socks5_hello = {
				.ver = 5,
				.nmethods = 1,
				.method = 0x00 /* no auth */
				/* one could also implement username/password. If you know
				 * a jabber client or proxy that actually does it, tell me.
				 */
			};
			
			ASSERTSOCKOP( send( fd, &socks5_hello, sizeof( socks5_hello ) , 0 ), "Sending auth request" );

			bt->phase = BS_PHASE_REQUEST;

			bt->tf->watch_in = b_input_add( fd, GAIM_INPUT_READ, jabber_bs_recv_handshake, bt );

			bt->tf->watch_out = 0;
			return FALSE;
		}
	case BS_PHASE_REQUEST:
		{
			struct socks5_message socks5_connect = 
			{
				.ver = 5,
				.cmdrep.cmd = 0x01,
				.rsv = 0,
				.atyp = 0x03,
				.addrlen = strlen( bt->pseudoadr ),
				.port = 0
			};
			int ret;
			char buf[2];

			/* If someone's trying to be funny and sends only one byte at a time we'll fail :) */
			ASSERTSOCKOP( ret = recv( fd, buf, 2, 0 ) , "Receiving auth reply" );

			if( !( ret == 2 ) ||
			    !( buf[0] == 5 ) ||
			    !( buf[1] == 0 ) )
				return jabber_bs_abort( bt, "Auth not accepted by streamhost (reply: len=%d, ver=%d, status=%d)",
									ret, buf[0], buf[1] );

			/* copy hash into connect message */
			memcpy( socks5_connect.address, bt->pseudoadr, socks5_connect.addrlen );

			ASSERTSOCKOP( send( fd, &socks5_connect, sizeof( struct socks5_message ), 0 ) , "Sending SOCKS5 Connect" );

			bt->phase = BS_PHASE_REPLY;

			return TRUE;
		}
	case BS_PHASE_REPLY:
		{
			struct socks5_message socks5_reply;
			int ret;

			if ( !( ret = jabber_bs_peek( bt, &socks5_reply, sizeof( struct socks5_message ) ) ) )
				return FALSE;

			if ( ret < sizeof( socks5_reply ) )
				return TRUE;

			if( !( socks5_reply.ver == 5 ) ||
			    !( socks5_reply.cmdrep.rep == 0 ) ||
			    !( socks5_reply.atyp == 3 ) ||
			    !( socks5_reply.addrlen == 40 ) )
				return jabber_bs_abort( bt, "SOCKS5 CONNECT failed (reply: ver=%d, rep=%d, atyp=%d, addrlen=%d", 
					socks5_reply.ver,
					socks5_reply.cmdrep.rep,
					socks5_reply.atyp,
					socks5_reply.addrlen);

			if( bt->tf->ft->sending )
				jabber_bs_send_activate( bt );
			else
				jabber_bs_recv_answer_request( bt );

			return FALSE;
		}
	default:
		/* BUG */
		imcb_log( bt->tf->ic, "BUG in file transfer code: undefined handshake phase" );

		bt->tf->watch_in = 0;
		return FALSE;
	}
}

/*
 * If the handshake failed we can try the next streamhost, if there is one.
 * An intelligent sender would probably specify himself as the first streamhost and
 * a proxy as the second (Kopete and PSI are examples here). That way, a (potentially) 
 * slow proxy is only used if neccessary. This of course also means, that the timeout
 * per streamhost should be kept short. If one or two firewalled adresses are specified,
 * they have to timeout first before a proxy is tried.
 */
gboolean jabber_bs_recv_handshake_abort( struct bs_transfer *bt, char *error )
{
	struct jabber_transfer *tf = bt->tf;
	struct xt_node *reply, *iqnode;
	GSList *shlist;

	imcb_log( tf->ic, "Transferring file %s: connection to streamhost %s:%s failed (%s)", 
		  tf->ft->file_name, 
		  bt->sh->host,
		  bt->sh->port,
		  error );

	/* Alright, this streamhost failed, let's try the next... */
	bt->phase = BS_PHASE_CONNECT;
	shlist = g_slist_find( bt->streamhosts, bt->sh );
	if( shlist && shlist->next )
	{
		bt->sh = shlist->next->data;
		return jabber_bs_recv_handshake( bt, 0, 0 );
	}


	/* out of stream hosts */

	iqnode = jabber_make_packet( "iq", "result", tf->ini_jid, NULL );
	reply = jabber_make_error_packet( iqnode, "item-not-found", "cancel" , "404" );
	xt_free_node( iqnode );

	xt_add_attr( reply, "id", tf->iq_id );
		
	if( !jabber_write_packet( tf->ic, reply ) )
		imcb_log( tf->ic, "WARNING: Error transmitting bytestream response" );
	xt_free_node( reply );

	imcb_file_canceled( tf->ft, "couldn't connect to any streamhosts" );

	bt->tf->watch_in = 0;
	return FALSE;
}

/* 
 * After the SOCKS5 handshake succeeds we need to inform the initiator which streamhost we chose.
 * If he is the streamhost himself, he might already know that. However, if it's a proxy,
 * the initiator will have to make a connection himself.
 */
void jabber_bs_recv_answer_request( struct bs_transfer *bt )
{
	struct jabber_transfer *tf = bt->tf;
	struct xt_node *reply;

	imcb_log( tf->ic, "File %s: established SOCKS5 connection to %s:%s", 
		  tf->ft->file_name, 
		  bt->sh->host,
		  bt->sh->port );

	tf->ft->data = tf;
	tf->watch_in = b_input_add( tf->fd, GAIM_INPUT_READ, jabber_bs_recv_read, bt );
	tf->ft->write_request = jabber_bs_recv_write_request;

	reply = xt_new_node( "streamhost-used", NULL, NULL );
	xt_add_attr( reply, "jid", bt->sh->jid );

	reply = xt_new_node( "query", NULL, reply );
	xt_add_attr( reply, "xmlns", XMLNS_BYTESTREAMS );

	reply = jabber_make_packet( "iq", "result", tf->ini_jid, reply );

	xt_add_attr( reply, "id", tf->iq_id );
		
	if( !jabber_write_packet( tf->ic, reply ) )
		imcb_file_canceled( tf->ft, "Error transmitting bytestream response" );
	xt_free_node( reply );
}

/* 
 * This function is called from write_request directly. If no data is available, it will install itself
 * as a watcher for input on fd and once that happens, deliver the data and unschedule itself again.
 */
gboolean jabber_bs_recv_read( gpointer data, gint fd, b_input_condition cond )
{
	int ret;
	struct bs_transfer *bt = data;
	struct jabber_transfer *tf = bt->tf;

	if( fd != 0 ) /* called via event thread */
	{
		tf->watch_in = 0;
		ASSERTSOCKOP( ret = recv( fd, tf->ft->buffer, sizeof( tf->ft->buffer ), 0 ) , "Receiving" );
	}
	else
	{
		/* called directly. There might not be any data available. */
		if( ( ( ret = recv( tf->fd, tf->ft->buffer, sizeof( tf->ft->buffer ), 0 ) ) == -1 ) &&
		    ( errno != EAGAIN ) )
		    return jabber_bs_abort( bt, "Receiving: %s", strerror( errno ) );

		if( ( ret == -1 ) && ( errno == EAGAIN ) )
		{
			tf->watch_in = b_input_add( tf->fd, GAIM_INPUT_READ, jabber_bs_recv_read, bt );
			return FALSE;
		}
	}

	/* shouldn't happen since we know the file size */
	if( ret == 0 )
		return jabber_bs_abort( bt, "Remote end closed connection" );
	
	if( tf->bytesread == 0 )
		tf->ft->started = time( NULL );

	tf->bytesread += ret;

	tf->ft->write( tf->ft, tf->ft->buffer, ret );	

	return FALSE;
}

/* 
 * imc callback that is invoked when it is ready to receive some data.
 */
gboolean jabber_bs_recv_write_request( file_transfer_t *ft )
{
	struct jabber_transfer *tf = ft->data;

	if( tf->watch_in )
	{
		imcb_file_canceled( ft, "BUG in jabber file transfer: write_request called when already watching for input" );
		return FALSE;
	}
	
	jabber_bs_recv_read( tf->streamhandle, 0 , 0 );

	return TRUE;
}

/* 
 * Issues a write_request to imc.
 * */
gboolean jabber_bs_send_can_write( gpointer data, gint fd, b_input_condition cond )
{
	struct bs_transfer *bt = data;

	bt->tf->watch_out = 0;

	bt->tf->ft->write_request( bt->tf->ft );

	return FALSE;
}

/*
 * This should only be called if we can write, so just do it.
 * Add a write watch so we can write more during the next cycle (if possible).
 */
gboolean jabber_bs_send_write( file_transfer_t *ft, char *buffer, unsigned int len )
{
	struct jabber_transfer *tf = ft->data;
	struct bs_transfer *bt = tf->streamhandle;
	int ret;

	if( tf->watch_out )
		return jabber_bs_abort( bt, "BUG: write() called while watching " );
	
	/* TODO: catch broken pipe */
	ASSERTSOCKOP( ret = send( tf->fd, buffer, len, 0 ), "Sending" );

	if( tf->byteswritten == 0 )
		tf->ft->started = time( NULL );

	tf->byteswritten += ret;
	
	/* TODO: this should really not be fatal */
	if( ret < len )
		return jabber_bs_abort( bt, "send() sent %d instead of %d (send buffer too big!)", ret, len );

	bt->tf->watch_out = b_input_add( tf->fd, GAIM_INPUT_WRITE, jabber_bs_send_can_write, bt );
		
	return TRUE;
}

/*
 * Handles the reply by the receiver containing the used streamhost.
 */
static xt_status jabber_bs_send_handle_reply(struct im_connection *ic, struct xt_node *node, struct xt_node *orig ) {
	struct jabber_transfer *tf = NULL;
	struct jabber_data *jd = ic->proto_data;
	struct bs_transfer *bt;
	GSList *tflist;
	struct xt_node *c;
	char *sid, *jid;

	if( !( c = xt_find_node( node->children, "query" ) ) ||
	    !( c = xt_find_node( c->children, "streamhost-used" ) ) ||
	    !( jid = xt_find_attr( c, "jid" ) ) )

	{
		imcb_log( ic, "WARNING: Received incomplete bytestream reply" );
		return XT_HANDLED;
	}
	
	if( !( c = xt_find_node( orig->children, "query" ) ) ||
	    !( sid = xt_find_attr( c, "sid" ) ) )
	{
		imcb_log( ic, "WARNING: Error parsing request corresponding to the incoming bytestream reply" );
		return XT_HANDLED;
	}

	/* Let's see if we can find out what this bytestream should be for... */

	for( tflist = jd->filetransfers ; tflist; tflist = g_slist_next(tflist) )
	{
		struct jabber_transfer *tft = tflist->data;
		if( ( strcmp( tft->sid, sid ) == 0 ) )
		{
		    	tf = tft;
			break;
		}
	}

	if( !tf )
	{
		imcb_log( ic, "WARNING: Received SOCKS5 bytestream reply to unknown request" );
		return XT_HANDLED;
	}

	bt = tf->streamhandle;

	tf->accepted = TRUE;

	if( strcmp( jid, tf->ini_jid ) == 0 )
	{
		/* we're streamhost and target */
		if( bt->phase == BS_PHASE_REPLY )
		{
			/* handshake went through, let's start transferring */
			tf->ft->write_request( tf->ft );
		}
	} else
	{
		/* using a proxy, abort listen */

		closesocket( tf->fd );
		tf->fd = 0;

		if ( bt->connect_timeout )
		{
			b_event_remove( bt->connect_timeout );
			bt->connect_timeout = 0;
		}

		GSList *shlist;
		for( shlist = jd->streamhosts ; shlist ; shlist = g_slist_next( shlist ) )
		{
			jabber_streamhost_t *sh = shlist->data;
			if( strcmp( sh->jid, jid ) == 0 )
			{
				bt->sh = sh;
				jabber_bs_recv_handshake( bt, 0, 0 );
				return XT_HANDLED;
			}
		}

		imcb_log( ic, "WARNING: Received SOCKS5 bytestream reply with unknown streamhost %s", jid );
	}

	return XT_HANDLED;
}

/* 
 * Tell the proxy to activate the stream. Looks like this:
 *
 * <iq type=set>
 * 	<query xmlns=bs sid=sid>
 * 		<activate>tgt_jid</activate>
 * 	</query>
 * </iq>
 */
void jabber_bs_send_activate( struct bs_transfer *bt )
{
	struct xt_node *node;

	node = xt_new_node( "activate", bt->tf->tgt_jid, NULL );
	node = xt_new_node( "query", NULL, node );
	xt_add_attr( node, "xmlns", XMLNS_BYTESTREAMS );
	xt_add_attr( node, "sid", bt->tf->sid );
	node = jabber_make_packet( "iq", "set", bt->sh->jid, node );

	jabber_cache_add( bt->tf->ic, node, jabber_bs_send_handle_activate );

	jabber_write_packet( bt->tf->ic, node );
}

/*
 * The proxy has activated the bytestream.
 * We can finally start pushing some data out.
 */
static xt_status jabber_bs_send_handle_activate( struct im_connection *ic, struct xt_node *node, struct xt_node *orig )
{
	char *sid;
	GSList *tflist;
	struct jabber_transfer *tf;
	struct xt_node *query;
	struct jabber_data *jd = ic->proto_data;

	query = xt_find_node( orig->children, "query" );
	sid = xt_find_attr( query, "sid" );

	for( tflist = jd->filetransfers ; tflist; tflist = g_slist_next(tflist) )
	{
		struct jabber_transfer *tft = tflist->data;
		if( ( strcmp( tft->sid, sid ) == 0 ) )
		{
		    	tf = tft;
			break;
		}
	}

	if( !tf )
	{
		imcb_log( ic, "WARNING: Received SOCKS5 bytestream activation for unknown stream" );
		return XT_HANDLED;
	}

	/* handshake went through, let's start transferring */
	tf->ft->write_request( tf->ft );

	return XT_HANDLED;
}

/*
 * Starts a bytestream.
 */
gboolean jabber_bs_send_start( struct jabber_transfer *tf )
{
	char host[INET6_ADDRSTRLEN];
	struct bs_transfer *bt;
	sha1_state_t sha;
	char hash_hex[41];
	unsigned char hash[20];
	int i,ret;
	struct jabber_data *jd = tf->ic->proto_data;
	jabber_streamhost_t sh;
	GSList *streamhosts = jd->streamhosts;

	/* SHA1( SID + Initiator JID + Target JID ) is given to the streamhost which it will match against the initiator's value */
	sha1_init( &sha );
	sha1_append( &sha, (unsigned char*) tf->sid, strlen( tf->sid ) );
	sha1_append( &sha, (unsigned char*) tf->ini_jid, strlen( tf->ini_jid ) );
	sha1_append( &sha, (unsigned char*) tf->tgt_jid, strlen( tf->tgt_jid ) );
	sha1_finish( &sha, hash );
	
	for( i = 0; i < 20; i ++ )
		sprintf( hash_hex + i * 2, "%02x", hash[i] );
		
	bt = g_new0( struct bs_transfer, 1 );
	bt->tf = tf;
	bt->phase = BS_PHASE_CONNECT;
	bt->pseudoadr = g_strdup( hash_hex );
	tf->streamhandle = bt;
	tf->ft->free = jabber_bs_free_transfer;
	tf->ft->canceled = jabber_bs_canceled;

	if ( !jabber_bs_send_listen( bt, &tf->saddr, sh.host = host, sh.port ) )
		return FALSE;

	bt->tf->watch_in = b_input_add( tf->fd, GAIM_INPUT_READ, jabber_bs_send_handshake, bt );
	bt->connect_timeout = b_timeout_add( JABBER_BS_LISTEN_TIMEOUT * 1000, jabber_bs_connect_timeout, bt );

	sh.jid = tf->ini_jid;

	/* temporarily add listen address to streamhosts, send the request and remove it */
	streamhosts = g_slist_prepend( streamhosts, &sh );
	ret = jabber_bs_send_request( tf, streamhosts);
	streamhosts = g_slist_remove( streamhosts, &sh );

	return ret;
}

gboolean jabber_bs_send_request( struct jabber_transfer *tf, GSList *streamhosts )
{
	struct xt_node *shnode, *query, *iq;

	query = xt_new_node( "query", NULL, NULL );
	xt_add_attr( query, "xmlns", XMLNS_BYTESTREAMS );
	xt_add_attr( query, "sid", tf->sid );
	xt_add_attr( query, "mode", "tcp" );

	while( streamhosts ) {
		jabber_streamhost_t *sh = streamhosts->data;
		shnode = xt_new_node( "streamhost", NULL, NULL );
		xt_add_attr( shnode, "jid", sh->jid );
		xt_add_attr( shnode, "host", sh->host );
		xt_add_attr( shnode, "port", sh->port );

		xt_add_child( query, shnode );

		streamhosts = g_slist_next( streamhosts );
	}


	iq = jabber_make_packet( "iq", "set", tf->tgt_jid, query );
	xt_add_attr( iq, "from", tf->ini_jid );

	jabber_cache_add( tf->ic, iq, jabber_bs_send_handle_reply );

	if( !jabber_write_packet( tf->ic, iq ) )
		imcb_file_canceled( tf->ft, "Error transmitting bytestream request" );
	return TRUE;
}

gboolean jabber_bs_send_handshake_abort(struct bs_transfer *bt, char *error )
{
	struct jabber_transfer *tf = bt->tf;
	struct jabber_data *jd = tf->ic->proto_data;

	/* TODO: did the receiver get here somehow??? */
	imcb_log( tf->ic, "Transferring file %s: SOCKS5 handshake failed: %s", 
		  tf->ft->file_name, 
		  error );

	if( jd->streamhosts==NULL ) /* we're done here unless we have a proxy to try */
		imcb_file_canceled( tf->ft, error );

	return FALSE;
}

/*
 * Creates a listening socket and returns it in saddr_ptr.
 */
gboolean jabber_bs_send_listen( struct bs_transfer *bt, struct sockaddr_storage *saddr, char *host, char *port )
{
	struct jabber_transfer *tf = bt->tf;
	int fd;
	char hostname[ HOST_NAME_MAX + 1 ];
	struct addrinfo hints, *rp;
	socklen_t ssize = sizeof( struct sockaddr_storage );

	/* won't be long till someone asks for this to be configurable :) */

	ASSERTSOCKOP( gethostname( hostname, sizeof( hostname ) ), "gethostname()" );

	memset( &hints, 0, sizeof( struct addrinfo ) );
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;

	if ( getaddrinfo( hostname, "0", &hints, &rp ) != 0 )
		return jabber_bs_abort( bt, "getaddrinfo()" );

	memcpy( saddr, rp->ai_addr, rp->ai_addrlen );

	ASSERTSOCKOP( fd = tf->fd = socket( saddr->ss_family, SOCK_STREAM, 0 ), "Opening socket" );

	ASSERTSOCKOP( bind( fd, ( struct sockaddr *)saddr, rp->ai_addrlen ), "Binding socket" );
	
	freeaddrinfo( rp );

	ASSERTSOCKOP( listen( fd, 1 ), "Making socket listen" );

	if ( !inet_ntop( saddr->ss_family, saddr->ss_family == AF_INET ?
			( void * )&( ( struct sockaddr_in * ) saddr )->sin_addr.s_addr : ( void * )&( ( struct sockaddr_in6 * ) saddr )->sin6_addr.s6_addr
			, host, INET6_ADDRSTRLEN ) )
		return jabber_bs_abort( bt, "inet_ntop failed on listening socket" );

	ASSERTSOCKOP( getsockname( fd, ( struct sockaddr *)saddr, &ssize ), "Getting socket name" );

	if( saddr->ss_family == AF_INET )
		sprintf( port, "%d", ntohs( ( ( struct sockaddr_in *) saddr )->sin_port ) );
	else
		sprintf( port, "%d", ntohs( ( ( struct sockaddr_in6 *) saddr )->sin6_port ) );

	return TRUE;
}

/*
 * SOCKS5BYTESTREAM protocol for the sender
 */
gboolean jabber_bs_send_handshake( gpointer data, gint fd, b_input_condition cond )
{
	struct bs_transfer *bt = data;
	struct jabber_transfer *tf = bt->tf;
	short revents;

	if ( !jabber_bs_poll( bt, fd, &revents ) )
		return FALSE;
	
	switch( bt->phase ) 
	{
	case BS_PHASE_CONNECT:
		{
			struct sockaddr_storage clt_addr;
			socklen_t ssize = sizeof( clt_addr );
			
			/* Connect */

			ASSERTSOCKOP( tf->fd = accept( fd, (struct sockaddr *) &clt_addr, &ssize ), "Accepting connection" );

			closesocket( fd );
			fd = tf->fd;
			sock_make_nonblocking( fd );
			
			bt->phase = BS_PHASE_CONNECTED;

			bt->tf->watch_in = b_input_add( fd, GAIM_INPUT_READ, jabber_bs_send_handshake, bt );
			return FALSE;
		}
	case BS_PHASE_CONNECTED:
		{
			int ret, have_noauth=FALSE;
			struct {
				unsigned char ver;
				unsigned char method;
			} socks5_auth_reply = { .ver = 5, .method = 0 };
			struct {
				unsigned char ver;
				unsigned char nmethods;
				unsigned char method;
			} socks5_hello;

			if( !( ret = jabber_bs_peek( bt, &socks5_hello, sizeof( socks5_hello ) ) ) )
				return FALSE;

			if( ret < sizeof( socks5_hello ) )
				return TRUE;

			if( !( socks5_hello.ver == 5 ) ||
			    !( socks5_hello.nmethods >= 1 ) ||
			    !( socks5_hello.nmethods < 32 ) )
				return jabber_bs_abort( bt, "Invalid auth request ver=%d nmethods=%d method=%d", socks5_hello.ver, socks5_hello.nmethods, socks5_hello.method );

			have_noauth = socks5_hello.method == 0;

			if( socks5_hello.nmethods > 1 )
			{
				char mbuf[32];
				int i;
				ASSERTSOCKOP( ret = recv( fd, mbuf, socks5_hello.nmethods - 1, 0 ) , "Receiving auth methods" );
				if( ret < ( socks5_hello.nmethods - 1 ) )
					return jabber_bs_abort( bt, "Partial auth request");
				for( i = 0 ; !have_noauth && ( i < socks5_hello.nmethods - 1 ) ; i ++ )
					if( mbuf[i] == 0 )
						have_noauth = TRUE;
			}
			
			if( !have_noauth )
				return jabber_bs_abort( bt, "Auth request didn't include no authentication" );

			ASSERTSOCKOP( send( fd, &socks5_auth_reply, sizeof( socks5_auth_reply ) , 0 ), "Sending auth reply" );

			bt->phase = BS_PHASE_REQUEST;

			return TRUE;
		}
	case BS_PHASE_REQUEST:
		{
			struct socks5_message socks5_connect;
			int msgsize = sizeof( struct socks5_message );

			if( !jabber_bs_peek( bt, &socks5_connect, msgsize ) )
				return FALSE;

			if( !( socks5_connect.ver == 5) ||
			    !( socks5_connect.cmdrep.cmd == 1 ) ||
			    !( socks5_connect.atyp == 3 ) ||
			    !(socks5_connect.addrlen == 40 ) )
				return jabber_bs_abort( bt, "Invalid SOCKS5 Connect message (addrlen=%d, ver=%d, cmd=%d, atyp=%d)", socks5_connect.addrlen, socks5_connect.ver, socks5_connect.cmdrep.cmd, socks5_connect.atyp );
			if( !( memcmp( socks5_connect.address, bt->pseudoadr, 40 ) == 0 ) )
				return jabber_bs_abort( bt, "SOCKS5 Connect message contained wrong digest");

			socks5_connect.cmdrep.rep = 0;

			ASSERTSOCKOP( send( fd, &socks5_connect, msgsize, 0 ), "Sending connect reply" );

			bt->phase = BS_PHASE_REPLY;

			imcb_log( tf->ic, "File %s: SOCKS5 handshake successful! Transfer about to start...", tf->ft->file_name );

			if( tf->accepted )
			{
				/* streamhost-used message came already in(possible?), let's start sending */
				tf->ft->write_request( tf->ft );
			}

			tf->watch_in = 0;
			return FALSE;

		}
	default:
		/* BUG */
		imcb_log( bt->tf->ic, "BUG in file transfer code: undefined handshake phase" );

		bt->tf->watch_in = 0;
		return FALSE;
	}
}
#undef ASSERTSOCKOP
