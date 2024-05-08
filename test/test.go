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
    "strings"
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

func load_buffer( n int) {
    for i := 0; i < n; i++ {
        a := rand.Intn(1000)
        b := rand.Intn(1000)
        if ( n > 1 ) { buffer.WriteString(fmt.Sprintf(`[`)) }
        if i < n-1 {
          buffer.WriteString(fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":%d,"session_id":%d},"id":%d},`, a, b, i))
        } else {
          buffer.WriteString(fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":%d,"session_id":%d},"id":%d}`, a, b, i))
        }
    }
    if ( n > 1 ) { buffer.WriteString(fmt.Sprintf(`]`)) }
    //fmt.Printf("%s\n",buffer.String())
}

func test_rpc(tcpAddr *net.TCPAddr ) {
    print("  Test rpc ... ")    
    conn, err := net.DialTCP("tcp", nil, tcpAddr)
    if err != nil {
        println("connection failed:", err.Error())
        return
    }
    reply := make([]byte, 4096)

    req = fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":46,"session_id":0},"id":0}`)
    _, err = conn.Write([]byte(req))
    if err != nil {
        println("write error: %v\n", err)
        return
    }
    _, err = conn.Read(reply)
    if err != nil && !errors.Is(err, io.EOF) {
        println("read error: %v\n", err)
        return
    }
    rep := fmt.Sprintf(`{"jsonrpc":"2.0","id":0,"result":true}`)
    if strings.Compare( rep, string(reply[:]) ) == 0 {
        println("unexpected reply")
        println("    exp: ", rep)
        println("    act: ", string(reply[:]))
        return
    }

    println("successful")
}
func test_html(tcpAddr *net.TCPAddr ) {
    print("  Test html ... ")    
    conn, err := net.DialTCP("tcp", nil, tcpAddr)
    if err != nil {
        println("connection failed:", err.Error())
        return
    }
    reply := make([]byte, 4096)

    jrpc := fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":46,"session_id":0},"id":0}`)
    req := fmt.Sprintf(`POST / HTTP/1.1\r\nHost: localhost:8558\r\nUser-Agent: python-requests/2.31.0\r\nAccept-Encoding: gzip, deflate\r\nAccept: */*\r\nConnection: keep-alive\r\nContent-Length: %d\r\nContent-Type: application/json\r\n\r\n%s`, len(jrpc), jrpc)

    _, err = conn.Write([]byte(req))
    if err != nil {
        println("write error: %v\n", err)
        return
    }
    _, err = conn.Read(reply)
    if err != nil && !errors.Is(err, io.EOF) {
        println("read error: %v\n", err)
        return
    }
    rep := fmt.Sprintf(`{"jsonrpc":"2.0","id":0,"result":true}`)
    if rep != string(reply[:]) {
        println("unexpected reply")
        println("    exp: ", rep)
        println("    act: ", string(reply[:]))
        return
    }

    println("successful")
}
func test_big(tcpAddr *net.TCPAddr ) {
    print("  Test 4097 byte json ... ")    
    conn, err := net.DialTCP("tcp", nil, tcpAddr)
    if err != nil {
        println("connection failed:", err.Error())
        return
    }
    reply := make([]byte, 4096)
    pad := bytes.Repeat([]byte("a"), 3992)
    req = fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":46,"session_id":0},"id":0, "padding":"%s"}`,pad)
    _, err = conn.Write([]byte(req))
    if err != nil {
        println("write error: %v\n", err)
        return
    }
    _, err = conn.Read(reply)
    if err != nil && !errors.Is(err, io.EOF) {
        println("read error: %v\n", err)
        return
    }
    rep := fmt.Sprintf(`{"jsonrpc":"2.0","id":0,"result":true}`)
    if strings.Compare( rep, string(reply[:]) ) == 0 {
        println("unexpected reply")
        println("    exp: ", rep)
        println("    act: ", string(reply[:]))
        return
    }

    println("successful")
}
func test_partial(tcpAddr *net.TCPAddr ) {
    print("  Test partial ... ")    
    conn, err := net.DialTCP("tcp", nil, tcpAddr)
    if err != nil {
        println("connection failed:", err.Error())
        return
    }
    reply := make([]byte, 4096)

    req = fmt.Sprintf(`{"jsonrpc":"2.0","method":"validate_session","params":{"user_id":46,"session_id"`)
    _, err = conn.Write([]byte(req))
    if err != nil {
        println("write error: %v\n", err)
        return
    }
    time.Sleep(1000 * time.Millisecond)
    req = fmt.Sprintf(`:0},"id":0}`)
    _, err = conn.Write([]byte(req))
    if err != nil {
        println("write second part error: %v\n", err)
        return
    }
    _, err = conn.Read(reply)
    if err != nil && !errors.Is(err, io.EOF) {
        println("read error: %v\n", err)
        return
    }
    rep := fmt.Sprintf(`{"jsonrpc":"2.0","id":0,"result":true}`)
    if strings.Compare( rep, string(reply[:]) ) == 0 {
        println("unexpected reply")
        println("    exp: ", rep)
        println("    act: ", string(reply[:]))
        return
    }

    println("successful")
}


func client(c chan int, tcpAddr *net.TCPAddr, tid int ) {
	  reply := make([]byte, 4096)

    start := time.Now()
    transmits := 0
    conn, err := net.DialTCP("tcp", nil, tcpAddr)
    if err != nil {
	      println("Dial failed:", err.Error())
	      os.Exit(1)
    }

    for {
        _, err = conn.Write(buffer.Bytes())
        if err != nil {
            fmt.Printf("Write Error: %v\n", err)
            break
        }

        //conn.SetReadDeadline(time.Now().Add(time.Second*5))
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

func main() {

  flag.StringVar(&hostname,    "h", "localhost", "hostname")
  flag.IntVar(&port,           "p", 8545,        "port")
  flag.IntVar(&numConnections, "c", 16,          "Number of connections")
  flag.IntVar(&limitSeconds,   "s", 2,           "Stop after n seconds")
  flag.IntVar(&batch,          "b", 1,           "Batch n requests together")
  flag.BoolVar(&html,          "html", false,    "Send an html request instead of jsonrpc")
  flag.Parse()
  
  //var b [16]bytes.Buffer
  //b[0].WriteString("foo")
  //b[1].WriteString("bar")
  //fmt.Println(b)

    servAddr := fmt.Sprintf(`%s:%d`,hostname,port)
	tcpAddr, err := net.ResolveTCPAddr("tcp", servAddr)
	if err != nil {
		  println("ResolveTCPAddr failed:", err.Error())
		  os.Exit(1)
	}

    test_rpc(tcpAddr)
    test_html(tcpAddr)
    test_big(tcpAddr)
    test_partial(tcpAddr)

    //load_buffer(batch)


	//start := time.Now()
  //c := make(chan int)
	////for i := 0; i < numConnections; i++ {
      //go client( c, tcpAddr, i )
	//}

  // Wait for all connections to finish
  //transmits := 0
	//for i := 0; i < numConnections; i++ {
      //transmits += <-c
	//}

}
