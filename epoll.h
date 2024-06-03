#include "types.h"

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* Epoll event masks */
#define EPOLLIN		0x00000001
#define EPOLLPRI	0x00000002
#define EPOLLOUT	0x00000004
#define EPOLLERR	0x00000008
#define EPOLLHUP	0x00000010
#define EPOLLNVAL	0x00000020
#define EPOLLRDNORM 0x00000040
#define EPOLLRDBAND	0x00000080
#define EPOLLWRNORM	0x00000100
#define EPOLLWRBAND	0x00000200
#define EPOLLMSG	0x00000400
#define EPOLLRDHUP	0x00002000

struct epoll_event {
	WV_I32 events;
	WV_I64 data;
};

void raise_event(int fd, struct epoll_event *event);

int epoll_create(int size __attribute__((__unused__)));

int epoll_create1(int flags __attribute__((__unused__)));

