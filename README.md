# Uninterrupted JSON RPC

A light-weight kernel bypass library using io_uring, SIMDJSON, Amazon Ion, Nvidia UCX to achieve ultimate performance.

Creating the UCX environment:

```sh
conda create -n ucx -c conda-forge -c rapidsai   ucx-proc=*=cpu ucx ucx-py python=3.9
```
