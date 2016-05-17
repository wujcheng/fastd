package main

/*
#include <netinet/in.h>
#include <net/if.h>
*/
import "C"

import (
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"
	"unsafe"
)

const (
	DevicePath = "/dev/fastd"
)

var (
	listenPort uint16 = 8000
	listenAddr        = net.ParseIP("127.0.0.1")
)

var (
	ioctl_BIND         = _IOW('F', 1, unsafe.Sizeof(syscall.RawSockaddr{}))
	ioctl_CLOSE        = _IO('F', 2)
	ioctl_SET_DRV_SPEC = _IOW('i', 123, unsafe.Sizeof(C.struct_ifdrv{}))
	ioctl_GET_DRV_SPEC = _IOWR('i', 123, unsafe.Sizeof(C.struct_ifdrv{}))

	testMsg = "\x01\x00\x00\x8f\x00\x00\x01\x00\x01\x04\x00\x01\x00\x00\x0d\x00\x03\x00\x76\x31\x38\x05\x00\x0e\x00\x65\x63\x32\x35\x35\x31\x39\x2d\x66\x68\x6d\x71\x76\x63\x06\x00\x20\x00\x83\x36\x9b\xed\xdc\xa7\x77\x58\x51\x67\x52\x0f\xb5\x4a\x7f\xb0\x59\x10\x2b\xf4\xe0\xa4\x6d\xd5\xfb\x1c\x63\x3d\x83\xdb\x77\xa2\x07\x00\x20\x00\xf0\x5c\x6f\x62\x33\x7d\x29\x1e\x34\xf5\x08\x97\xd8\x9b\x02\xae\x43\xa6\xa2\x47\x6e\x29\x69\xd1\xc8\xe8\x10\x4f\xd1\x1c\x18\x73\x08\x00\x20\x00\xd0\x1b\x0e\xdc\x7b\x7d\x06\x4c\xd7\xf8\x20\xba\xc9\x7b\x4b\x9f\x3e\x72\x12\x79\x49\xbe\x04\xc7\xdf\xab\x92\x16\x59\xd4\xec\xe0"
)

func main() {
	if len(os.Args) < 2 {
		println("no arguments given")
		os.Exit(1)
	}

	switch os.Args[1] {
	case "server":
		server()
	case "ifconfig":
		ifconfig(os.Args[2:])
	}
}

func server() {
	// close previous socket
	close()

	// create new socket
	if err := bind(); err != nil {
		panic(err)
	}

	if err := OpenDevice(); err != nil {
		panic(err)
	}

	go readPackets()

	go func() {
		for {
			udpTest(testMsg)
			time.Sleep(time.Second * 1)
		}
	}()

	// Wait for SIGINT or SIGTERM
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	<-sigs
}
