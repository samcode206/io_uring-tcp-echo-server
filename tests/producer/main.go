package main

import (
	"encoding/binary"
	"fmt"
	"strings"

	"log"
	"net"
	"time"
)

const producers = 1
var topic = []byte("*\000")

var seq = 1
func main() {

	/**
	 * 32 bits to represent the msg size
	 * 8 bits to represent the msg type
	 * 16 bits to represent the msg metadata
	 * 8 bits padding (not used currently but will most likely be used)
	 */
	


	data := strings.Repeat(fmt.Sprintf("%d", seq), 4096)
	seq++
	// data, err := os.ReadFile("payload.json")
	// if err != nil {
	// 	log.Fatal(err)
	// }

	buff := make([]byte, len(data)+8+len(topic))
	// encode msg size 4
	binary.BigEndian.PutUint32(buff, uint32(len(data)))
	buff[4] = uint8(1)
	binary.BigEndian.PutUint16(buff[5:], uint16(len(topic)))
	buff[7] = uint8(0)
	copy(buff[8:], topic)
	copy(buff[8+len(topic):], []byte(data))

	for i := 0; i < producers; i++ {
		c, err := net.Dial("tcp", ":9919")
		if err != nil {
			log.Fatal(err)
		}
		go func() {
			for {
	
				n, err := c.Write(buff)
				if err != nil {
					log.Fatal(err)
				}
				if (n != len(buff)){
					log.Fatal("couldn't write all data")
				}

				// fmt.Print(string(buff[:n]))

				// data = strings.Repeat(fmt.Sprintf("%d", seq), 4096 * 3)
				seq++
			}

		}()

		go func() {
			for {
				in := make([]byte, 1024 * 1024)
				_, err = c.Read(in)
				if err != nil {
					// log.pr(err)
				}

				// fmt.Println(binary.BigEndian.Uint32(in[:n]))
				// fmt.Println((in[4]))
				// metaLen := binary.BigEndian.Uint16(in[5:7])
				
				// fmt.Println(string(in[8:8+metaLen]))
			}
		}()
	}

	time.Sleep(time.Hour)
}