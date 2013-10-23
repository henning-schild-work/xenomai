/*
 * Copyright (C) 2011 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 *
 * @defgroup alchemy_queue Message queue services.
 * @ingroup alchemy_queue
 * @ingroup alchemy
 *
 * Queue services.
 *
 * Message queueing is a method by which real-time tasks can exchange
 * or pass data through a Xenomai-managed queue of messages. Messages
 * can vary in length and be assigned different types or usages. A
 * message queue can be created by one task and used by multiple tasks
 * that send and/or receive messages to the queue.
 */

#include <errno.h>
#include <string.h>
#include <copperplate/threadobj.h>
#include <copperplate/heapobj.h>
#include "reference.h"
#include "internal.h"
#include "queue.h"
#include "timer.h"

struct syncluster alchemy_queue_table;

static DEFINE_NAME_GENERATOR(queue_namegen, "queue",
			     struct alchemy_queue, name);

DEFINE_SYNC_LOOKUP(queue, RT_QUEUE);

static void queue_finalize(struct syncobj *sobj)
{
	struct alchemy_queue *qcb;

	qcb = container_of(sobj, struct alchemy_queue, sobj);
	heapobj_destroy(&qcb->hobj);
	xnfree(qcb);
}
fnref_register(libalchemy, queue_finalize);

/**
 * @fn int rt_queue_create(RT_QUEUE *q, const char *name, size_t poolsize, size_t qlimit, int mode)
 * @brief Create a message queue.
 *
 * Create a message queue object which allows multiple tasks to
 * exchange data through the use of variable-sized messages. A message
 * queue is created empty.
 *
 * This service needs the special character device /dev/rtheap
 * (10,254) when called from user-space tasks.
 *
 * @param q The address of a queue descriptor which can be later used
 * to identify uniquely the created object, upon success of this call.
 *
 * @param name An ASCII string standing for the symbolic name of the
 * queue. When non-NULL and non-empty, a copy of this string is used
 * for indexing the created queue into the object registry.
 *
 * @param poolsize The size (in bytes) of the message buffer pool to
 * be pre-allocated for holding messages. Message buffers will be
 * claimed and released to this pool.  The buffer pool memory cannot
 * be extended. See note.
 *
 * @param mode The queue creation mode. The following flags can be
 * OR'ed into this bitmask, each of them affecting the new queue:
 *
 * - Q_FIFO makes tasks pend in FIFO order on the queue for consuming
 * messages.
 *
 * - Q_PRIO makes tasks pend in priority order on the queue.
 *
 * @return Zero is returned upon success. Otherwise:
 *
 * - -ENOMEM is returned if the system fails to get memory from the
 * main heap in order to create the queue.
 *
 * - -EEXIST is returned if the @a name is conflicting with an already
 * registered queue.
 *
 * - -EPERM is returned if this service was called from an
 * asynchronous context.
 *
 * Valid calling context:
 *
 * - Regular POSIX threads
 * - Xenomai threads
 *
 * @note Queues can be shared by multiple processes which belong to
 * the same Xenomai session.
 *
 * @note Each message pending into the queue consumes four long words
 * plus the actual payload size, aligned to the next long word
 * boundary. e.g. a 6 byte message on a 32 bit platform would require
 * 24 bytes of storage into the pool.
 *
 * When @a qlimit is given (i.e. different from Q_UNLIMITED), this
 * overhead is accounted for automatically, so that @a qlimit messages
 * of @a poolsize / @a qlimit bytes can be stored into the pool
 * concurrently. Otherwise, @a poolsize is increased by 5% internally
 * to cope with such overhead.
 */
int rt_queue_create(RT_QUEUE *queue, const char *name,
		    size_t poolsize, size_t qlimit, int mode)
{
	struct alchemy_queue *qcb;
	int sobj_flags = 0, ret;
	struct service svc;

	if (threadobj_irq_p())
		return -EPERM;

	if (poolsize == 0 || (mode & ~Q_PRIO) != 0)
		return -EINVAL;

	CANCEL_DEFER(svc);

	ret = -ENOMEM;
	qcb = xnmalloc(sizeof(*qcb));
	if (qcb == NULL)
		goto out;

	generate_name(qcb->name, name, &queue_namegen);
	/*
	 * The message pool has to be part of the main heap for proper
	 * sharing between processes.
	 *
	 * We have the message descriptor overhead to cope with when
	 * allocating the buffer pool. When the queue limit is not
	 * known, assume 5% overhead.
	 */
	if (qlimit == Q_UNLIMITED)
		ret = heapobj_init(&qcb->hobj, qcb->name,
				   poolsize + (poolsize / 5));
	else
		ret = heapobj_init_array(&qcb->hobj, qcb->name,
					 (poolsize / qlimit) *
					 sizeof(struct alchemy_queue_msg),
					 qlimit);
	if (ret) {
		xnfree(qcb);
		goto out;
	}

	qcb->magic = queue_magic;
	qcb->mode = mode;
	qcb->limit = qlimit;
	list_init(&qcb->mq);
	qcb->mcount = 0;

	if (mode & Q_PRIO)
		sobj_flags = SYNCOBJ_PRIO;

	syncobj_init(&qcb->sobj, CLOCK_COPPERPLATE, sobj_flags,
		     fnref_put(libalchemy, queue_finalize));

	ret = 0;
	if (syncluster_addobj(&alchemy_queue_table, qcb->name, &qcb->cobj)) {
		heapobj_destroy(&qcb->hobj);
		syncobj_uninit(&qcb->sobj);
		xnfree(qcb);
		ret = -EEXIST;
	} else
		queue->handle = mainheap_ref(qcb, uintptr_t);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn int rt_queue_delete(RT_QUEUE *q)
 * @brief Delete a message queue.
 *
 * This routine deletes a queue object previously created by a call to
 * rt_queue_create(). All resources attached to that queue are
 * automatically released, including all pending messages.
 *
 * @param q The descriptor address of the deleted queue.
 *
 * @return Zero is returned upon success. Otherwise:
 *
 * - -EINVAL is returned if @a q is not a valid queue descriptor.
 *
 * - -EPERM is returned if this service was called from an
 * asynchronous context.
 *
 * Valid calling context:
 *
 * - Regular POSIX threads
 * - Xenomai threads
 */
int rt_queue_delete(RT_QUEUE *queue)
{
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	int ret = 0;

	if (threadobj_irq_p())
		return -EPERM;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	syncluster_delobj(&alchemy_queue_table, &qcb->cobj);
	qcb->magic = ~queue_magic;
	syncobj_destroy(&qcb->sobj, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn void *rt_queue_alloc(RT_QUEUE *q, size_t size)
 * @brief Allocate a message buffer.
 *
 * This service allocates a message buffer from the queue's internal
 * pool. This buffer can be filled in with payload information, prior
 * enqueuing it by a call to rt_queue_send().  When used in pair,
 * these services provide a zero-copy interface for sending messages.
 *
 * @param q The descriptor address of the queue to allocate a buffer
 * from.
 *
 * @param size The requested size in bytes of the buffer. Zero is an
 * acceptable value, which means that the message conveys no payload;
 * in this case, the receiver will get a zero-sized message.
 *
 * @return The address of the allocated buffer upon success, or NULL
 * if the call fails.
 *
 * Valid calling context: any.
 */
void *rt_queue_alloc(RT_QUEUE *queue, size_t size)
{
	struct alchemy_queue_msg *msg = NULL;
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	int ret;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	msg = heapobj_alloc(&qcb->hobj, size + sizeof(*msg));
	if (msg == NULL)
		goto done;

	/*
	 * XXX: no need to init the ->next holder, list_*pend() do not
	 * require this, and this ends up being costly on low end.
	 */
	msg->size = size;	/* Zero is allowed. */
	msg->refcount = 1;
	++msg;
done:
	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return msg;
}

/**
 * @fn int rt_queue_free(RT_QUEUE *q, void *buf)
 * @brief Free a message buffer.
 *
 * This service releases a message buffer to the queue's internal
 * pool.
 *
 * @param q The descriptor address of the queue to release a buffer
 * to.
 *
 * @param buf The address of the message buffer to free. Even
 * zero-sized messages carrying no payload data must be freed, since
 * they are assigned a valid memory space to store internal
 * information.
 *
 * @return Zero is returned upon success, or -EINVAL if @a buf is not
 * a valid message buffer previously allocated by the rt_queue_alloc()
 * service, or the caller did not get ownership of the message through
 * a successful return from rt_queue_receive().
 *
 * Valid calling context: any.
 */
int rt_queue_free(RT_QUEUE *queue, void *buf)
{
	struct alchemy_queue_msg *msg;
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	int ret = 0;

	if (buf == NULL)
		return -EINVAL;

	msg = (struct alchemy_queue_msg *)buf - 1;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	if (heapobj_validate(&qcb->hobj, msg) == 0) {
		ret = -EINVAL;
		goto done;
	}

	/*
	 * Check the reference count under lock, so that we properly
	 * serialize with rt_queue_send() and rt_queue_receive() which
	 * may update it.
	 */
	if (msg->refcount == 0) { /* Mm, double-free? */
		ret = -EINVAL;
		goto done;
	}

	if (--msg->refcount == 0)
		heapobj_free(&qcb->hobj, msg);
done:
	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn int rt_queue_send(RT_QUEUE *q, const void *buf, size_t size, int mode)
 * @brief Send a message to a queue.
 *
 * This service sends a complete message to a given queue. The message
 * must have been allocated by a previous call to rt_queue_alloc().
 *
 * @param q The descriptor address of the message queue to send to.
 *
 * @param buf The address of the message buffer to be sent.  The
 * message buffer must have been allocated using the rt_queue_alloc()
 * service.  Once passed to rt_queue_send(), the memory pointed to by
 * @a buf is no more under the control of the sender and thus should
 * not be referenced by it anymore; deallocation of this memory must
 * be handled on the receiving side.
 *
 * @param size The actual size in bytes of the message, which may be
 * lower than the allocated size for the buffer obtained from
 * rt_queue_alloc(). Zero is a valid value, in which case an empty
 * message will be sent.
 *
 * @param mode A set of flags affecting the operation:
 *
 * - Q_URGENT causes the message to be prepended to the message queue,
 * ensuring a LIFO ordering.
 *
 * - Q_NORMAL causes the message to be appended to the message queue,
 * ensuring a FIFO ordering.
 *
 * - Q_BROADCAST causes the message to be sent to all tasks currently
 * waiting for messages. The message is not copied; a reference count
 * is maintained instead so that the message will remain valid until
 * the last receiver releases its own reference using rt_queue_free(),
 * after which the message space will be returned to the queue's
 * internal pool.
 *
 * @return Upon success, this service returns the number of receivers
 * which got awaken as a result of the operation. If zero is returned,
 * no task was waiting on the receiving side of the queue, and the
 * message has been enqueued. Upon error, one of the following error
 * codes is returned:
 *
 * - -EINVAL is returned if @a q is not a message queue descriptor, or
 * @a buf is not a valid message buffer obtained from a previous call
 * to rt_queue_alloc().
 *
 * - -ENOMEM is returned if queuing the message would exceed the limit
 * defined for the queue at creation.
 *
 * Valid calling context: any.
 */
int rt_queue_send(RT_QUEUE *queue,
		  const void *buf, size_t size, int mode)
{
	struct alchemy_queue_wait *wait;
	struct alchemy_queue_msg *msg;
	struct alchemy_queue *qcb;
	struct threadobj *waiter;
	struct syncstate syns;
	struct service svc;
	int ret = 0;

	if (buf == NULL)
		return -EINVAL;

	msg = (struct alchemy_queue_msg *)buf - 1;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	if (qcb->limit && qcb->mcount >= qcb->limit) {
		ret = -ENOMEM;
		goto done;
	}

	if (msg->refcount == 0) {
		ret = -EINVAL;
		goto done;
	}

	msg->refcount--;
	msg->size = size;
	ret = 0;  /* # of tasks unblocked. */

	do {
		waiter = syncobj_grant_one(&qcb->sobj);
		if (waiter == NULL)
			break;
		wait = threadobj_get_wait(waiter);
		wait->msg = msg;
		msg->refcount++;
		ret++;
	} while (mode & Q_BROADCAST);

	if (ret)
		goto done;
	/*
	 * We need to queue the message if no task was waiting for it,
	 * except in broadcast mode, in which case we only fix up the
	 * reference count.
	 */
	if (mode & Q_BROADCAST)
		msg->refcount++;
	else {
		qcb->mcount++;
		if (mode & Q_URGENT)
			list_prepend(&msg->next, &qcb->mq);
		else
			list_append(&msg->next, &qcb->mq);
	}
done:
	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn int rt_queue_write(RT_QUEUE *q, const void *buf, size_t size)
 * @brief Write data to a queue.
 *
 * This service builds a message out of a raw data buffer, then send
 * it to a given queue.
 *
 * @param q The descriptor address of the message queue to write to.
 *
 * @param buf The address of the payload data to be written to the
 * queue. The payload is copied to a message buffer allocated
 * internally by this service.
 *
 * @param size The size in bytes of the payload data.  Zero is a valid
 * value, in which case an empty message is queued.
 *
 * @param mode A set of flags affecting the operation:
 *
 * - Q_URGENT causes the message to be prepended to the message queue,
 * ensuring a LIFO ordering.
 *
 * - Q_NORMAL causes the message to be appended to the message queue,
 * ensuring a FIFO ordering.
 *
 * - Q_BROADCAST causes the message to be sent to all tasks currently
 * waiting for messages. The message is not copied; a reference count
 * is maintained instead so that the message will remain valid until
 * the last receiver releases its own reference using rt_queue_free(),
 * after which the message space will be returned to the queue's
 * internal pool.
 *
 * @return Upon success, this service returns the number of receivers
 * which got awaken as a result of the operation. If zero is returned,
 * no task was waiting on the receiving side of the queue, and the
 * message has been enqueued. Upon error, one of the following error
 * codes is returned:
 *
 * - -EINVAL is returned if @a q is not a message queue descriptor.
 *
 * - -ENOMEM is returned if queuing the message would exceed the limit
 * defined for the queue at creation, or if no memory can be obtained
 * to convey the message data internally.
 *
 * Valid calling context: any.
 */
int rt_queue_write(RT_QUEUE *queue,
		   const void *buf, size_t size, int mode)
{
	struct alchemy_queue_wait *wait;
	struct alchemy_queue_msg *msg;
	struct alchemy_queue *qcb;
	struct threadobj *waiter;
	struct syncstate syns;
	int ret = 0, nwaiters;
	struct service svc;
	size_t usersz;

	if (size == 0)
		return 0;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	waiter = syncobj_peek_grant(&qcb->sobj);
	if (waiter && threadobj_local_p(waiter)) {
		/*
		 * Fast path for local threads already waiting for
		 * data via rt_queue_read(): do direct copy to the
		 * reader's buffer.
		 */
		wait = threadobj_get_wait(waiter);
		usersz = wait->usersz;
		if (usersz == 0)
			/* no buffer provided, enqueue normally. */
			goto enqueue;
		if (size > usersz)
			size = usersz;
		if (size > 0)
			memcpy(wait->userbuf, buf, size);
		wait->usersz = size;
		syncobj_grant_to(&qcb->sobj, waiter);
		ret = 1;
		goto done;
	}

enqueue:
	nwaiters = syncobj_count_grant(&qcb->sobj);
	if (nwaiters == 0 && (mode & Q_BROADCAST) != 0)
		goto done;

	ret = -ENOMEM;
	if (qcb->limit && qcb->mcount >= qcb->limit)
		goto done;

	msg = heapobj_alloc(&qcb->hobj, size + sizeof(*msg));
	if (msg == NULL)
		goto done;

	msg->size = size;
	msg->refcount = 0;
	memcpy(msg + 1, buf, size);

	ret = 0;  /* # of tasks unblocked. */
	if (nwaiters == 0) {
		qcb->mcount++;
		if (mode & Q_URGENT)
			list_prepend(&msg->next, &qcb->mq);
		else
			list_append(&msg->next, &qcb->mq);
		goto done;
	}

	do {
		waiter = syncobj_grant_one(&qcb->sobj);
		if (waiter == NULL)
			break;
		wait = threadobj_get_wait(waiter);
		wait->msg = msg;
		msg->refcount++;
		ret++;
	} while (mode & Q_BROADCAST);
done:
	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn ssize_t rt_queue_receive_until(RT_QUEUE *q, void **bufp, RTIME timeout)
 * @brief Receive a message from a queue (with absolute timeout date).
 *
 * This service receives the next available message from a given
 * queue.
 *
 * @param q The descriptor address of the message queue to receive
 * from.
 *
 * @param bufp A pointer to a memory location which will be written
 * with the address of the received message, upon success. Once
 * consumed, the message space should be freed using rt_queue_free().
 *
 * @param timeout An absolute date expressed in clock ticks,
 * specifying a time limit to wait for a message to be available from
 * the queue (see note). Passing TM_INFINITE causes the caller to
 * block indefinitely until a message is available. Passing
 * TM_NONBLOCK causes the service to return immediately without
 * blocking in case no message is available.
 *
 * @return The number of bytes available from the received message is
 * returned upon success. Zero is a possible value corresponding to a
 * zero-sized message passed to rt_queue_send() or
 * rt_queue_write(). Otherwise:
 *
 * - -ETIMEDOUT is returned if the absolute @a timeout date is reached
 * before a message arrives.
 *
 * - -EWOULDBLOCK is returned if @a timeout is equal to TM_NONBLOCK
 * and no message is immediately available on entry to the call.

 * - -EINTR is returned if rt_task_unblock() was called for the
 * current task before a message was available.
 *
 * - -EINVAL is returned if @a q is not a valid queue descriptor.
 *
 * - -EIDRM is returned if @a q is deleted while the caller was
 * waiting for a message. In such event, @a q is no more valid upon
 * return of this service.
 *
 * - -EPERM is returned if this service should block, but was not
 * called from a Xenomai thread.
 *
 * Valid calling contexts:
 *
 * - Xenomai threads
 * - Any other context if @a timeout equals TM_NONBLOCK.
 *
 * @note The @a timeout value is interpreted as a multiple of the
 * Alchemy clock resolution (see --alchemy-clock-resolution option,
 * defaults to 1 nanosecond).
 */

/**
 * @fn ssize_t rt_queue_receive(RT_QUEUE *q, void **bufp, RTIME timeout)
 * @brief Receive from a queue (with relative timeout date).
 *
 * This routine is a variant of rt_queue_receive_until() accepting a
 * relative timeout specification.
 */

ssize_t rt_queue_receive_timed(RT_QUEUE *queue, void **bufp,
			       const struct timespec *abs_timeout)
{
	struct alchemy_queue_wait *wait;
	struct alchemy_queue_msg *msg;
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	ssize_t ret;
	int err = 0;

	if (!threadobj_current_p() && !alchemy_poll_mode(abs_timeout))
		return -EPERM;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &err);
	if (qcb == NULL) {
		ret = err;
		goto out;
	}

	if (list_empty(&qcb->mq))
		goto wait;

	msg = list_pop_entry(&qcb->mq, struct alchemy_queue_msg, next);
	msg->refcount++;
	*bufp = msg + 1;
	ret = (ssize_t)msg->size;
	qcb->mcount--;
	goto done;
wait:
	if (alchemy_poll_mode(abs_timeout)) {
		ret = -EWOULDBLOCK;
		goto done;
	}

	wait = threadobj_prepare_wait(struct alchemy_queue_wait);
	wait->usersz = 0;

	ret = syncobj_wait_grant(&qcb->sobj, abs_timeout, &syns);
	if (ret) {
		if (ret == -EIDRM) {
			threadobj_finish_wait();
			goto out;
		}
	} else {
		msg = wait->msg;
		*bufp = msg + 1;
		ret = (ssize_t)msg->size;
	}

	threadobj_finish_wait();
done:
	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn ssize_t rt_queue_read_until(RT_QUEUE *q, void *buf, size_t size, RTIME timeout)
 * @brief Read from a queue (with absolute timeout date).
 *
 * This service reads the next available message from a given
 * queue.
 *
 * @param q The descriptor address of the message queue to read
 * from.
 *
 * @param buf A pointer to a memory area which will be written upon
 * success with the received message payload. The internal message
 * buffer conveying the data is automatically freed by this call.
 *
 * @param size The length in bytes of the memory area pointed to by @a
 * buf. Messages larger than @a size are truncated appropriately.
 *
 * @param timeout An absolute date expressed in clock ticks,
 * specifying a time limit to wait for a message to be available from
 * the queue (see note). Passing TM_INFINITE causes the caller to
 * block indefinitely until a message is available. Passing
 * TM_NONBLOCK causes the service to return immediately without
 * blocking in case no message is available.
 *
 * @return The number of bytes copied to @a buf is returned upon
 * success. Zero is a possible value corresponding to a zero-sized
 * message passed to rt_queue_send() or rt_queue_write(). Otherwise:
 *
 * - -ETIMEDOUT is returned if the absolute @a timeout date is reached
 * before a message arrives.
 *
 * - -EWOULDBLOCK is returned if @a timeout is equal to TM_NONBLOCK
 * and no message is immediately available on entry to the call.

 * - -EINTR is returned if rt_task_unblock() was called for the
 * current task before a message was available.
 *
 * - -EINVAL is returned if @a q is not a valid queue descriptor.
 *
 * - -EIDRM is returned if @a q is deleted while the caller was
 * waiting for a message. In such event, @a q is no more valid upon
 * return of this service.
 *
 * - -EPERM is returned if this service should block, but was not
 * called from a Xenomai thread.
 *
 * Valid calling contexts:
 *
 * - Xenomai threads
 * - Any other context if @a timeout equals TM_NONBLOCK.
 *
 * @note The @a timeout value is interpreted as a multiple of the
 * Alchemy clock resolution (see --alchemy-clock-resolution option,
 * defaults to 1 nanosecond).
 */

/**
 * @fn ssize_t rt_queue_read(RT_QUEUE *q, void *buf, size_t size, RTIME timeout)
 * @brief Read from a queue (with relative timeout date).
 *
 * This routine is a variant of rt_queue_read_until() accepting a
 * relative timeout specification.
 */
ssize_t rt_queue_read_timed(RT_QUEUE *queue,
			    void *buf, size_t size,
			    const struct timespec *abs_timeout)
{
	struct alchemy_queue_wait *wait;
	struct alchemy_queue_msg *msg;
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	ssize_t ret;
	int err = 0;

	if (!threadobj_current_p() && !alchemy_poll_mode(abs_timeout))
		return -EPERM;

	if (size == 0)
		return 0;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &err);
	if (qcb == NULL) {
		ret = err;
		goto out;
	}

	if (list_empty(&qcb->mq))
		goto wait;

	msg = list_pop_entry(&qcb->mq, struct alchemy_queue_msg, next);
	qcb->mcount--;
	goto transfer;
wait:
	if (alchemy_poll_mode(abs_timeout)) {
		ret = -EWOULDBLOCK;
		goto done;
	}

	wait = threadobj_prepare_wait(struct alchemy_queue_wait);
	wait->userbuf = buf;
	wait->usersz = size;
	wait->msg = NULL;

	ret = syncobj_wait_grant(&qcb->sobj, abs_timeout, &syns);
	if (ret) {
		if (ret == -EIDRM) {
			threadobj_finish_wait();
			goto out;
		}
	} else if (wait->msg) {
		msg = wait->msg;
	transfer:
		ret = (ssize_t)(msg->size > size ? size : msg->size);
		if (ret > 0) 
			memcpy(buf, msg + 1, ret);
		heapobj_free(&qcb->hobj, msg);
	} else	/* A direct copy took place. */
		ret = (ssize_t)wait->usersz;

	threadobj_finish_wait();
done:
	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn int rt_queue_flush(RT_QUEUE *q)
 * @brief Flush pending messages from a queue.
 *
 * This routine flushes all messages currently pending in a queue,
 * releasing all message buffers appropriately.
 *
 * @param q The descriptor address of the queue to flush.
 *
 * @return Zero is returned upon success. Otherwise:
 *
 * - -EINVAL is returned if @a q is not a valid queue descriptor.
 *
 * Valid calling context: any.
 */
int rt_queue_flush(RT_QUEUE *queue)
{
	struct alchemy_queue_msg *msg, *tmp;
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	int ret = 0;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	ret = qcb->mcount;
	qcb->mcount = 0;

	/*
	 * Flushing a message queue is not an operation we should see
	 * in any fast path within an application, so locking out
	 * other threads from using that queue while we flush it is
	 * acceptable.
	 */
	if (!list_empty(&qcb->mq)) {
		list_for_each_entry_safe(msg, tmp, &qcb->mq, next) {
			list_remove(&msg->next);
			heapobj_free(&qcb->hobj, msg);
		}
	}

	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn int rt_queue_inquire(RT_QUEUE *q, RT_QUEUE_INFO *info)
 * @brief Query queue status.
 *
 * This routine returns the status information about the specified
 * queue.
 *
 * @param q The descriptor address of the queue to get the status
 * of.
 *
 * @return Zero is returned and status information is written to the
 * structure pointed at by @a info upon success. Otherwise:
 *
 * - -EINVAL is returned if @a q is not a valid queue descriptor.
 *
 * Valid calling context: any.
 */
int rt_queue_inquire(RT_QUEUE *queue, RT_QUEUE_INFO *info)
{
	struct alchemy_queue *qcb;
	struct syncstate syns;
	struct service svc;
	int ret = 0;

	CANCEL_DEFER(svc);

	qcb = get_alchemy_queue(queue, &syns, &ret);
	if (qcb == NULL)
		goto out;

	info->nwaiters = syncobj_count_grant(&qcb->sobj);
	info->nmessages = qcb->mcount;
	info->mode = qcb->mode;
	info->qlimit = qcb->limit;
	info->poolsize = heapobj_size(&qcb->hobj);
	info->usedmem = heapobj_inquire(&qcb->hobj);
	strcpy(info->name, qcb->name);

	put_alchemy_queue(qcb, &syns);
out:
	CANCEL_RESTORE(svc);

	return ret;
}

/**
 * @fn int rt_queue_bind(RT_QUEUE *q, const char *name, RTIME timeout)
 * @brief Bind to a message queue.
 *
 * This routine creates a new descriptor to refer to an existing
 * message queue identified by its symbolic name. If the object does
 * not exist on entry, the caller may block until a queue of the
 * given name is created.
 *
 * @param q The address of a queue descriptor filled in by the
 * operation. Contents of this memory is undefined upon failure.
 *
 * @param name A valid NULL-terminated name which identifies the
 * queue to bind to. This string should match the object name
 * argument passed to rt_queue_create().
 *
 * @param timeout The number of clock ticks to wait for the
 * registration to occur (see note). Passing TM_INFINITE causes the
 * caller to block indefinitely until the object is
 * registered. Passing TM_NONBLOCK causes the service to return
 * immediately without waiting if the object is not registered on
 * entry.
 *
 * @return Zero is returned upon success. Otherwise:
 *
 * - -EINTR is returned if rt_task_unblock() was called for the
 * current task before the retrieval has completed.
 *
 * - -EWOULDBLOCK is returned if @a timeout is equal to TM_NONBLOCK
 * and the searched object is not registered on entry.
 *
 * - -ETIMEDOUT is returned if the object cannot be retrieved within
 * the specified amount of time.
 *
 * - -EPERM is returned if this service should block, but was not
 * called from a Xenomai thread.
 *
 * Valid calling contexts:
 *
 * - Xenomai threads
 * - Any other context if @a timeout equals TM_NONBLOCK.
 *
 * @note The @a timeout value is interpreted as a multiple of the
 * Alchemy clock resolution (see --alchemy-clock-resolution option,
 * defaults to 1 nanosecond).
 */
int rt_queue_bind(RT_QUEUE *queue,
		  const char *name, RTIME timeout)
{
	return alchemy_bind_object(name,
				   &alchemy_queue_table,
				   timeout,
				   offsetof(struct alchemy_queue, cobj),
				   &queue->handle);
}

/**
 * @fn int rt_queue_unbind(RT_QUEUE *q)
 * @brief Unbind from a message queue.
 *
 * @param q The descriptor address of the queue to unbind from.
 *
 * This routine releases a previous binding to a message queue. After
 * this call has returned, the descriptor is no more valid for
 * referencing this object.
 */
int rt_queue_unbind(RT_QUEUE *queue)
{
	queue->handle = 0;
	return 0;
}
