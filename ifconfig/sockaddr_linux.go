package ifconfig

import (
	"net"
	"syscall"
	"unsafe"
)

/*
  Returns a struct sockaddr_storage* that is in fact a sockaddr_in* or sockaddr_in6*
*/
func Sockaddr(addr net.IP) *syscall.RawSockaddrAny {
	if IsIPv4(addr) {
		raw := syscall.RawSockaddrInet4{
			Family: syscall.AF_INET,
		}
		copy(raw.Addr[:], addr.To4())
		return (*syscall.RawSockaddrAny)(unsafe.Pointer(&raw))
	} else {
		raw := syscall.RawSockaddrInet6{
			Family: syscall.AF_INET6,
		}
		copy(raw.Addr[:], addr.To16())
		return (*syscall.RawSockaddrAny)(unsafe.Pointer(&raw))
	}
}
