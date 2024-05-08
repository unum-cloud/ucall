# Benchmark UCall

UCall can produce both a POSIX compliant old-school server, and a modern `io_uring`-based version for Linux kernel 5.19 and newer.
You would either run `ucall_example_sum_posix` or `ucall_example_sum_uring`.

```sh
sudo apt-get install cmake g++ build-essential
cmake -DCMAKE_BUILD_TYPE=Release -B ./build_release  && make -C ./build_release
./build_release/build/bin/ucall_example_sum_posix &
./build_release/build/bin/ucall_example_sum_uring &
go run test/bench.go
kill %%
```

Alternatively try [wrk](https://github.com/wg/wrk):

```sh
wrk -t1 -c32 -d2s http://localhost:8545/ -s json.lua
wrk -t1 -c32 -d2s http://localhost:8545/ -s json16.lua
```

# Testing UCall

```sh
go run test/test.go
```

