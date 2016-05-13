#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_var.h>
#include <netinet6/ip6_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include "socket.h"

static struct socket *fastd_sock;
static void	fastd_rcv_udp_packet(struct mbuf *, int, struct inpcb *,
		    const struct sockaddr *, void *);

int
fastd_create_socket(){
	int error;

	uprintf("create socket\n");
	error = socreate(PF_INET, &fastd_sock, SOCK_DGRAM, IPPROTO_UDP, curthread->td_ucred, curthread);

	if (error) {
		uprintf("cannot create socket: %d\n", error);
	}
	return (error);
}

int
fastd_bind_socket(union fastd_sockaddr *laddr){
	int error;

	if (fastd_sock == NULL){
		error = fastd_create_socket();
		if (error) {
			goto out;
		}
	}

	if (laddr->sa.sa_family == AF_INET) {
		uprintf("binding ipv4 socket, port=%u addr=%08X\n", ntohs(laddr->in4.sin_port), laddr->in4.sin_addr.s_addr);
	} else if (laddr->sa.sa_family == AF_INET6) {
		uprintf("binding ipv6 socket\n");
	} else{
		uprintf("unknown family: %u\n", laddr->sa.sa_family);
	}
	error = sobind(fastd_sock, &laddr->sa, curthread);

	if (error == EADDRINUSE){
		uprintf("address in use\n");
		goto out;
	}
	if (error) {
		goto out;
	}

	error = udp_set_kernel_tunneling(fastd_sock, fastd_rcv_udp_packet, NULL);
	if (error) {
		uprintf("cannot set tunneling function: %d\n", error);
	}else{
		uprintf("tunneling function set\n");
	}

out:
	return (error);
}

void
fastd_destroy_socket(){
	if (fastd_sock != NULL) {
		uprintf("destroy socket\n");
		soclose(fastd_sock);
		fastd_sock = NULL;
	}
}

static void
fastd_rcv_udp_packet(struct mbuf *m, int offset, struct inpcb *inpcb,
    const struct sockaddr *saddr, void *xfso)
{

	uprintf("fastd: received UDP packet");
	goto out;

out:
	if (m != NULL)
		m_freem(m);

}