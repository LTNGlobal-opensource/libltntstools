/**
 * @file        kl-queue.h
 * @author      Steven Toth <stoth@kernellabs.com>
 * @copyright   Copyright (c) 2017-2021 Kernel Labs Inc. All Rights Reserved.
 * @brief       A thread-safe mechanism for queue items between processes.
 */

#ifndef KL_QUEUE_H
#define KL_QUEUE_H

#include <pthread.h>
#include <stdint.h>
#include <libltntstools/xorg-list.h>

/**
 * @brief	Initialize and track a queue of pointers in a FIFO type arrangement.
 *		The implementation doesn't care about the pointer being tracked, and
 *		make no attempt to "manage" the pointer storage.
 *		The implementation is best suited to application that push items
 *		every 1ms or so, such to the overhead of calling malloc() on each push.
 *		For projects that require usec accuracy, suggest pre-allocating
 *		a list of free items.
 */
struct klqueue_item_s
{
	struct xorg_list list;
	void *data;
};

/**
 * @brief	Initialize and track a queue of pointers in a FIFO type arrangement.
 */
struct klqueue_s
{
	/* Private, users should not inspect. */
	pthread_mutex_t  mutex;
	pthread_cond_t   item_add;
	pthread_cond_t   item_remove;
	struct xorg_list head;
	uint64_t         item_count;
};

/**
 * @brief	    Initialize an existing allocation.
 * @param[in]	struct klqueue_s *q) - Brief description goes here.
 */
void klqueue_initialize(struct klqueue_s *q);

/**
 * @brief	    TODO - Brief description goes here.
 * @param[in]	struct klqueue_s *q - Brief description goes here.
 * @return	    number of items on the queue.
 */
uint64_t klqueue_count(struct klqueue_s *q);

/**
 * @brief	    TODO - Brief description goes here.
 * @param[in]	struct klqueue_s *q - Brief description goes here.
 * @return      0 - Success, else < 0 on error.
 */
int  klqueue_empty(struct klqueue_s *q);

/**
 * @brief	    Delete any items in the queue, prepare to abandon the queue.
 * @param[in]	struct klqueue_s *q - Brief description goes here.
 */
void klqueue_destroy(struct klqueue_s *q);

/**
 * @brief	    Push the ptr 'item' on to the queue.
 * @param[in]	struct klqueue_s *q - Brief description goes here.
 * @param[in]	void *item - Brief description goes here.
 */
void klqueue_push(struct klqueue_s *q, void *item);

/**
 * @brief	    Blocking call that times out after n period, dequeue an item.
 * @param[in]	struct klqueue_s *q - Brief description goes here.
 * @param[in]	int usec - Brief description goes here.
 * @param[in]	void **item - Brief description goes here.
 * @return      0 - Success, else < 0 on error.
 */
int  klqueue_pop_non_blocking(struct klqueue_s *q, int usec, void **item);

#endif /* KL_QUEUE_H */
