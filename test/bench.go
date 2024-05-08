package main

import (
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"time"
	"bytes"
  "flag"
  "strconv"
)

var(
    limitSeconds int
    numConnections int
    hostname string
    port int
    batch int
    html bool
    req string
    buffer bytes.Buffer
)

func load_buffer() {
    for i := 0; i < batch; i++ {
        a := rand.Intn(1000)
        b := rand.Intn(1000)
        if ( batch > 1 ) { buffer.WriteString(fmt.Sprintf(`[`)) }
        if i < batch-1 {
          buffer.WriteString(fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":%d,"session_id":%d},"id":%d},`, a, b, i))
        } else {
          buffer.WriteString(fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":%d,"session_id":%d},"id":%d}`, a, b, i))
        }
    }
    if ( batch > 1 ) { buffer.WriteString(fmt.Sprintf(`]`)) }
    //fmt.Printf("%s\n",buffer.String())
}

func client(c chan int, tcpAddr *net.TCPAddr, tid int ) {
	  reply := make([]byte, 4096)
    transmits := 0
    conn, err := net.DialTCP("tcp", nil, tcpAddr)
    if err != nil {
	      println("Dial failed:", err.Error())
	      os.Exit(1)
    }

    start := time.Now()
    for {
        _, err = conn.Write(buffer.Bytes())
        if err != nil {
            fmt.Printf("Write Error: %v\n", err)
            break
        }

        _, err := conn.Read(reply)
        //fmt.Printf("Reply\n%s",reply[:l])
        if err != nil && !errors.Is(err, io.EOF) {
            break
        }
        if time.Since(start).Seconds() >= float64(limitSeconds) {
            break
        }
        transmits++
    }
    conn.Close()
    c <- transmits
}

func formatInt(number int64) string {
    output := strconv.FormatInt(number, 10)
    startOffset := 3
    if number < 0 {
        startOffset++
    }
    for outputIndex := len(output); outputIndex > startOffset; {
        outputIndex -= 3
        output = output[:outputIndex] + "," + output[outputIndex:]
    }
    return output
}


func main() {

  flag.StringVar(&hostname,    "h", "localhost", "hostname")
  flag.IntVar(&port,           "p", 8545,        "port")
  flag.IntVar(&numConnections, "c", 16,          "Number of connections")
  flag.IntVar(&limitSeconds,   "s", 2,           "Stop after n seconds")
  flag.IntVar(&batch,          "b", 1,           "Batch n requests together")
  flag.BoolVar(&html,          "html", false,    "Send an html request instead of jsonrpc")
  flag.Parse()

  fmt.Printf("DELME before connecting")
  servAddr := fmt.Sprintf(`%s:%d`,hostname,port)
	tcpAddr, err := net.ResolveTCPAddr("tcp", servAddr)
	if err != nil {
		  println("ResolveTCPAddr failed:", err.Error())
		  os.Exit(1)
	}

  load_buffer();

  fmt.Printf("Benchmarking jsonrpc for %d seconds with %d connections and a batch size of %d \n", limitSeconds, numConnections, batch);

	start := time.Now()
  c := make(chan int)
	for i := 0; i < numConnections; i++ {
      go client( c, tcpAddr, i )
	}

  // Wait for all connections to finish
  transmits := 0
	for i := 0; i < numConnections; i++ {
      transmits += <-c
	}

  elapsed := time.Since(start)
  latency := float64(elapsed.Microseconds()) / float64(transmits)
  speed := int64((float64(transmits) / float64(elapsed.Seconds())) * float64(batch))
  fmt.Printf("    %s commands/second, mean latency %.1fu\n", formatInt(speed), latency)

}
