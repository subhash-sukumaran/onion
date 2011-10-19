/*
	Onion HTTP server library
	Copyright (C) 2011 David Moreno Montero

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 3.0 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not see <http://www.gnu.org/licenses/>.
	*/

#include <malloc.h>
#include <errno.h>
#include <sys/epoll.h>
#include <string.h>
#include <stdio.h>

#include "log.h"
#include "types.h"
#include "poller.h"
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

#ifdef HAVE_PTHREADS
# include <pthread.h>
#else  // if no pthreads, ignore locks.
# define pthread_mutex_lock(...)
# define pthread_mutex_unlock(...)
#endif


struct onion_poller_t{
	int fd;
	int stop;
	int n;
#ifdef HAVE_PTHREADS
	pthread_mutex_t mutex;
	int npollers;
#endif

	struct onion_poller_el_t *head;
};

typedef struct onion_poller_el_t onion_poller_el;

/// Each element of the poll
/// @private
struct onion_poller_el_t{
	int fd;
	int (*f)(void*);
	void *data;

	void (*shutdown)(void*);
	void *shutdown_data;

	int delta_timeout;
	int timeout;
#ifdef HAVE_PTHREADS
	pthread_mutex_t mutex;
#endif
	int active;
	
	struct onion_poller_el_t *next;
};

/**
 * @short Looks for a file descriptor, returns NULL if none.
 * 
 * The mutex of the element is locked when returns.
 */
static onion_poller_el *onion_poller_find_fd_and_lock(onion_poller *p, int fd){
	onion_poller_el *next=NULL;
	pthread_mutex_lock(&p->mutex);
	next=p->head;
	while(next){
		if (next->fd==fd)
			break;
		next=next->next;
	}
	if (next){
		pthread_mutex_lock(&next->mutex);
	}
	pthread_mutex_unlock(&p->mutex);
	return next;
}

/**
 * @short Returns a poller object that helps polling on sockets and files
 * @memberof onion_poller_t
 *
 * This poller is implemented through epoll, but other implementations are possible 
 */
onion_poller *onion_poller_new(int n){
	onion_poller *p=malloc(sizeof(onion_poller));
	p->fd=epoll_create(n);
	if (p->fd < 0){
		ONION_ERROR("Error creating the poller. %s", strerror(errno));
		free(p);
		return NULL;
	}
	p->stop=0;
	p->head=NULL;
	p->n=0;
#ifdef HAVE_PTHREADS
	p->npollers=0;
	pthread_mutex_init(&p->mutex, NULL);
#endif
	return p;
}

/// @memberof onion_poller_t
void onion_poller_free(onion_poller *p){
	ONION_DEBUG("Free onion poller");
	p->stop=1;
	close(p->fd); 
	// Wait until all pollers exit.
#ifdef HAVE_PTHREADS
	int n=10;
	while (p->npollers>0 && n>0){
		ONION_DEBUG("Waiting for %d epollers (%d)", p->npollers, n);
		usleep(100000);
		n--;
	}
#endif
	
	if (pthread_mutex_trylock(&p->mutex)>0){
		ONION_WARNING("When cleaning the poller object, some poller is still active; not freeing memory");
		onion_poller_el *next=p->head;
		while (next){
			onion_poller_el *tnext=next->next;
			pthread_mutex_lock(&next->mutex);
			if (next->shutdown)
				next->shutdown(next->shutdown_data);
			pthread_mutex_unlock(&next->mutex);
			free(next);
			next=tnext;
		}
		pthread_mutex_unlock(&p->mutex);
		free(p);
	}
	ONION_DEBUG0("Done");
}

/**
 * @short Adds a file descriptor to poll.
 * @memberof onion_poller_t
 *
 * When new data is available (read/write/event) the given function
 * is called with that data.
 */
int onion_poller_add(onion_poller *poller, int fd, int (*f)(void*), void *data){
	ONION_DEBUG0("Adding fd %d for polling (%d)", fd, poller->n);

	onion_poller_el *nel=malloc(sizeof(onion_poller_el));
	nel->fd=fd;
	nel->f=f;
	nel->data=data;
	nel->next=NULL;
	nel->shutdown=NULL;
	nel->shutdown_data=NULL;
	nel->timeout=-1;
	nel->delta_timeout=-1;
	nel->active=0;
	pthread_mutex_init(&nel->mutex,NULL);

	pthread_mutex_lock(&poller->mutex);
	if (poller->head){
		onion_poller_el *next=poller->head;
		while (next->next)
			next=next->next;
		next->next=nel;
	}
	else
		poller->head=nel;

	poller->n++;
	pthread_mutex_unlock(&poller->mutex);
	

	return 0;
}

/**
 * @short After adding the poller, this activates it.
 * 
 * Until go is called, the epoll on this thread is disabled, and thus nothing
 * is done there. Once added the callbacks could be called.
 * 
 * This is done in two steps to avoid this:
 * 
 * thread A: onion_poller_add
 * thread B: onion_poller_poll -> execute f -> remove
 * thread A: onion_poller_set_shutdown
 * 
 * @returns boolean with success or not. If not it is that the fd has not been added.
 */
int onion_poller_go(onion_poller *poller, int fd){
	onion_poller_el *el=onion_poller_find_fd_and_lock(poller, fd);
	if (!el)
		return 0;
	el->active=1;
	
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events=EPOLLIN | EPOLLHUP | EPOLLONESHOT;
	ev.data.ptr=el;
	if (epoll_ctl(poller->fd, EPOLL_CTL_ADD, fd, &ev) < 0){
		ONION_ERROR("Error add descriptor to listen to. %s", strerror(errno));
		return 1;
	}
	pthread_mutex_unlock(&el->mutex);
	return 1;
}


/**
 * @short Sets a shutdown for a given file descriptor
 * 
 * This shutdown will be called whenever this descriptor is removed, for example explicit call, or closed on the other side.
 * 
 * The file descriptor must be already on the poller.
 * 
 * @param poller
 * @param fd The file descriptor
 * @param f The function to call
 * @param data The data for that call
 * 
 * @returns 1 if ok, 0 if the file descriptor is not on the poller.
 */
int onion_poller_set_shutdown(onion_poller *poller, int fd, void (*f)(void*), void *data){
	onion_poller_el *el=onion_poller_find_fd_and_lock(poller, fd);
	if (!poller){
		ONION_ERROR("Could not find fd %d", fd);
		return 0;
	}
	ONION_DEBUG0("Added shutdown callback for fd %d", fd);
	el->shutdown=f;
	el->shutdown_data=data;
	pthread_mutex_unlock(&el->mutex);
	return 1;
}

/**
 * @short For a given file descriptor, already in this poller, set a timeout, in ms.
 * 
 * @param poller
 * @param fd The file descriptor
 * @param timeout The timeout, in ms.
 * 
 * @return 1 if set OK, 0 if error.
 */
int onion_poller_set_timeout(onion_poller *poller, int fd, int timeout){
	onion_poller_el *el=onion_poller_find_fd_and_lock(poller, fd);
	if (!poller){
		ONION_ERROR("Could not find fd %d", fd);
		return 0;
	}
	//ONION_DEBUG("Added timeout for fd %d; %d", fd, timeout);
	el->timeout=timeout;
	el->delta_timeout=timeout;
 	pthread_mutex_unlock(&el->mutex);
	return 1;
}

static void onion_poller_free_element(onion_poller *poller, onion_poller_el *el){
	pthread_mutex_lock(&el->mutex);
	if (epoll_ctl(poller->fd, EPOLL_CTL_DEL, el->fd, NULL) < 0){
		ONION_ERROR("Error remove descriptor to listen to. %s", strerror(errno));
	}
	
	if (el->shutdown)
		el->shutdown(el->shutdown_data);
	pthread_mutex_unlock(&el->mutex);
	free(el);
	poller->n--;
}

/**
 * @short Removes a file descriptor, and all related callbacks from the listening queue
 * @memberof onion_poller_t
 */
int onion_poller_remove(onion_poller *poller, int fd){
	pthread_mutex_lock(&poller->mutex);
	ONION_DEBUG0("Trying to remove fd %d (%d)", fd, poller->n);
	onion_poller_el *el=poller->head;
	if (el && el->fd==fd){
		ONION_DEBUG0("Removed from head %p", el);
		
		poller->head=el->next;
		onion_poller_free_element(poller, el);
		
		pthread_mutex_unlock(&poller->mutex);
		return 0;
	}
	while (el->next){
		if (el->next->fd==fd){
			ONION_DEBUG0("Removed from tail %p",el);
				onion_poller_el *t=el->next;
			el->next=t->next;
			onion_poller_free_element(poller, t);
			
			pthread_mutex_unlock(&poller->mutex);
			return 0;
		}
		el=el->next;
	}
	pthread_mutex_unlock(&poller->mutex);
	ONION_WARNING("Trying to remove unknown fd from poller %d", fd);
	return 0;
}

/**
 * @short Gets the next timeout
 * 
 * On edge cases could get a wrong timeout, but always old or new, so its ok.
 */
/*
static int onion_poller_get_next_timeout(onion_poller *p){
	onion_poller_el *next;
	int timeout=60*60000; // Ok, minimum wakeup , once per hour.
	pthread_mutex_lock(&poller->mutex);
	next=p->head;
	while(next){
		//ONION_DEBUG("Check %d %d %d", timeout, next->timeout, next->delta_timeout);
		if (next->timeout>0){
			if (next->delta_timeout>0 && next->delta_timeout<timeout)
				timeout=next->delta_timeout;
		}
		
		next=next->next;
	}
	pthread_mutex_unlock(&poller->mutex);
	
	//ONION_DEBUG("Next wakeup in %d ms, at least", timeout);
	return timeout;
}
*/
// Max of events per loop. If not al consumed for next, so no prob.  right number uses less memory, and makes less calls.
#define MAX_EVENTS 10

/**
 * @short Do the event polling.
 * @memberof onion_poller_t
 *
 * It loops over polling. To exit polling call onion_poller_stop().
 * 
 * If no fd to poll, returns.
 */
void onion_poller_poll(onion_poller *p){
	struct epoll_event event[MAX_EVENTS];
	ONION_DEBUG0("Start poll of fds");
	p->stop=0;
#ifdef HAVE_PTHREADS
	pthread_mutex_lock(&p->mutex);
	p->npollers++;
	ONION_DEBUG0("Npollers %d. %d listenings %p", p->npollers, p->n, p->head);
	pthread_mutex_unlock(&p->mutex);
#endif
	int timeout=-1;
	/*
	int elapsed=0;
	struct timeval ts, te;
	gettimeofday(&ts, NULL);
	*/
	while (!p->stop && p->head){
		//timeout=onion_poller_get_next_timeout(p);
		
		int nfds = epoll_wait(p->fd, event, MAX_EVENTS, timeout);
		/* /// FIXME, no timeout!
		gettimeofday(&te, NULL);
		elapsed=((te.tv_sec-ts.tv_sec)*1000.0) + ((te.tv_usec-ts.tv_usec)/1000.0);
		ONION_DEBUG("Real time waiting was %d ms (compared to %d of timeout). %d wakeups.", elapsed, timeout, nfds);
		ts=te;

		pthread_mutex_lock(&poller->mutex);
		onion_poller_el *next=p->head;
		while (next){
			next->delta_timeout-=elapsed;
			next=next->next;
		}

		{ // Somebody timedout?
			onion_poller_el *next=p->head;
			while (next){
				onion_poller_el *cur=next;
				next=next->next;
				if (cur->timeout >= 0 && cur->delta_timeout <= 0){
					ONION_DEBUG0("Timeout on %d, was %d", cur->fd, cur->timeout);
					onion_poller_remove(p, cur->fd);
				}
			}
		}
		pthread_mutex_unlock(&poller->mutex);
		*/
		if (nfds<0){ // This is normally closed p->fd
				ONION_DEBUG("Some error happened");
				if(p->fd<0 || !p->head){
					ONION_DEBUG("Finishing the epoll as finished: %s", strerror(errno));
#ifdef HAVE_PTHREADS
					pthread_mutex_lock(&p->mutex);
					p->npollers--;
					pthread_mutex_unlock(&p->mutex);
#endif
					return;
				}
		}
		int i;
		for (i=0;i<nfds;i++){
			onion_poller_el *el=(onion_poller_el*)event[i].data.ptr;
			pthread_mutex_lock(&el->mutex);
			el->delta_timeout=el->timeout;
			// Call the callback
			//ONION_DEBUG("Calling callback for fd %d (%X %X)", el->fd, event[i].events);
			int n=el->f(el->data);
			if (n<0){
				pthread_mutex_unlock(&el->mutex);
				onion_poller_remove(p, el->fd);
			}
			else{
				ONION_DEBUG0("Resetting poller %d", el->fd);
				event[i].events=EPOLLIN | EPOLLHUP | EPOLLONESHOT;
				int e=epoll_ctl(p->fd, EPOLL_CTL_MOD, el->fd, &event[i]);
				if (e<0){
					ONION_ERROR("Error resetting poller, %s", strerror(errno));
				}
				pthread_mutex_unlock(&el->mutex);
			}
		}
	}
	ONION_DEBUG0("Finished polling fds");
#ifdef HAVE_PTHREADS
	pthread_mutex_lock(&p->mutex);
	p->npollers--;
	ONION_DEBUG0("Npollers %d", p->npollers);
	pthread_mutex_unlock(&p->mutex);
#endif
}

/**
 * @short Marks the poller to stop ASAP
 * @memberof onion_poller_t
 */
void onion_poller_stop(onion_poller *p){
	p->stop=1;
}

