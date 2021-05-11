# sockbiter
HTTP/1 load generator and server analyzer based on multithreaded sendfile

The goal of this program is to benchmark the raw request-processing capabilities
of an HTTP server. It is intended to run on the same machine as the server, but
can also be used over the network.

## How it works

It works by generating a file containing many HTTP requests (requests.txt),
opening one or more connections to the target server, and then using blocking
sendfile() operations to send the requests while using as little CPU as possible.
Responses are recorded into a file per connection (responses-<connection ID>.txt).
The server may need to be configured to allow many keep-alive requests. Although
it is planned, responses are not parsed for now.

For each connection, two threads are started that mostly block on I/O and record timestamps
when their operations are finished.

The timestamps are used for an extensive summary with requests per second, throughput,
and a connection timing table to gain insight into the threading model of the server.

## Command-line usage and options
```
sockbiter - HTTP/1.1 load generator and server analyzer
Usage: sockbiter [options] http://hostname[:port][/path]

Options:
    -c conns         Number of parallel connections.
    -n requests      Number of requests to perform for each connection.
    -nocheck         Do not store received responses.
                     This option is useful when the disk is too slow to
                     store responses without introducing delays.
    -shutwr          Half-close connection after all data has been sent.
                     This can cause problems with some servers.
    -human           Use human-readable number formats to print results.
    -no-sample       Do not show sample request.
    -no-perconn      Do not show per-connection details.
    -no-timings      Do not show timing table.
    -no-summary      Do not show summary.

Timing diagram explanation:
    Waiting for connect()
    |     connect() succeeded
    |     | Sending data, but not receiving anything
    |     | |  Sending and receiving       Connection closed
    |     | |  |    Receiving only         |    Waiting for other threads
    |     | |  |    |                      |    |
[.........*>>>XXX<<<<<<<<<<<<<<<<<<<<<<<<<<|...........]]=]
```

## Sample outputs

Benchmarking Python's standard library http.server, "hello world" BaseHTTPRequestHandler, on localhost
```
./sockbiter -c 8 -n 10000 -human -no-sample -no-perconn http://localhost:1234
Generating input file with 10000 requests..

---------- Benchmark ---------
Benchmarking localhost:1234
 * Parallel connections: 8
 * Requests/connection:  10000
Waiting for completion...
Benchmark successful, 4.71 sec

-------- Timing table --------
Duration: 4.70 sec, 96.00 ms per column.
#7 [*X<<<<|...........................................] 1
#8 [*XXXXXXX<<<<|.....................................] 2
#3 [*XXXXXXXXXXXXX<<<<|...............................] 3
#1 [*XXXXXXXXXXXXXXXXXXX<<<<|.........................] 4
#4 [*XXXXXXXXXXXXXXXXXXXXXXXXX<<<<|...................] 5
#5 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.............] 6
#2 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.......] 7
#6 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|] 8

----------- Summary ----------
Successful connections: 8 out of 8 (0 failed).
Total bytes sent . . . . .        24.80 MB
Total bytes received . . .        11.75 MB
Benchmark duration . . . .         4.70 sec
Send throughput  . . . . .         5.27 MB/sec
Receive throughput . . . .         2.50 MB/sec
Aggregate req/second . . .        17.01 K
Longest connection . . . .         4.70 sec (#6)
Average connection . . . .         2.64 sec
Shortest connection  . . .       586.09 ms (#7)
Longest connect()  . . . .       718.71 us (#6)
Average connect()  . . . .       611.68 us
Shortest connect() . . . .       465.62 us (#7)
```

Bechmarking my personal website (Apache2, only redirects to HTTPS), using the Internet
```
./sockbiter -c 16 -n 100 -human -no-perconn -no-sample http://evelance.de/
Generating input file with 100 requests..

---------- Benchmark ---------
Benchmarking evelance.de:80
 * Parallel connections: 16
 * Requests/connection:  100
Waiting for completion...
Benchmark successful, 2.19 sec

-------- Timing table --------
Duration: 2.19 sec, 44.63 ms per column.
 #3 [*<|...............................................]  1
#14 [*<<|..............................................]  2
 #4 [*<<|..............................................]  3
 #9 [*<<|..............................................]  4
 #1 [*<<|..............................................]  5
 #7 [*<<|..............................................]  6
#16 [*<<|..............................................]  7
#15 [*<<|..............................................]  8
 #6 [*<<|..............................................]  9
 #2 [.*<<|.............................................] 10
 #5 [.*<<<<<<<<<<<<<<<<<<<|............................] 11
#11 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.....] 12
#10 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.....] 13
 #8 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|] 14
#13 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.] 15
#12 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.] 16

----------- Summary ----------
Successful connections: 16 out of 16 (0 failed).
Total bytes sent . . . . .       507.73 KB
Total bytes received . . .       810.47 KB
Benchmark duration . . . .         2.19 sec
Send throughput  . . . . .       232.16 KB/sec
Receive throughput . . . .       370.59 KB/sec
Aggregate req/second . . .       731.61
Longest connection . . . .         2.19 sec (#8)
Average connection . . . .       811.36 ms
Shortest connection  . . .       129.61 ms (#3)
Longest connect()  . . . .        47.22 ms (#13)
Average connect()  . . . .        39.22 ms
Shortest connect() . . . .        25.62 ms (#3)
```

Benchmarking Google, using the Internet
```
./sockbiter -c 16 -n 100 -human -no-sample -no-perconn http://google.de/
Generating input file with 100 requests..

---------- Benchmark ---------
Benchmarking google.de:80
 * Parallel connections: 16
 * Requests/connection:  100
Waiting for completion...
Benchmark successful, 2.10 sec

-------- Timing table --------
Duration: 2.10 sec, 42.93 ms per column.
#11 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|......]  1
#13 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.......]  2
 #4 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|....]  3
#10 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|...]  4
 #2 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.......]  5
 #8 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|......]  6
#12 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.....]  7
 #9 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.....]  8
 #3 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|......]  9
 #7 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|...] 10
 #1 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|] 11
#14 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.] 12
#15 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|...] 13
#16 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|...] 14
 #5 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|..] 15
 #6 [.*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<|.] 16

----------- Summary ----------
Successful connections: 16 out of 16 (0 failed).
Total bytes sent . . . . .       504.61 KB
Total bytes received . . .       822.17 KB
Benchmark duration . . . .         2.10 sec
Send throughput  . . . . .       239.86 KB/sec
Receive throughput . . . .       390.81 KB/sec
Aggregate req/second . . .       760.55
Longest connection . . . .         2.10 sec (#1)
Average connection . . . .         1.96 sec
Shortest connection  . . .         1.83 sec (#2)
Longest connect()  . . . .        67.83 ms (#6)
Average connect()  . . . .        58.71 ms
Shortest connect() . . . .        46.78 ms (#11)
```
Benchmarking NodeJS "Hello World" on localhost with 500 connections/1000 requests each:
```
./sockbiter -c 500 -n 1000 -human -no-perconn -no-sample -no-timings  http://localhost/
Generating input file with 1000 requests..

---------- Benchmark ---------
Benchmarking localhost:80
 * Parallel connections: 500
 * Requests/connection:  1000
Waiting for completion...
Benchmark successful, 7.12 sec

----------- Summary ----------
Successful connections: 423 out of 500 (77 failed).
Encountered errors:
 * recv failed: Connection reset by peer
Total bytes sent . . . . .       130.30 MB
Total bytes received . . .        75.83 MB
Benchmark duration . . . .         7.00 sec
Send throughput  . . . . .        18.61 MB/sec
Receive throughput . . . .        10.83 MB/sec
Aggregate req/second . . .        60.42 K
Longest connection . . . .         7.00 sec (#371)
Average connection . . . .         7.00 sec
Shortest connection  . . .         6.99 sec (#74)
Longest connect()  . . . .        32.05 ms (#371)
Average connect()  . . . .        27.31 ms
Shortest connect() . . . .        22.43 ms (#74)
```

Benchmarking Nginx "Hello World" on localhost with 500 connections/1000 requests each:
```
./sockbiter -c 500 -n 1000 -human -no-perconn -no-sample -no-timings  http://localhost/
Generating input file with 1000 requests..

---------- Benchmark ---------
Benchmarking localhost:80
 * Parallel connections: 500
 * Requests/connection:  1000
Waiting for completion...
Benchmark successful, 4.69 sec

----------- Summary ----------
Successful connections: 500 out of 500 (0 failed).
Total bytes sent . . . . .       154.02 MB
Total bytes received . . .       122.54 MB
Benchmark duration . . . .         4.62 sec
Send throughput  . . . . .        33.37 MB/sec
Receive throughput . . . .        26.55 MB/sec
Aggregate req/second . . .       108.33 K
Longest connection . . . .         4.61 sec (#369)
Average connection . . . .         2.34 sec
Shortest connection  . . .       106.54 ms (#470)
Longest connect()  . . . .        62.94 ms (#498)
Average connect()  . . . .        55.62 ms
Shortest connect() . . . .        45.91 ms (#82)
```

Benchmarking Apache2 "Hello World" on localhost with 100 connections/100000 requests each:
```
./sockbiter -c 100 -n 100000 -human -no-perconn -no-sample http://localhost/
Generating input file with 100000 requests..

---------- Benchmark ---------
Benchmarking localhost:80
 * Parallel connections: 100
 * Requests/connection:  100000
Waiting for completion...
Benchmark successful, 46.37 sec

-------- Timing table --------
Duration: 46.37 sec, 946.32 ms per column.
 #87 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<<|.]   1
 #91 [*XXXXXXXXXXXXXXXXXXXXX<<<|........................]   2
 #25 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<<|.]   3
  #4 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<<|.]   4
  #8 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.......]   5
 #13 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.]   6
  #1 [*XXXXXXXXXXXXXXXXXXXXX<<<|........................]   7
 #39 [*XXXXXXXXXXXXX<|..................................]   8
 #73 [*XXXXXXXXXXXXXXXX|................................]   9
 #93 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<<|..]  10
 #82 [*XXXXXXXXXXXXXXXX|................................]  11
 #99 [*XXXXXXXXXXXXXXXX|................................]  12
 [...]
 #95 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.......]  90
 #18 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.]  91
 #36 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<<|.......]  92
 #14 [*XXXXXXXXXXXXXX<|.................................]  93
 #27 [*XXXXXXXXXXX<<|...................................]  94
 #55 [*XXXXXXXXXXXXXXXXXXXXXX<|.........................]  95
 #29 [*XXXXXXXXXXXXXXXXXXXXX<<|.........................]  96
 #48 [*XXXXXXXXXXXXX<|..................................]  97
 #43 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|.]  98
 #53 [*XXXXXXXXXXXXXXXXXXXXX<<|.........................]  99
 #98 [*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX<<<<|........] 100

----------- Summary ----------
Successful connections: 100 out of 100 (0 failed).
Total bytes sent . . . . .         3.01 GB
Total bytes received . . .         2.73 GB
Benchmark duration . . . .        46.37 sec
Send throughput  . . . . .        66.43 MB/sec
Receive throughput . . . .        60.26 MB/sec
Aggregate req/second . . .       215.66 K
Longest connection . . . .        46.37 sec (#67)
Average connection . . . .        29.75 sec
Shortest connection  . . .        12.70 sec (#30)
Longest connect()  . . . .        35.34 ms (#98)
Average connect()  . . . .        16.39 ms
Shortest connect() . . . .         1.45 ms (#4)
```
