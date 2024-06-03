#include "epoll.h"
#include "tcp_ip.h"
#include "types.h"
#include "ustack/lib/list.h"
#include "ustack/lib/rbtree.h"
#include "time.h"

#define MAX_EPOLL_INSTANCES 1024

struct eventpoll *epoll_instances[MAX_EPOLL_INSTANCES] = { NULL };

// extern form tcp_ip.c
extern struct ReadRequest read_req[MAX_CONNECTIONS + 100];

struct epitem {

    struct rb_node rbn;

    /* List header used to link this structure to the eventpoll ready list */
    struct list_node rdllink;
    /*
     * Works together "struct eventpoll"->ovflist in keeping the
     * single linked chain of items.
     */
    struct epitem *next;

    /* The file descriptor information this item refers to */
    WV_I32 fd;

    /* The "container" of this item */
    struct eventpoll *ep;

    /* The structure that describe the interested events and the source fd */
    struct epoll_event event;
};

struct eventpoll {

    /* Wait queue used by sys_epoll_wait() */
    struct list wq;

    /* List of ready file descriptors */
    struct list rdllist;

    /* RB tree root used to store monitored fd structs */
    struct rb_root_cached rbr;
};

int
do_epoll_create(int flag __attribute__((__unused__)))
{
    struct eventpoll *ep = malloc(sizeof(struct eventpoll));
    if (!ep)
        return -1;
    memset(ep, 0, sizeof(struct eventpoll));
    list_init(&ep->wq);
    list_init(&ep->rdllist);
    ep->rbr = RB_ROOT_CACHED;

    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (epoll_instances[i] == NULL) {
            epoll_instances[i] = ep;
            return i;
        }
    }

    free(ep);
    return -1;
}

int
epoll_create1(int flags __attribute__((__unused__)))
{
    return do_epoll_create(0);
}

int
epoll_create(int size __attribute__((__unused__)))
{
    return do_epoll_create(0);
}

/* Tells us if the item is currently linked */
static inline int
ep_is_linked(struct epitem *epi)
{
    return !(epi->rdllink.list->size == 0);
}

// TODO: support more events
static int
check_event_fd(int fd, struct epoll_event *event)
{
    if (read_req[fd].ready && (event->events & EPOLLIN)) {
        return 1;
    }
    return 0;
}

static void
ep_rbtree_insert(struct eventpoll *ep, struct epitem *epi)
{
    struct rb_node **p = &ep->rbr.rb_root.rb_node, *parent = NULL;
    struct epitem   *epic;
    bool             leftmost = true;

    while (*p) {
        parent = *p;
        epic   = rb_entry(parent, struct epitem, rbn);
        if (epi->fd > epic->fd) {
            p        = &parent->rb_right;
            leftmost = false;
        } else
            p = &parent->rb_left;
    }
    rb_link_node(&epi->rbn, parent, p);
    rb_insert_color_cached(&epi->rbn, &ep->rbr, leftmost);
}

static void
ep_insert(struct eventpoll *ep, int fd, struct epoll_event *event)
{
    struct epitem *epi = malloc(sizeof(struct epitem));
    if (!epi)
        return;

    epi->fd    = fd;
    epi->event = *event;
    epi->ep    = ep;

    ep_rbtree_insert(ep, epi);

    int revents = check_event_fd(fd, event);

    /* If the file is already "ready" we drop it inside the ready list */
    if (revents && !ep_is_linked(epi)) {
        list_append(&ep->rdllist, &epi->rdllink);
    }
}

static void
ep_rbtree_remove(struct eventpoll *ep, struct epitem *epi)
{
    struct rb_node        *node = &epi->rbn;
    struct rb_root_cached *root = &ep->rbr;

    rb_erase_cached(node, root);

    if (root->rb_leftmost == node) {
        struct rb_node *next = rb_next(node);
        root->rb_leftmost    = next;
    }
}

static void
ep_remove(struct eventpoll *ep, struct epitem *epi)
{
    ep_rbtree_remove(ep, epi);

    if (ep_is_linked(epi)) {
        list_detach(&epi->rdllink);
    }

    free(epi);
}
static struct epitem *
ep_find(struct eventpoll *ep, int fd)
{
    struct rb_node *rbp;
    struct epitem  *epi, *epir = NULL;

    for (rbp = ep->rbr.rb_root.rb_node; rbp;) {
        epi = rb_entry(rbp, struct epitem, rbn);
        if (fd > epi->fd)
            rbp = rbp->rb_right;
        else if (fd < epi->fd)
            rbp = rbp->rb_left;
        else {
            epir = epi;
            break;
        }
    }

    return epir;
}

void
epoll_raise_event(struct epitem *epi, struct epoll_event *event)
{
    struct eventpoll *ep = epi->ep;
    switch (event->events) {
    case EPOLLIN:
        if (epi->event.events & EPOLLIN) {
            if (!ep_is_linked(epi)) {
                list_append(&ep->rdllist, &epi->rdllink);
            }
        }
    default:
        break;
    }
}

void
raise_event(int fd, struct epoll_event *event)
{
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (epoll_instances[i] != NULL) {
            struct epitem *epi = ep_find(epoll_instances[i], fd);
            if (epi != NULL) {
                epoll_raise_event(epi, event);
            }
        }
    }
}

int
epoll_ctl(int epid, int op, int fd, struct epoll_event *event)
{
    switch (op) {
    case EPOLL_CTL_ADD: {
        struct eventpoll *ep = epoll_instances[epid];
        ep_insert(ep, fd, event);
        return 0; // success
    }
    case EPOLL_CTL_DEL: {
        struct eventpoll *ep  = epoll_instances[epid];
        struct epitem    *epi = ep_find(ep, fd);
        if (!epi) {
            return -2; //
        }
        ep_remove(ep, epi);
        return 0; // success
    }
    case EPOLL_CTL_MOD: {
        struct eventpoll *ep  = epoll_instances[epid];
        struct epitem    *epi = ep_find(ep, fd);
        if (!epi) {
            return -2; // ENOENT
        }
        epi->event = *event;
        return 0; // success
    }
    }
}

int
epoll_wait(int epid, struct epoll_event *events, int maxevents, int timeout)
{
    struct eventpoll *ep = epoll_instances[epid];
    int               n  = 0;

    if (timeout == 0) {
        if (ep->rdllist.size == 0)
            return 0;
    }

    if (timeout > 0) {
        struct timespec deadline;

        clock_gettime(CLOCK_REALTIME, &deadline);
        if (timeout >= 1000) {
            int sec;
            sec = timeout / 1000;
            deadline.tv_sec += sec;
            timeout -= sec * 1000;
        }

        deadline.tv_nsec += timeout * 1000000;

        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
}