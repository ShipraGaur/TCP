#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include "rlib.h"

#include <stdbool.h>
#include "packet_list.c"
#include "constants.h"

// TODO:
// - multiple connections


// The mapping from rel_t to conn_t is one-to-one; for every connection, there is one
// rel_t and one conn_t instance.
// rel_t also contains a linked list for traversing all connections
struct reliable_state {
	rel_t *next; /* Linked list for traversing all connections */
	rel_t **prev;
	conn_t *c; /* This is the connection object */
	/* Add your own data fields below this */
	/**
	 * This consists of the data that has been sent but not acknowledged.
	 * Data that has been acknowledged is not included in the send buffer.
	 *
	 * The send buffer stores data in packets ordered ascending by their
	 * sequence number.
	 */
	packet_list* send_buffer;
	/**
	 * The next sequence number to send with a packet
	 */
	unsigned int next_seqno_to_send;

	/**
	 * This consists of the data that has not been read by the application yet.
	 * Data that has been read by the application is not included in the receive buffer.
	 *
	 * The receive buffer stores data in packets ordered ascending by their
	 * sequence number.
	 *
	 * The first half consists of the contiguous data that has been received,
	 * and the second half consists of data that is not yet contiguous
	 */
	packet_list* receive_buffer;
	/**
	 * The sequence number of the lowest packet that could be received next in
	 * the receive buffer
	 */
	unsigned int next_seqno_expected;

	/**
	 * The configuration parameters passed from the user
	 */
	const struct config_common *config;

	//flags for calling rel_destroy; check if all equal 1, and if so, call rel_destroy
	uint8_t eof_other_side;
	uint8_t eof_conn_input;
	uint8_t eof_all_acked;
	uint8_t eof_conn_output;
};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* Exactly one of c and ss should be NULL. (ss is NULL when called
* from rlib.c, while c is NULL when this function is called from
* rel_demux.) */
// 1) if this is a new connection the conn_t is NULL
// 2) if the connection has already been created,
// then there is no need for the sockaddr_storage, so it will be NULL
// During startup, an initial connection is created for you, leading to 2)
// During runtime, if a new connection is created, then you have to deal with 1)
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc)
{
	rel_t *r;
	r = xmalloc (sizeof (*r));
	memset (r, 0, sizeof (*r));
	if (!c) {
		c = conn_create (r, ss);
		if (!c) {
		free (r);
		return NULL;
		}
	}
	r->c = c;
	// wtf / s > 1
	//r->next = rel_list;
	//r->prev = &rel_list;
	//if (rel_list)
	//	rel_list->prev = &r->next;
	//rel_list = r;
	// advance the list pointer forward
	r->next = NULL;
	if (rel_list) {
		r->prev = &rel_list;
		rel_list->next = r;
	}
	else {
		r->prev = NULL;
	}
	rel_list = r;
	/* Do any other initialization you need here */
	r->send_buffer = NULL;
	r->next_seqno_to_send = 1;
	r->receive_buffer = NULL;
	r->next_seqno_expected = 1;
	r->config = cc;
	r->eof_other_side = 0;
	r->eof_conn_input = 0;
	r->eof_all_acked = 0;
	r->eof_conn_output = 0;
	return r;
}

void
rel_destroy (rel_t *r)
{
	if (r->next)
	r->next->prev = r->prev;
	*r->prev = r->next;
	conn_destroy (r->c);
	/* Free any other allocated memory here */
	while (r->send_buffer) {
		remove_head_packet(&r->send_buffer);
	}
	while (r->receive_buffer) {
		remove_head_packet(&r->receive_buffer);
	}
}

/* This function only gets called when the process is running as a
* server and must handle connections from multiple clients. You have
* to look up the rel_t structure based on the address in the
* sockaddr_storage passed in. If this is a new connection (sequence
* number 1), you will need to allocate a new conn_t using rel_create
* (). (Pass rel_create NULL for the conn_t, so it will know to
* allocate a new connection.)
*/
// Note: This is only called in server mode, i.e. when you supply the -s option when running
// This will add a new node to the linked list if this is a new connection
// if not, ???
void
rel_demux (const struct config_common *cc, const struct sockaddr_storage *ss, packet_t *pkt, size_t len)
{
}

void send_ack(rel_t *r, uint32_t ackno) {
	size_t ack_packet_size = sizeof(struct ack_packet);
	struct ack_packet* ack = (struct ack_packet*) malloc(ack_packet_size);
	memset(ack, 0, ack_packet_size);
	ack->len = (uint16_t) ack_packet_size;
	ack->ackno = (uint32_t) ackno;
	ack->cksum = cksum((void *)ack, ack_packet_size);
	conn_sendpkt(r->c, (packet_t *)ack, ack_packet_size);
	free(ack);
	return;
}

// For receiving packets; these are either ACKs (for sending) or data packets (for receiving)
// For receiving: read in and buffer the packets so they can be consumed by by rel_output;
// send ACKs for the packets buffered
// For sending: update how many unACKed packets there are
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	// first check validity
	unsigned int check = pkt->cksum;
	pkt->cksum = 0;
	if (cksum(pkt, n)!=check)	return;
	
	if(n == 8){ // Ack only
		packet_list* send_buffer_head = r->send_buffer; // this assume that the pointer directly points to the true head
		while(send_buffer_head->packet->ackno != pkt->seqno && send_buffer_head !=NULL){
			remove_head_packet(&send_buffer_head);
		}
	} 
	else if (n >= 12 && pkt->seqno > 0){ //ack and data
		insert_packet_in_order(&(r->receive_buffer), pkt->seqno);
		if(pkt->seqno == r->next_seqno_expected){
			r->next_seqno_expected++;
			send_ack(r, r->next_seqno_expected);
		}

		packet_list* send_buffer_head = r->send_buffer; // piggy ack
		while(send_buffer_head->packet->ackno < pkt->seqno && send_buffer_head !=NULL){
			remove_head_packet(&send_buffer_head);
		}
		if(n == 12){
			r->eof_other_side = 1;
			if(r->next_seqno_to_send == pkt->ackno)	r->eof_all_acked = 1;
		}
	}
	if (r->eof_other_side && r->eof_conn_input && 
		r->eof_all_acked && r->eof_conn_output) {
		rel_destroy(r);
	}
	return;
}

void
rel_read (rel_t *s)
{
	if (!s) {
		return;
	}
	packet_list* send_buffer = s->send_buffer;
	int window_size = s->config->window;
	while (packet_list_size(send_buffer) < window_size) {
		packet_list* packet_node = new_packet();
		int bytes_read = conn_input(s->c, packet_node->packet->data, MAX_PACKET_DATA_SIZE);
		if (bytes_read == 0) {
			break;
		}
		if(bytes_read < 0){
			s->eof_conn_input = 1;
			bytes_read = 0;
		}
		packet_node->packet->cksum = 0;
		int packet_length = DATA_PACKET_METADATA_LENGTH + bytes_read;
		packet_node->packet->len = packet_length;
		packet_node->packet->ackno = s->next_seqno_expected - 1;
		packet_node->packet->seqno = s->next_seqno_to_send;
		uint16_t checksum = cksum(packet_node->packet, packet_length);
		packet_node->packet->cksum = checksum;
		s->next_seqno_to_send++;

		conn_sendpkt(s->c, packet_node->packet, packet_length);
		append_packet(&s->send_buffer, packet_node);
	}
	if (s->eof_other_side && s->eof_conn_input &&
		s->eof_all_acked && s->eof_conn_output) {
		rel_destroy(s);
	}
}

// Consume the packets buffered by rel_recvpkt; call conn_bufspace to see how much data
// you can flush, and flush using conn_output
void rel_output (rel_t *r) {
	int check = conn_bufspace(r->c);
	int total = packet_data_size(r->receive_buffer, r->next_seqno_expected);
	if (check == 0) {
		printf("Not enough space in output\n");
		return;
	}
	size_t size = (size_t) (check < total) ? check : total;
	char *buf = malloc(size);
	packet_list *list = r->receive_buffer;
	int packets_written;
	int last_packet_offset;
	serialize_packet_data(buf, size, r->next_seqno_expected, list,
			&packets_written, &last_packet_offset);

	conn_output(r->c, buf, (int) size);
	if (last_packet_offset != 0) {
		packets_written--;
	}
	int i = 0;
	for (i = 0; i < packets_written; i++) {
		remove_head_packet(&r->receive_buffer);
	}
	r->eof_conn_output = 1;
	if (r->eof_other_side && r->eof_conn_input &&
		r->eof_all_acked && r->eof_conn_output) {
		//there has to be a better way to do this
		rel_destroy(r);
	}
	return;
}

void resend_packets(rel_t *rel) {
	packet_list* packets_iter = rel->send_buffer;
	while (packets_iter && packets_iter->packet) {
		conn_sendpkt(rel->c, packets_iter->packet, packets_iter->packet->len);
		packets_iter = packets_iter->next;
	}
}

// Retransmit any unACKed packets after a certain amount of time
void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	if (rel_list) {
		resend_packets(rel_list);
	}
	rel_t *rel_list_fwd = rel_list;
	while (rel_list_fwd->next) {
		rel_list_fwd = rel_list_fwd->next;
		resend_packets(rel_list_fwd);
	}
	rel_t *rel_list_bwd = rel_list;
	while (rel_list_bwd->prev && *(rel_list_bwd->prev)) {
		rel_list_bwd = *(rel_list_bwd->prev);
		resend_packets(rel_list_bwd);
	}
}
