package main

import (
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"time"
)

func main() {

	limitTransmits := 10_000
	servAddr := "localhost:8545"
	tcpAddr, err := net.ResolveTCPAddr("tcp", servAddr)
	if err != nil {
		println("ResolveTCPAddr failed:", err.Error())
		os.Exit(1)
	}

	start := time.Now()
	reply := make([]byte, 4096)
	restarts := 0
	transmits := 0

	for ; transmits != limitTransmits; restarts++ {

		conn, err := net.DialTCP("tcp", nil, tcpAddr)
		if err != nil {
			println("Dial failed:", err.Error())
			os.Exit(1)
		}

		for ; transmits != limitTransmits; transmits++ {
			a := rand.Intn(1000)
			b := rand.Intn(1000)
			jsonRPC := fmt.Sprintf(`{"jsonrpc":"2.0","method":"sum","params":{"a":%d,"b":%d},"id":0}`, a, b)
			_, err = conn.Write([]byte(jsonRPC))
			if err != nil {
				break
			}

			_, err = conn.Read(reply)
			if err != nil && !errors.Is(err, io.EOF) {
				break
			}

		}

		conn.Close()
	}

	elapsed := time.Since(start)
	latency := float64(elapsed.Microseconds()) / float64(limitTransmits)
	speed := float64(limitTransmits) / float64(elapsed.Seconds())
	fmt.Printf("Took %s to perform %d queries\n", elapsed, limitTransmits)
	fmt.Printf("Mean latency is %.1f microsecond\n", latency)
	fmt.Printf("Resulting in %.1f requests/second\n", speed)
	fmt.Printf("Recreating %d TCP connections\n", restarts)

}
