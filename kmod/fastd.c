#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/event.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/ioccom.h>
#include <sys/socketvar.h>
#include <sys/param.h>
#include <sys/buf_ring.h>
#include <sys/mutex.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/hash.h>
#include <sys/lock.h>
#include <sys/rmlock.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/netisr.h>
#include <net/bpf.h>

#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include "fastd.h"

#define DEBUG	if_printf

/* Maximum output packet size (default) */
#define	FASTD_MTU		1406
#define	FASTD_PUBKEY_SIZE	32

#define FASTD_SOCKADDR_IS_IPV4(_sin) ((_sin)->sa.sa_family == AF_INET)
#define FASTD_SOCKADDR_IS_IPV6(_sin) ((_sin)->sa.sa_family == AF_INET6)
#define FASTD_SOCKADDR_IS_IPV46(_sin) (FASTD_SOCKADDR_IS_IPV4(_sin) || FASTD_SOCKADDR_IS_IPV6(_sin))

#define FASTD_HASH_SHIFT	6
#define FASTD_HASH_SIZE		(1 << FASTD_HASH_SHIFT)
#define FASTD_HASH_ADDR(_sa)	((_sa)->in4.sin_port % FASTD_HASH_SIZE)
#define FASTD_HASH(_sc)		((_sc)->remote.in4.sin_port % FASTD_HASH_SIZE)

// SIOCGDRVSPEC/SIOCSDRVSPEC commands on fastd interface
#define FASTD_CMD_GET_REMOTE	0
#define FASTD_CMD_SET_REMOTE	1
#define FASTD_CMD_GET_STATS	2

#define satoconstsin(sa)  ((const struct sockaddr_in *)(sa))
#define satoconstsin6(sa) ((const struct sockaddr_in6 *)(sa))

struct iffastdcfg {
	char			pubkey[FASTD_PUBKEY_SIZE];
	struct fastd_inaddr	remote;
};

struct iffastdstats {
	u_long	ipackets;
	u_long	opackets;
};

static void fastd_iface_load(void);
static void fastd_iface_unload(void);


MALLOC_DEFINE(M_FASTD, "fastd_buffer", "buffer for fastd driver");

#define BUFFER_SIZE     256

/* Forward declarations. */
static d_read_t		fastd_read;
static d_write_t	fastd_write;
static d_ioctl_t	fastd_ioctl;
static d_poll_t		fastd_poll;
static d_kqfilter_t	fastd_kqfilter;
static int		fastd_kqevent(struct knote *, long);
static void		fastd_kqdetach(struct knote *);

static struct filterops fastd_filterops = {
	.f_isfd =	0,
	.f_attach =	NULL,
	.f_detach =	fastd_kqdetach,
	.f_event =	fastd_kqevent,
};

static struct cdevsw fastd_cdevsw = {
	.d_version =	D_VERSION,
	.d_read =	fastd_read,
	.d_write =	fastd_write,
	.d_ioctl =	fastd_ioctl,
	.d_poll =	fastd_poll,
	.d_kqfilter =	fastd_kqfilter,
	.d_name =	"fastd"
};


static struct cdev *fastd_dev;
static struct buf_ring *fastd_msgbuf;
static struct mtx       fastd_msgmtx;
static struct selinfo fastd_rsel;


struct fastdudphdr {
	struct udphdr	fastd_udp;
	char type;
} __packed;

// ------------------------------------
// Kernel Sockets

struct fastd_socket {
	LIST_ENTRY(fastd_socket) list;
	LIST_HEAD(,fastd_softc) softc_head; // List of all assigned interfaces
	struct socket        *socket;
	union fastd_sockaddr  laddr;
};

// Head of all kernel sockets
static LIST_HEAD(,fastd_socket) fastd_sockets_head = LIST_HEAD_INITIALIZER(fastd_socket);


// ------------------------------------
// Network Interfaces

struct fastd_softc {
	// lists are protected by global fastd_lock
	LIST_ENTRY(fastd_softc) fastd_ifaces; // list of all interfaces
	LIST_ENTRY(fastd_softc) fastd_flow_entry; // entry in flow table
	LIST_ENTRY(fastd_softc) fastd_socket_entry; // list of softc for a socket

	struct ifnet		*ifp;  /* the interface */
	struct fastd_socket	*socket; /* socket for outgoing packets */
	union fastd_sockaddr	remote;  /* remote ip address and port */
	struct mtx		mtx; /* protect mutable softc fields */
	char			pubkey[FASTD_PUBKEY_SIZE]; /* public key of the peer */
};

// Head of all interfaces
static LIST_HEAD(,fastd_softc) fastd_ifaces_head = LIST_HEAD_INITIALIZER(fastd_softc);

// Mapping from sources addresses to interfaces
LIST_HEAD(fastd_softc_head, fastd_softc);
struct fastd_softc_head fastd_peers[FASTD_HASH_SIZE];



static int fastd_bind_socket(union fastd_sockaddr*);
static int fastd_close_socket(union fastd_sockaddr*);
static void fastd_close_sockets();
static struct fastd_socket* fastd_find_socket(const union fastd_sockaddr*);
static struct fastd_socket* fastd_find_socket_locked(const union fastd_sockaddr*);
static int fastd_send_packet(struct uio *uio);

static void fastd_rcv_udp_packet(struct mbuf *, int, struct inpcb *, const struct sockaddr *, void *);
static void fastd_recv_data(struct mbuf *, u_int, u_int, const union fastd_sockaddr *, struct fastd_socket *);

static struct rmlock fastd_lock;
static const char fastdname[] = "fastd";

static int  fastd_clone_create(struct if_clone *, int, caddr_t);
static void fastd_clone_destroy(struct ifnet *);
static void fastd_destroy(struct fastd_softc *sc);
static int  fastd_ifioctl(struct ifnet *, u_long, caddr_t);
static int  fastd_ioctl_drvspec(struct fastd_softc *, struct ifdrv *, int);
static struct if_clone *fastd_cloner;

static int	fastd_output(struct ifnet *, struct mbuf *, const struct sockaddr *, struct route *ro);
static void	fastd_ifinit(struct ifnet *);
static void fastd_ifstart(struct ifnet *);

static void fastd_add_peer(struct fastd_softc *);
static void fastd_remove_peer(struct fastd_softc *);
static struct fastd_softc* fastd_lookup_peer(const union fastd_sockaddr *);

static void fastd_sockaddr_copy(union fastd_sockaddr *, const union fastd_sockaddr *);
static int  fastd_sockaddr_equal(const union fastd_sockaddr *, const union fastd_sockaddr *);

static int  fastd_ctrl_get_remote(struct fastd_softc *, void *);
static int  fastd_ctrl_set_remote(struct fastd_softc *, void *);
static int  fastd_ctrl_get_stats(struct fastd_softc *, void *);

struct fastd_control {
	int (*fastdc_func)(struct fastd_softc *, void *);
	int fastdc_argsize;
	int fastdc_flags;
#define FASTD_CTRL_FLAG_COPYIN  0x01
#define FASTD_CTRL_FLAG_COPYOUT 0x02
};




// ------------------------------------------------------------------
// Socket helper functions
// ------------------------------------------------------------------


static inline int
isIPv4(const struct fastd_inaddr *inaddr){
	char *buf = (char *) inaddr;
	return (
			 (char)0x00 == (buf[0] | buf[1] | buf[2] | buf[3] | buf[4] | buf[5]| buf[6] | buf[7] | buf[8] | buf[9])
		&& (char)0xff == (buf[10] & buf[11])
	);
}

// Copies a fastd_inaddr into a fixed length fastd_sockaddr
static inline void
sock_to_inet(struct fastd_inaddr *dst, const union fastd_sockaddr *src){
	switch (src->sa.sa_family) {
	case AF_INET:
		bzero(dst->addr, 10);
		memset((char *)&dst->addr + 10, 0xff, 2);
		memcpy((char *)&dst->addr + 12, &src->in4.sin_addr, 4);
		memcpy(        &dst->port,      &src->in4.sin_port, 2);
		break;
	case AF_INET6:
		memcpy(&dst->addr, &src->in6.sin6_addr, 16);
		memcpy(&dst->port, &src->in6.sin6_port, 2);
		break;
	default:
		panic("unsupported address family: %d", src->sa.sa_family);
	}
}

// Copies a fastd_sockaddr into fastd_inaddr
static inline void
inet_to_sock(union fastd_sockaddr *dst, const struct fastd_inaddr *src){
	if (isIPv4(src)){
		// zero struct
		bzero(dst, sizeof(struct sockaddr_in));

		dst->in4.sin_len    = sizeof(struct sockaddr_in);
		dst->in4.sin_family = AF_INET;
		memcpy(&dst->in4.sin_addr, (char *)&src->addr + 12, 4);
		memcpy(&dst->in4.sin_port,         &src->port, 2);
	}else{
		// zero struct
		bzero(dst, sizeof(struct sockaddr_in6));

		dst->in6.sin6_len    = sizeof(struct sockaddr_in6);
		dst->in6.sin6_family = AF_INET6;
		memcpy(&dst->in6.sin6_addr, &src->addr, 16);
		memcpy(&dst->in6.sin6_port, &src->port, 2);
	}
}



// copy fastd_sockaddr to fastd_sockaddr
static inline void
fastd_sockaddr_copy(union fastd_sockaddr *dst, const union fastd_sockaddr *src)
{
	switch (src->sa.sa_family) {
	case AF_INET:
		memcpy(dst, src, sizeof(struct sockaddr_in));
		break;
	case AF_INET6:
		memcpy(dst, src, sizeof(struct sockaddr_in6));
		break;
	}
}



// compares fastd_sockaddr with another fastd_sockaddr
static inline int
fastd_sockaddr_equal(const union fastd_sockaddr *a, const union fastd_sockaddr *b)
{
	if (a->sa.sa_family != b->sa.sa_family)
		return 0;

	switch (a->sa.sa_family) {
	case AF_INET:
		return (
			a->in4.sin_addr.s_addr == b->in4.sin_addr.s_addr &&
			a->in4.sin_port == b->in4.sin_port
		);
	case AF_INET6:
		return (
			IN6_ARE_ADDR_EQUAL (&a->in6.sin6_addr, &b->in6.sin6_addr) &&
			(a->in6.sin6_port == b->in6.sin6_port) &&
			(a->in6.sin6_scope_id == 0 || b->in6.sin6_scope_id == 0 || (a->in6.sin6_scope_id == b->in6.sin6_scope_id))
		);
	default:
		return 1;
	}
}



// ------------------------------------------------------------------
// Functions for control device
// ------------------------------------------------------------------


static int
fastd_poll(struct cdev *dev, int events, struct thread *td)
{
	int revents;

	mtx_lock(&fastd_msgmtx);
	if (buf_ring_empty(fastd_msgbuf)) {
		revents = 0;
		if (events & (POLLIN | POLLRDNORM))
			selrecord(td, &fastd_rsel);
	} else {
		revents = events & (POLLIN | POLLRDNORM);
	}
	mtx_unlock(&fastd_msgmtx);
	return (revents);
}

static int
fastd_kqfilter(struct cdev *dev, struct knote *kn)
{
	switch (kn->kn_filter) {
	case EVFILT_READ:
		kn->kn_fop = &fastd_filterops;
		knlist_add(&fastd_rsel.si_note, kn, 0);
		return (0);
	default:
		return (EINVAL);
	}
}

static int
fastd_kqevent(struct knote *kn, long hint)
{
	kn->kn_data = buf_ring_count(fastd_msgbuf);
	return (kn->kn_data > 0);
}

static void
fastd_kqdetach(struct knote *kn)
{
	knlist_remove(&fastd_rsel.si_note, kn, 0);
}

static int
fastd_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	int error = 0;
	if (uio->uio_iov->iov_len < sizeof(struct fastd_message) - sizeof(uint16_t)){
		uprintf("message too short.\n");
		error = EINVAL;
		return (error);
	}

	error = fastd_send_packet(uio);

	return (error);
}

static int
fastd_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	int error = 0;
	struct fastd_message *msg;
	size_t tomove;

	// dequeue next message
	msg = buf_ring_dequeue_mc(fastd_msgbuf);

	if (msg != NULL) {
		// move message to device
		tomove = MIN(uio->uio_resid, sizeof(struct fastd_message) - sizeof(uint16_t) + msg->datalen);
		error  = uiomove((char *)msg + sizeof(uint16_t), tomove, uio);
		free(msg, M_FASTD);
	}

	if (error != 0)
		uprintf("Read failed.\n");

	return (error);
}

static int
fastd_modevent(module_t mod __unused, int event, void *arg __unused)
{
	int error = 0;

	switch (event) {
	case MOD_LOAD:
		rm_init(&fastd_lock, "fastd_lock");
		mtx_init(&fastd_msgmtx, "fastd", NULL, MTX_SPIN);
		knlist_init_mtx(&fastd_rsel.si_note, NULL);
		fastd_msgbuf = buf_ring_alloc(FASTD_MSG_BUFFER_SIZE, M_FASTD, M_WAITOK, &fastd_msgmtx);
		fastd_dev = make_dev(&fastd_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600, "fastd");
		fastd_iface_load();

		uprintf("fastd driver loaded.\n");
		break;
	case MOD_UNLOAD:
		fastd_iface_unload();
		fastd_close_sockets();
		knlist_destroy(&fastd_rsel.si_note);
		seldrain(&fastd_rsel);
		destroy_dev(fastd_dev);

		// Free ringbuffer and stored items
		struct fastd_message *msg;
		while(1){
			msg = buf_ring_dequeue_mc(fastd_msgbuf);
			if (msg == NULL)
				break;
			free(msg, M_FASTD);
		}
		buf_ring_free(fastd_msgbuf, M_FASTD);
		mtx_destroy(&fastd_msgmtx);
		rm_destroy(&fastd_lock);

		uprintf("fastd driver unloaded.\n");
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static int
fastd_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
	int error;
	union fastd_sockaddr sa;

	switch (cmd) {
	case FASTD_IOCTL_BIND:
		inet_to_sock(&sa, (struct fastd_inaddr*)data);
		error = fastd_bind_socket(&sa);
		break;
	case FASTD_IOCTL_CLOSE:
		inet_to_sock(&sa, (struct fastd_inaddr*)data);
		error = fastd_close_socket(&sa);
		break;
	default:
		error = ENOTTY;
		break;
	}

	return (error);
}


DEV_MODULE(fastd, fastd_modevent, NULL);


// ------------------------------------------------------------------
// Network functions
// ------------------------------------------------------------------



static int
fastd_bind_socket(union fastd_sockaddr *sa){
	int error;
	struct fastd_socket *sock;

	sock = malloc(sizeof(*sock), M_FASTD, M_WAITOK | M_ZERO);
	fastd_sockaddr_copy(&sock->laddr, sa);

	error = socreate(sa->sa.sa_family, &sock->socket, SOCK_DGRAM, IPPROTO_UDP, curthread->td_ucred, curthread);

	if (error) {
		goto out;
	}

	error = sobind(sock->socket, &sa->sa, curthread);

	if (error){
		uprintf("can not bind to socket: %d\n", error);
		goto fail;
	}

	error = udp_set_kernel_tunneling(sock->socket, fastd_rcv_udp_packet, sock);

	if (error) {
		uprintf("can not set tunneling function: %d\n", error);
		goto fail;
	}

	// Initialize list of assigned interfaces
	LIST_INIT(&sock->softc_head);

	rm_wlock(&fastd_lock);
	LIST_INSERT_HEAD(&fastd_sockets_head, sock, list);
	rm_wunlock(&fastd_lock);

	goto out;

fail:
	soclose(sock->socket);
out:
	if (error){
		free(sock, M_FASTD);
	}
	return (error);
}


// Closes a socket
static int
fastd_close_socket(union fastd_sockaddr *sa){
	int error = ENXIO;
	struct fastd_socket *sock;

	rm_wlock(&fastd_lock);

	LIST_FOREACH(sock, &fastd_sockets_head, list) {
		if (fastd_sockaddr_equal(sa, &sock->laddr)) {
			soclose(sock->socket);
			free(sock, M_FASTD);
			LIST_REMOVE(sock, list);
			error = 0;
			break;
		}
	}

	rm_wunlock(&fastd_lock);
	return (error);
}


// Closes all sockets
static void
fastd_close_sockets(union fastd_sockaddr *laddr){
	struct fastd_socket *sock;

	rm_wlock(&fastd_lock);

	LIST_FOREACH(sock, &fastd_sockets_head, list) {
		soclose(sock->socket);
		free(sock, M_FASTD);
	}
	LIST_INIT(&fastd_sockets_head);

	rm_wunlock(&fastd_lock);
}


// Finds a socket by sockaddr
static struct fastd_socket *
fastd_find_socket(const union fastd_sockaddr *sa){
	struct rm_priotracker tracker;
	struct fastd_socket *sock;

	rm_rlock(&fastd_lock, &tracker);
	sock = fastd_find_socket_locked(sa);
	rm_runlock(&fastd_lock, &tracker);
	return sock;
}

// Finds a socket by sockaddr
static struct fastd_socket *
fastd_find_socket_locked(const union fastd_sockaddr *sa){
	struct fastd_socket *sock;

	LIST_FOREACH(sock, &fastd_sockets_head, list) {
		//if (fastd_sockaddr_equal(sa, &sock->laddr))

		// Find by sa_family
		if (sa->sa.sa_family == sock->laddr.sa.sa_family)
			return sock;
	}
	return NULL;
}

static void
fastd_rcv_udp_packet(struct mbuf *m, int offset, struct inpcb *inpcb,
		const struct sockaddr *sa_src, void *xfso)
{
	struct rm_priotracker tracker;
	struct fastd_message *fastd_msg;
	struct fastd_socket *fso;
	char msg_type;
	int error;
	u_int datalen;

	// Ensure packet header exists
	M_ASSERTPKTHDR(m);

	fso = xfso;
	offset += sizeof(struct udphdr);
	datalen = m->m_len - offset;

	// drop UDP packets with less than 1 byte payload
	if (datalen < 1)
		goto free;

	m_copydata(m, offset, 1, (caddr_t) &msg_type);
	rm_rlock(&fastd_lock, &tracker);

	switch (msg_type){
	case FASTD_HDR_HANDSHAKE:
		// Header too short?
		if (datalen < 4)
			goto free;

		// Allocate memory
		fastd_msg = malloc(sizeof(*fastd_msg) + datalen, M_FASTD, M_NOWAIT);
		if (fastd_msg == NULL)
			goto free;
		fastd_msg->datalen = datalen;

		// Copy addresses
		sock_to_inet(&fastd_msg->src, (union fastd_sockaddr *)sa_src);
		sock_to_inet(&fastd_msg->dst, &fso->laddr);

		// Copy fastd packet
		m_copydata(m, offset, datalen, (caddr_t) &fastd_msg->data);

		// Store into ringbuffer of character device
		error = buf_ring_enqueue(fastd_msgbuf, fastd_msg);
		if (error == ENOBUFS){
			printf("fastd: no buffer for handshake packets available\n");
			free(fastd_msg, M_FASTD);
		} else {
			selwakeup(&fastd_rsel);
			KNOTE_UNLOCKED(&fastd_rsel.si_note, 0);
		}

		break;
	case FASTD_HDR_DATA:
		fastd_recv_data(m, offset, datalen, (const union fastd_sockaddr *)sa_src, fso);
		goto unlock;
	default:
		printf("fastd: invalid packet type=%02X datalen=%d\n", msg_type, datalen);
	}
free:
	m_freem(m);
unlock:
	rm_runlock(&fastd_lock, &tracker);
}

static void
fastd_recv_data(struct mbuf *m, u_int offset, u_int datalen, const union fastd_sockaddr *sa_src, struct fastd_socket *socket)
{
	struct fastd_softc *sc;
	int isr, af;

	sc = fastd_lookup_peer(sa_src);
	if (sc == NULL) {
		m_freem(m);
		printf("fastd: unable to find peer\n");
		return;
	}

	if (datalen == 1){
		// Keepalive packet
		if_inc_counter(sc->ifp, IFCOUNTER_IPACKETS, 1);

		m->m_len = m->m_pkthdr.len = 1;
		m->m_data[0] = FASTD_HDR_DATA;
		int error = sosend(socket->socket, (struct sockaddr *)sa_src, NULL, m, NULL, 0, curthread);

		if (error){
			m_freem(m);
			if_inc_counter(sc->ifp, IFCOUNTER_OERRORS, 1);
			DEBUG(sc->ifp, "fastd keepalive response failed: %d\n", error);
		} else {
			if_inc_counter(sc->ifp, IFCOUNTER_OPACKETS, 1);
		}
		return;
	}

	// Get the IP version number
	u_int8_t tp;
	m_copydata(m, offset+1, 1, &tp);
	tp = (tp >> 4) & 0xff;

	switch (tp) {
	case IPVERSION:
		isr = NETISR_IP;
		af = AF_INET;
		break;
	case (IPV6_VERSION >> 4):
		isr = NETISR_IPV6;
		af = AF_INET6;
		break;
	default:
		if_inc_counter(sc->ifp, IFCOUNTER_IERRORS, 1);
		DEBUG(sc->ifp, "unknown ip version: %02x\n", tp );
		m_freem(m);
		return;
	}

	// Trim ip+udp+fastd headers
	m_adj(m, offset+1);

	// Assign receiving interface
	m->m_pkthdr.rcvif = sc->ifp;

	// Pass to Berkeley Packet Filter
	BPF_MTAP2(sc->ifp, &af, sizeof(af), m);

	// Update counters
	if_inc_counter(sc->ifp, IFCOUNTER_IPACKETS, 1);
	if_inc_counter(sc->ifp, IFCOUNTER_IBYTES, m->m_pkthdr.len);

	netisr_dispatch(isr, m);
}

// Send outgoing control packet via UDP
static int
fastd_send_packet(struct uio *uio) {
	int error;
	size_t datalen, addrlen;
	struct fastd_message msg;
	struct mbuf *m = NULL;
	struct fastd_socket *sock;
	union fastd_sockaddr src_addr, dst_addr;


	addrlen = 2 * sizeof(struct fastd_inaddr);
	datalen = uio->uio_iov->iov_len - addrlen;

	// Copy addresses from user memory
	error = uiomove((char *)&msg + sizeof(uint16_t), addrlen, uio);
	if (error != 0){
		goto out;
	}

	// Build destination address
	inet_to_sock(&src_addr, &msg.src);
	inet_to_sock(&dst_addr, &msg.dst);

	// Find socket by address
	sock = fastd_find_socket(&src_addr);
	if (sock == NULL) {
	error = EIO;
		goto out;
	}

	// Allocate space for packet
	m = m_getm(NULL, datalen, M_WAITOK, MT_DATA);

	// Set mbuf current data length
	m->m_len = m->m_pkthdr.len = datalen;

	// Copy payload from user memory
	error = uiomove(m->m_data, datalen, uio);
	if (error != 0){
		goto fail;
	}

	// Send packet
	error = sosend(sock->socket, &dst_addr.sa, NULL, m, NULL, 0, uio->uio_td);
	if (error != 0){
		goto fail;
	}

	goto out;
fail:
	m_free(m);
out:
	return (error);
}


static int
fastd_output(struct ifnet *ifp, struct mbuf *m, const struct sockaddr *dst, struct route *ro)
{
	struct rm_priotracker tracker;
	struct fastd_softc *sc;
	u_int32_t af;
	int len, error;

	sc = ifp->if_softc;
	len = m->m_pkthdr.len;

	rm_rlock(&fastd_lock, &tracker);
	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0) {
		error = ENETDOWN;
		goto out;
	}

	/* BPF writes need to be handled specially. */
	if (dst->sa_family == AF_UNSPEC)
		bcopy(dst->sa_data, &af, sizeof(af));
	else
		af = dst->sa_family;

	// Pass to Berkeley Packet Filter
	BPF_MTAP2(ifp, &af, sizeof(af), m);

	// Add fastd header
	M_PREPEND(m, 1, M_NOWAIT);
	if (m == NULL) {
		error = ENOBUFS;
		goto count;
	}
	m->m_data[0] = FASTD_HDR_DATA;

	error = sosend(sc->socket->socket, (struct sockaddr *)&sc->remote, NULL, m, NULL, 0, curthread);
	if (!error) {
		// sosend was successful and already freed the mbuf
		m = NULL;
	}

count:
	// Update interface counters
	if (error == 0) {
		if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
		if_inc_counter(ifp, IFCOUNTER_OBYTES, len);
	} else
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);

out:
	rm_runlock(&fastd_lock, &tracker);
	if (m != NULL)
		m_freem(m);

	return (error);
}


static void
fastd_ifstart(struct ifnet *ifp __unused)
{
}


static void
fastd_iface_load()
{
	int i;

	for (i = 0; i < FASTD_HASH_SIZE; i++) {
		LIST_INIT(&fastd_peers[i]);
	}

	rm_init(&fastd_lock, "fastd_lock");
	fastd_cloner = if_clone_simple(fastdname, fastd_clone_create, fastd_clone_destroy, 0);
}

static void
fastd_iface_unload()
{
	int i;
	struct fastd_softc *sc;

	if_clone_detach(fastd_cloner);

	rm_wlock(&fastd_lock);
	while ((sc = LIST_FIRST(&fastd_ifaces_head)) != NULL) {
		fastd_destroy(sc);
	}
	rm_wunlock(&fastd_lock);

	for (i = 0; i < FASTD_HASH_SIZE; i++) {
		KASSERT(LIST_EMPTY(&fastd_peers[i]), "fastd: list not empty");
	}
}

static int
fastd_clone_create(struct if_clone *ifc, int unit, caddr_t params)
{
	struct fastd_softc *sc;
	struct ifnet *ifp;
	int error;

	sc = malloc(sizeof(*sc), M_FASTD, M_WAITOK | M_ZERO);

	if (params != NULL) {
		// TODO
	}

	ifp = if_alloc(IFT_PPP);
	if (ifp == NULL) {
		error = ENOSPC;
		goto fail;
	}

	mtx_init(&sc->mtx, "fastd_mtx", NULL, MTX_DEF);

	sc->ifp = ifp;

	if_initname(ifp, fastdname, unit);
	ifp->if_softc = sc;
	ifp->if_ioctl = fastd_ifioctl;
	ifp->if_output = fastd_output;
	ifp->if_start = fastd_ifstart;
	ifp->if_mtu = FASTD_MTU;
	ifp->if_flags = IFF_POINTOPOINT | IFF_MULTICAST;
	ifp->if_capabilities |= IFCAP_LINKSTATE;
	ifp->if_capenable |= IFCAP_LINKSTATE;

	if_attach(ifp);
	bpfattach(ifp, DLT_NULL, sizeof(u_int32_t));

	rm_wlock(&fastd_lock);
	LIST_INSERT_HEAD(&fastd_ifaces_head, sc, fastd_ifaces);
	rm_wunlock(&fastd_lock);

	return (0);

fail:
	free(sc, M_FASTD);
	return (error);
}


static void
fastd_clone_destroy(struct ifnet *ifp)
{
	struct fastd_softc *sc;
	sc = ifp->if_softc;

	rm_wlock(&fastd_lock);
	mtx_destroy(&sc->mtx);
	fastd_remove_peer(sc);
	fastd_destroy(sc);
	rm_wunlock(&fastd_lock);
}

// fastd_lock must be locked before
static void
fastd_destroy(struct fastd_softc *sc)
{
	LIST_REMOVE(sc, fastd_ifaces);

	bpfdetach(sc->ifp);
	if_detach(sc->ifp);
	if_free(sc->ifp);
	free(sc, M_FASTD);
}

static void
fastd_remove_peer(struct fastd_softc *sc)
{
	struct fastd_softc *entry;
	// Remove from flows
	LIST_FOREACH(entry, &fastd_peers[FASTD_HASH(sc)], fastd_flow_entry) {
		if (fastd_sockaddr_equal(&entry->remote, &sc->remote)) {
			LIST_REMOVE(entry, fastd_flow_entry);
			break;
		}
	}

	// Remove from socket
	if (sc->socket != NULL) {
		LIST_REMOVE(sc, fastd_socket_entry);
		sc->socket = NULL;
	}
}

static struct fastd_softc*
fastd_lookup_peer(const union fastd_sockaddr *addr)
{
	struct fastd_softc *entry;
	LIST_FOREACH(entry, &fastd_peers[FASTD_HASH_ADDR(addr)], fastd_flow_entry) {
		if (fastd_sockaddr_equal(&entry->remote, addr))
			return entry;
	}

	return NULL;
}

static void
fastd_add_peer(struct fastd_softc *sc)
{
	if (sc->remote.in4.sin_port > 0){
		// Add to flows
		LIST_INSERT_HEAD(&fastd_peers[FASTD_HASH(sc)], sc, fastd_flow_entry);
	}

	if(sc->socket != NULL){
		// Assign to new socket
		LIST_INSERT_HEAD(&sc->socket->softc_head, sc, fastd_socket_entry);
	}
}



static void
fastd_ifinit(struct ifnet *ifp)
{
	struct fastd_softc *sc = ifp->if_softc;

	DEBUG(ifp, "ifinit\n");

	mtx_lock(&sc->mtx);
	ifp->if_flags |= IFF_UP;
	mtx_unlock(&sc->mtx);
}




// ------------------------------------------------------------------
// Functions for control device
// ------------------------------------------------------------------




// Functions that are called on SIOCGDRVSPEC and SIOCSDRVSPEC
static const struct fastd_control fastd_control_table[] = {

	[FASTD_CMD_GET_REMOTE] =
			{   fastd_ctrl_get_remote, sizeof(struct iffastdcfg),
		FASTD_CTRL_FLAG_COPYOUT
			},

	[FASTD_CMD_SET_REMOTE] =
			{   fastd_ctrl_set_remote, sizeof(struct iffastdcfg),
		FASTD_CTRL_FLAG_COPYIN
			},

	[FASTD_CMD_GET_STATS] =
			{   fastd_ctrl_get_stats, sizeof(struct iffastdstats),
		FASTD_CTRL_FLAG_COPYOUT
			},
};

static const int fastd_control_table_size = nitems(fastd_control_table);



static int
fastd_ctrl_get_remote(struct fastd_softc *sc, void *arg)
{
	struct iffastdcfg *cfg;

	cfg = arg;
	bzero(cfg, sizeof(*cfg));

	memcpy(&cfg->pubkey, &sc->pubkey, sizeof(cfg->pubkey));

	if (FASTD_SOCKADDR_IS_IPV46(&sc->remote))
		sock_to_inet(&cfg->remote, &sc->remote);

	return (0);
}


static int
fastd_ctrl_set_remote(struct fastd_softc *sc, void *arg)
{
	struct iffastdcfg *cfg = arg;
	struct fastd_softc *other;
	struct fastd_socket *socket;
	union fastd_sockaddr sa;
	int error = 0;
	inet_to_sock(&sa, &cfg->remote);

	rm_wlock(&fastd_lock);

	// address and port already taken?
	other = fastd_lookup_peer(&sa);
	if (other != NULL) {
		if (other != sc) {
			error = EADDRNOTAVAIL;
			goto out;
		} else if (fastd_sockaddr_equal(&other->remote, &sa)) {
			// peer has already the address
			goto out;
		}
	}

	// Find socket
	socket = fastd_find_socket_locked(&sa);
	if (socket == NULL) {
		error = EADDRNOTAVAIL;
		goto out;
	}

	// Reconfigure
	fastd_remove_peer(sc);
	fastd_sockaddr_copy(&sc->remote, &sa);
	memcpy(&sc->pubkey, &cfg->pubkey, sizeof(sc->pubkey));
	sc->socket = socket;
	fastd_add_peer(sc);

	// Ready to deliver packets
	sc->ifp->if_drv_flags |= IFF_DRV_RUNNING;
	if_link_state_change(sc->ifp, LINK_STATE_UP);
out:
	rm_wunlock(&fastd_lock);
	return (error);
}

static int
fastd_ctrl_get_stats(struct fastd_softc *sc, void *arg)
{
	struct iffastdstats *stats = arg;

	stats->ipackets = sc->ifp->if_ipackets;
	stats->opackets = sc->ifp->if_opackets;

	return (0);
}


static int
fastd_ioctl_drvspec(struct fastd_softc *sc, struct ifdrv *ifd, int get)
{
	const struct fastd_control *vc;
	struct iffastdcfg args;
	int out, error;


	if (ifd->ifd_cmd >= fastd_control_table_size){
		printf("fastd_ioctl_drvspec() invalid command\n");
		return (EINVAL);
	}

	bzero(&args, sizeof(args));
	vc = &fastd_control_table[ifd->ifd_cmd];
	out = (vc->fastdc_flags & FASTD_CTRL_FLAG_COPYOUT) != 0;

	if ((get != 0 && out == 0) || (get == 0 && out != 0)){
		printf("fastd_ioctl_drvspec() invalid flags\n");
		return (EINVAL);
	}

	if (ifd->ifd_len != vc->fastdc_argsize || ifd->ifd_len > sizeof(args)){
		printf("fastd_ioctl_drvspec() invalid argsize given=%lu expected=%d, args=%lu\n", ifd->ifd_len, vc->fastdc_argsize, sizeof(args));
		return (EINVAL);
	}

	if (vc->fastdc_flags & FASTD_CTRL_FLAG_COPYIN) {
		error = copyin(ifd->ifd_data, &args, ifd->ifd_len);
		if (error){
			printf("fastd_ioctl_drvspec() copyin failed\n");
			return (error);
		}
	}

	error = vc->fastdc_func(sc, &args);
	if (error)
		return (error);

	if (vc->fastdc_flags & FASTD_CTRL_FLAG_COPYOUT) {
		error = copyout(&args, ifd->ifd_data, ifd->ifd_len);
		if (error)
			return (error);
	}

	return (0);
}


static int
fastd_ifioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct fastd_softc *sc;
	struct ifdrv *ifd = (struct ifdrv *)data;
	struct ifreq *ifr = (struct ifreq *)data;
	struct ifstat *ifs;
	int error = 0;

	sc = ifp->if_softc;

	switch(cmd) {
	case SIOCGIFSTATUS:
		ifs = (struct ifstat *)data;
		char ip6buf[INET6_ADDRSTRLEN];
		switch (sc->remote.sa.sa_family) {
		case AF_INET:
			sprintf(ifs->ascii + strlen(ifs->ascii),
			"\tremote port=%d inet4=%s\n", ntohs(sc->remote.in4.sin_port), inet_ntoa(sc->remote.in4.sin_addr));
			break;
		case AF_INET6:
			sprintf(ifs->ascii + strlen(ifs->ascii),
			"\tremote port=%d inet6=%s\n", ntohs(sc->remote.in6.sin6_port), ip6_sprintf(ip6buf, &sc->remote.in6.sin6_addr));
			break;
		}
		break;
	case SIOCSIFADDR:
		fastd_ifinit(ifp);
		DEBUG(ifp, "address set\n");
		/*
		 * Everything else is done at a higher level.
		 */
		break;
	case SIOCSIFMTU:
		// Set MTU
		ifp->if_mtu = ifr->ifr_mtu;
		break;
	case SIOCGDRVSPEC:
	case SIOCSDRVSPEC:
		printf("SIOCGDRVSPEC/SIOCSDRVSPEC ifname=%s cmd=%lx len=%lu\n", ifd->ifd_name, ifd->ifd_cmd, ifd->ifd_len);
		error = fastd_ioctl_drvspec(sc, ifd, cmd == SIOCGDRVSPEC);
		break;
	case SIOCSIFFLAGS:
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = EINVAL;
	}
	return (error);
}
