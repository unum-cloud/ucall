package main

import (
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"strconv"
	"time"
)

func main() {

	limitTransmits := 1_000_000_000
	limitSeconds := 100
	argsWithoutProg := os.Args[1:]
	if len(argsWithoutProg) == 2 {
		if argsWithoutProg[0] == "time" {
			s, _ := strconv.ParseInt(argsWithoutProg[1], 10, 0)
			limitSeconds = int(s)
		} else if argsWithoutProg[0] == "cycles" {
			s, _ := strconv.ParseInt(argsWithoutProg[1], 10, 0)
			limitTransmits = int(s)
		}
	}

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

	for {
		conn, err := net.DialTCP("tcp", nil, tcpAddr)
		if err != nil {
			println("Dial failed:", err.Error())
			os.Exit(1)
		}

		for {
			a := rand.Intn(1000)
			b := rand.Intn(1000)
			jsonRPC := fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":%d,"session_id":%d},"id":0}`, a, b)
			_, err = conn.Write([]byte(jsonRPC))
			if err != nil {
				break
			}

			_, err = conn.Read(reply)
			if err != nil && !errors.Is(err, io.EOF) {
				break
			}
			if transmits >= limitTransmits || time.Since(start).Seconds() >= float64(limitSeconds) {
				break
			}
			transmits++
		}

		conn.Close()
		if transmits >= limitTransmits || time.Since(start).Seconds() >= float64(limitSeconds) {
			break
		}
		restarts++
	}

	elapsed := time.Since(start)
	latency := float64(elapsed.Microseconds()) / float64(transmits)
	speed := float64(transmits) / float64(elapsed.Seconds())
	fmt.Printf("Took %s to perform %d queries\n", elapsed, transmits)
	fmt.Printf("Mean latency is %.1f microsecond\n", latency)
	fmt.Printf("Resulting in %.1f requests/second\n", speed)
	fmt.Printf("Recreating %d TCP connections\n", restarts)

}
