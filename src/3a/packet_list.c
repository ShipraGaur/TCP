#include <stdlib.h>

#define MAX_PACKET_SIZE 512
#define PACKET_METADATA_LENGTH 12

/**
 * A doubly-linked list with a malloc'd packet as the payload
 */
typedef struct packet_list {
	/**
	 * The next element in the list. NULL if the last element.
	 */
	struct packet_list *next;
	/**
	 * The previous element in the list. NULL if the first element.
	 */
	struct packet_list *prev;
	/**
	 * The malloc'd location of the packet
	 */
	packet_t *packet;
} packet_list;

/**
 * Create a new, unlinked packet node
 */
packet_list* new_packet() {
	packet_list *new = (packet_list*) malloc(sizeof(packet_list));
	new->next = NULL;
	new->prev = NULL;
	new->packet = (packet_t*) malloc(MAX_PACKET_SIZE);
	return new;
}

/**
 * Delete the node at this location in the list and modify the list to point
 * to the next entry, which may be NULL
 */
int remove_head_packet(packet_list** list) {
	if (!list) {
		return -1;
	}
	if (!(*list)) {
		return 0;
	}
	if ((*list)->packet) {
		free((*list)->packet);
	}
	packet_list* new_head = (*list)->next;
	if (new_head) {
		new_head->prev = NULL;
	}
	free(*list);
	*list = new_head;
	return 0;
}

/**
 * Insert a packet after the node pointed to in the list; if the node
 * pointed to is NULL, then the list will be initialized with that packet
 */
int insert_packet_after(packet_list** list, packet_list* packet) {
	if (!packet || !list) {
		return -1;
	}
	if (!(*list)) {
		*list = packet;
		return 0;
	}
	packet_list* old_next = (*list)->next;
	(*list)->next = packet;
	packet->prev = *list;
	packet->next = old_next;
	if (old_next) {
		old_next->prev = packet;
	}
	return 0;
}

/**
 * Insert a packet after the first node with a certain sequence number;
 * do nothing if the sequence number is not found, or if the node pointed
 * to in the list is NULL
 */
int insert_packet_after_seqno(
		packet_list** list, packet_list* packet, unsigned int seqno) {
	if (!packet || !list || !(*list)) {
		return -1;
	}
	packet_list* list_iter = *list;
	while (list_iter) {
		if (list_iter->packet && list_iter->packet->seqno == seqno) {
			 return insert_packet_after(&list_iter, packet);
		}
		list_iter = list_iter->next;
	}
	return -1;
}

/**
 * Insert a packet at the end of the list; if the node pointed to in the list
 * is NULL, then initialize the list with that packet
 */
int append_packet(packet_list** list, packet_list* packet) {
	if (!packet || !list) {
		return -1;
	}
	if (!(*list)) {
		*list = packet;
		return 0;
	}
	packet_list* list_iter = *list;
	while (list_iter->next) {
		list_iter = list_iter->next;
	}
	return insert_packet_after(&list_iter, packet);
}

// tail recursion for the lols
int packet_list_size_acc(packet_list* list, int current_size) {
	if (!list) {
		return current_size;
	}
	return packet_list_size_acc(list->next, current_size + 1);
}

/**
 * Return the size of the list, starting from this point onwards
 */
int packet_list_size(packet_list* list) {
	return packet_list_size_acc(list, 0);
}

int packet_data_size(packet_list* list) {
	int data_size = 0;
	while (list) {
		if (list->packet) {
			data_size += list->packet->len - PACKET_METADATA_LENGTH;
		}
		list = list->next;
	}
	return data_size;
}

void serialize_packet_data(char* buffer, packet_list* list) {
	char* buffer_iter = buffer;
	while (list) {
		if (list->packet) {
			uint16_t amountToCopy = list->packet->len - PACKET_METADATA_LENGTH;
			memcpy(buffer_iter, list->packet->data, amountToCopy);
			buffer_iter += amountToCopy;
		}
		list = list->next;
	}
}
