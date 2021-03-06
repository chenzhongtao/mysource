/*
  Copyright (c) 2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include <sys/poll.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "../debug/debug.h"
#include "event.h"

static int
event_register_poll (struct event_pool *event_pool, int fd,
                     event_handler_t handler,
                     void *data, int poll_in, int poll_out);


static int
__flush_fd (int fd, int idx, void *data,
            int poll_in, int poll_out, int poll_err)
{
        char buf[64];
        int ret = -1;

        if (!poll_in)
                return ret;

        do {
                ret = read (fd, buf, 64);
                if (ret == -1 && errno != EAGAIN) {
                        LOG_PRINT(D_LOG_ERR,
                                "read on %d returned error (%s)",
                                fd, strerror (errno));
                }
        } while (ret == 64);

        return ret;
}

//查找fd在event_pool中的索引
static int
__event_getindex (struct event_pool *event_pool, int fd, int idx)
{
        int  ret = -1;
        int  i = 0;

        GF_VALIDATE_OR_GOTO ("event", event_pool, out);

        if (idx > -1 && idx < event_pool->used) {
                if (event_pool->reg[idx].fd == fd)
                        ret = idx;
        }

        for (i=0; ret == -1 && i<event_pool->used; i++) {
                if (event_pool->reg[i].fd == fd) {
                        ret = i;
                        break;
                }
        }

out:
        return ret;
}


static struct event_pool *
event_pool_new_poll (int count)
{
        struct event_pool *event_pool = NULL;
        int                ret = -1;

        event_pool = calloc (1, sizeof (*event_pool));

        if (!event_pool)
                return NULL;

        event_pool->count = count;
        event_pool->reg = calloc (event_pool->count,
                                     sizeof (*event_pool->reg));

        if (!event_pool->reg) {
                free (event_pool);
                return NULL;
        }

        pthread_mutex_init (&event_pool->mutex, NULL);

        ret = pipe (event_pool->breaker);

        if (ret == -1) {
                LOG_PRINT(D_LOG_ERR,
                        "pipe creation failed (%s)", strerror (errno));
                free (event_pool->reg);
                free (event_pool);
                return NULL;
        }

        ret = fcntl (event_pool->breaker[0], F_SETFL, O_NONBLOCK);
        if (ret == -1) {
                LOG_PRINT(D_LOG_ERR,
                        "could not set pipe to non blocking mode (%s)",
                        strerror (errno));
                close (event_pool->breaker[0]);
                close (event_pool->breaker[1]);
                event_pool->breaker[0] = event_pool->breaker[1] = -1;

                free (event_pool->reg);
                free (event_pool);
                return NULL;
        }

        ret = fcntl (event_pool->breaker[1], F_SETFL, O_NONBLOCK);
        if (ret == -1) {
                LOG_PRINT(D_LOG_ERR,
                        "could not set pipe to non blocking mode (%s)",
                        strerror (errno));

                close (event_pool->breaker[0]);
                close (event_pool->breaker[1]);
                event_pool->breaker[0] = event_pool->breaker[1] = -1;

                free (event_pool->reg);
                free (event_pool);
                return NULL;
        }

        ret = event_register_poll (event_pool, event_pool->breaker[0],
                                   __flush_fd, NULL, 1, 0);
        if (ret == -1) {
                LOG_PRINT(D_LOG_ERR,
                        "could not register pipe fd with poll event loop");
                close (event_pool->breaker[0]);
                close (event_pool->breaker[1]);
                event_pool->breaker[0] = event_pool->breaker[1] = -1;

                free (event_pool->reg);
                free (event_pool);
                return NULL;
        }

        return event_pool;
}


static int
event_register_poll (struct event_pool *event_pool, int fd,
                     event_handler_t handler,
                     void *data, int poll_in, int poll_out)
{
        int idx = -1;

        GF_VALIDATE_OR_GOTO ("event", event_pool, out);

        pthread_mutex_lock (&event_pool->mutex);
        {
                if (event_pool->count == event_pool->used)
                {
                        event_pool->count += 256;

                        event_pool->reg = realloc (event_pool->reg,
                                                      event_pool->count *
                                                      sizeof (*event_pool->reg));
                        if (!event_pool->reg)
                                goto unlock;
                }

                idx = event_pool->used++;

                event_pool->reg[idx].fd = fd;
                event_pool->reg[idx].events = POLLPRI;
                event_pool->reg[idx].handler = handler;
                event_pool->reg[idx].data = data;

                switch (poll_in) {
                case 1:
                        event_pool->reg[idx].events |= POLLIN;
                        break;
                case 0:
                        event_pool->reg[idx].events &= ~POLLIN;
                        break;
                case -1:
                        /* do nothing */
                        break;
                default:
                        LOG_PRINT(D_LOG_ERR,
                                "invalid poll_in value %d", poll_in);
                        break;
                }

                switch (poll_out) {
                case 1:
                        event_pool->reg[idx].events |= POLLOUT;
                        break;
                case 0:
                        event_pool->reg[idx].events &= ~POLLOUT;
                        break;
                case -1:
                        /* do nothing */
                        break;
                default:
                        LOG_PRINT(D_LOG_ERR,
                                "invalid poll_out value %d", poll_out);
                        break;
                }

                event_pool->changed = 1;

        }
unlock:
        pthread_mutex_unlock (&event_pool->mutex);

out:
        return idx;
}


static int
event_unregister_poll (struct event_pool *event_pool, int fd, int idx_hint)
{
        int idx = -1;

        GF_VALIDATE_OR_GOTO ("event", event_pool, out);

        pthread_mutex_lock (&event_pool->mutex);
        {
                idx = __event_getindex (event_pool, fd, idx_hint);

                if (idx == -1) {
                        LOG_PRINT(D_LOG_ERR,
                                "index not found for fd=%d (idx_hint=%d)",
                                fd, idx_hint);
                        errno = ENOENT;
                        goto unlock;
                }

                event_pool->reg[idx] =  event_pool->reg[--event_pool->used];
                event_pool->changed = 1;
        }
unlock:
        pthread_mutex_unlock (&event_pool->mutex);

out:
        return idx;
}


static int
event_select_on_poll (struct event_pool *event_pool, int fd, int idx_hint,
                      int poll_in, int poll_out)
{
        int idx = -1;

        GF_VALIDATE_OR_GOTO ("event", event_pool, out);

        pthread_mutex_lock (&event_pool->mutex);
        {
                idx = __event_getindex (event_pool, fd, idx_hint);

                if (idx == -1) {
                        LOG_PRINT(D_LOG_ERR,
                                "index not found for fd=%d (idx_hint=%d)",
                                fd, idx_hint);
                        errno = ENOENT;
                        goto unlock;
                }

                switch (poll_in) {
                case 1:
                        event_pool->reg[idx].events |= POLLIN;
                        break;
                case 0:
                        event_pool->reg[idx].events &= ~POLLIN;
                        break;
                case -1:
                        /* do nothing */
                        break;
                default:
                        /* TODO: log error */
                        break;
                }

                switch (poll_out) {
                case 1:
                        event_pool->reg[idx].events |= POLLOUT;
                        break;
                case 0:
                        event_pool->reg[idx].events &= ~POLLOUT;
                        break;
                case -1:
                        /* do nothing */
                        break;
                default:
                        /* TODO: log error */
                        break;
                }

                if (poll_in + poll_out > -2)
                        event_pool->changed = 1;
        }
unlock:
        pthread_mutex_unlock (&event_pool->mutex);

out:
        return idx;
}


static int
event_dispatch_poll_handler (struct event_pool *event_pool,
                             struct pollfd *ufds, int i)
{
        event_handler_t  handler = NULL;
        void            *data = NULL;
        int              idx = -1;
        int              ret = 0;

        handler = NULL;
        data    = NULL;

        pthread_mutex_lock (&event_pool->mutex);
        {
                idx = __event_getindex (event_pool, ufds[i].fd, i);

                if (idx == -1) {
                        LOG_PRINT(D_LOG_ERR,
                                "index not found for fd=%d (idx_hint=%d)",
                                ufds[i].fd, i);
                        goto unlock;
                }

                handler = event_pool->reg[idx].handler;
                data = event_pool->reg[idx].data;
        }
unlock:
        pthread_mutex_unlock (&event_pool->mutex);

        if (handler)
                ret = handler (ufds[i].fd, idx, data,
                               (ufds[i].revents & (POLLIN|POLLPRI)),
                               (ufds[i].revents & (POLLOUT)),
                               (ufds[i].revents & (POLLERR|POLLHUP|POLLNVAL)));

        return ret;
}


static int
event_dispatch_poll_resize (struct event_pool *event_pool,
                            struct pollfd *ufds, int size)
{
        int              i = 0;

        pthread_mutex_lock (&event_pool->mutex);
        {
                if (event_pool->changed == 0) {
                        goto unlock;
                }

                if (event_pool->used > event_pool->evcache_size) {
                        free (event_pool->evcache);

                        event_pool->evcache = ufds = NULL;

                        event_pool->evcache_size = event_pool->used;

                        ufds = calloc (sizeof (struct pollfd),
                                          event_pool->evcache_size);
                        if (!ufds)
                                goto unlock;
                        event_pool->evcache = ufds;
                }

                for (i = 0; i < event_pool->used; i++) {
                        ufds[i].fd = event_pool->reg[i].fd;
                        ufds[i].events = event_pool->reg[i].events;
                        ufds[i].revents = 0;
                }

                size = i;
        }
unlock:
        pthread_mutex_unlock (&event_pool->mutex);

        return size;
}


static int
event_dispatch_poll (struct event_pool *event_pool)
{
        struct pollfd   *ufds = NULL;
        int              size = 0;
        int              i = 0;
        int              ret = -1;

        GF_VALIDATE_OR_GOTO ("event", event_pool, out);

        while (1) {
                size = event_dispatch_poll_resize (event_pool, ufds, size);
                ufds = event_pool->evcache;

                ret = poll (ufds, size, 1);

                if (ret == 0)
                        /* timeout */
                        continue;

                if (ret == -1 && errno == EINTR)
                        /* sys call */
                        continue;

                for (i = 0; i < size; i++) {
                        if (!ufds[i].revents)
                                continue;

                        event_dispatch_poll_handler (event_pool, ufds, i);
                }
        }

out:
        return -1;
}


struct event_ops event_ops_poll = {
        .new              = event_pool_new_poll,
        .event_register   = event_register_poll,
        .event_select_on  = event_select_on_poll,
        .event_unregister = event_unregister_poll,
        .event_dispatch   = event_dispatch_poll
};
