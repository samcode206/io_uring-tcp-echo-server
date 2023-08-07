package main

import (
	"fmt"
	"log"
	"net"
	"sync/atomic"
	"time"
)

const consumers = 1
const subscribe = 2
const unsubscribe = 3
const ping = 100
var topic = []byte("*\000")

func ByteCountSI(b uint64) string {
	const unit = 1000
	if b < unit {
		return fmt.Sprintf("%d B", b)
	}
	div, exp := unit, 0
	for n := b / unit; n >= unit; n /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB",
		float64(b)/float64(div), "kMGTPE"[exp])
}

func main() {
	var read uint64 = 0

	go func() {
		var prev uint64 = 0
		for range time.Tick(time.Second) {
			totalRead := atomic.LoadUint64(&read)
			fmt.Println("Per Second: ", ByteCountSI(totalRead-prev))
			prev = totalRead
		}

	}()


	for i := 0; i < consumers; i++ {
		go func() {
			c, err := net.Dial("tcp", ":9919")
			if err != nil {
				fmt.Println(err)
				return
			}

			buf := make([]byte, 1024*1024)
			
			for {
				n, err := c.Read(buf)
				if err != nil {
					log.Println(err)
					return
				}
				
				atomic.AddUint64(&read, uint64(n))
			}
		}()
	}

	fmt.Scanln()
}

