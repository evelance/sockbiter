--[[ sockbiter: HTTP load generator script ]]-- 

-- Parse shell arguments, print help
local argc, argv = ...
local help = [=[
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
    -human, -h       Use human-readable number formats to print results.
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
if argc < 2 then
    print(help)
    return 1
end
local options = {
    nreq = 1, nconns = 1, nocheck = false, shutwr = false, human = false,
    show_sample = true, show_conndetails = true, show_timings = true, show_summary = true,
}
local uri, option
for i = 1, argc - 1 do
    if option then
        -- If option is set, this argument is the value for the option.
        if option == "c" then
            local n = tonumber(argv[i])
            if math.type(n) ~= "integer" or n <= 0 then
                print("Error in option -c: Expected positive nonzero integer"
                    .." as number of parallel connections, but got '"..argv[i].."'")
                return 1
            end
            options.nconns = n
        elseif option == "n" then
            local n = tonumber(argv[i])
            if math.type(n) ~= "integer" or n <= 0 then
                print("Error in option -n: Expected positive nonzero integer"
                    .." as number of requests, but got '"..argv[i].."'")
                return 1
            end
            options.nreq = n
        end
        option = nil
    elseif argv[i]:sub(1, 1) == "-" then
        -- Otherwise, if it starts with "-", a new option will be set.
        local op = argv[i]:sub(2)
        if op == "h" or op == "-h" or op == "help" or op == "-help" then
            print(help)
            return 1
        end
        if op == "nocheck" then
            options.nocheck = true
        elseif op == "shutwr" then
            options.shutwr = true
        elseif op == "human" or op == "h" then
            options.human = true
        elseif op == "no-sample" then
            options.show_sample = false
        elseif op == "no-perconn" then
            options.show_conndetails = false
        elseif op == "no-timings" then
            options.show_timings = false
        elseif op == "no-summary" then
            options.show_summary = false
        elseif op == "c" or op == "n" then
            option = op
        else
            print("Error: Unknown option '"..argv[i].."'.")
            print("To show a list of all options, use --help")
            return 1
        end
    else
        -- If it is neither an option nor an argument, it is the URI.
        uri = argv[i]
        break
    end
end
if option then
    print("Error: Missing value for option '"..option.."'")
    return 1
end
if not uri then
    print("Error: URI is missing")
    return 1
end

-- Extract and validate URI parts
if uri:sub(1, 7) ~= "http://" then
    print("Error: Expected URI starting with 'http://' but got '"..uri.."'")
    return 1
end
local uri_noproto = uri:sub(8)
local port_start, target_start
for i = 1, #uri_noproto do
    local c = uri_noproto:sub(i, i)
    if c == ":" and not port_start then
        port_start = i
    end
    if c == "/" then
        target_start = i
        break
    end
end
local host, port, target = uri_noproto, "80", "/"
if port_start then
    port = target_start and uri_noproto:sub(port_start + 1, target_start - 1) or uri_noproto:sub(port_start + 1)
    host = uri_noproto:sub(1, port_start - 1)
end
if target_start then
    target = uri_noproto:sub(target_start)
    if not port_start then
        host = uri_noproto:sub(1, target_start - 1)
    end
end
if host == "" then
    print("Error: Host name is empty")
    return 1
end
local intport = math.tointeger(port)
if not intport or intport < 0 or intport > 65535 then
    print("Error: Expected port to be integer of [0..65535], but got '"..port.."'")
    return 1
end
port = tostring(intport)

-- Number formatting
if options.human then
    function format_bytes(b)
        local kb, mb, gb = b/1024, b/(1024*1024), b/(1024*1024*1024)
        if b < 1024 then
            return string.format("%12.2f B", b)
        end
        if kb < 1024 then
            return string.format("%12.2f KB", kb)
        end
        if mb < 1024 then
            return string.format("%12.2f MB", mb)
        end
        return string.format("%12.2f GB", gb)
    end
    function format_ns(ns, fmt)
        fmt = fmt or "%12.2f"
        local us, ms, s = ns/1.0e3, ns/1.0e6, ns/1.0e9
        if ns < 1000 then
            return string.format(fmt.." ns", ns)
        end
        if us < 1000 then
            return string.format(fmt.." us", us)
        end
        if ms < 1000 then
            return string.format(fmt.." ms", ms)
        end
        return string.format(fmt.." sec", s)
    end
    function format_rps(nreq, ns)
        local rps = nreq / (ns / 1.0e9)
        if rps > 1000 then
            if rps > 1000000 then
                return string.format("%12.2f M", rps / 1000000)
            else
                return string.format("%12.2f K", rps / 1000)
            end
        else
            return string.format("%12.2f", rps)
        end
    end
else
    function format_bytes(n)
        return string.format("%12.2f B", n)
    end
    function format_ns(ns, fmt)
        fmt = fmt or "%12.2f"
        return string.format(fmt.." ms", ns / 1.0e6)
    end
    function format_rps(nreq, ns)
        local rps = nreq / (ns / 1.0e9)
        return string.format("%12.2f", rps)
    end
end
function format_tp(b, ns)
    return format_bytes(b / (ns / 1.0e9)).."/sec"
end

-- Generate requests
local infile = "requests.txt"
local outfmt = "responses-%d.txt"
local req =
    "GET "..target.." HTTP/1.1\r\n"
  .."Host: "..host..":"..port.."\r\n"
  .."User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:88.0) Gecko/20100101 Firefox/88.0\r\n"
  .."Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n"
  .."Accept-Language: en-US,en;q=0.5\r\n"
  .."Accept-Encoding: gzip, deflate\r\n"
  .."Upgrade-Insecure-Requests: 1\r\n"
local req_close = req.."Connection: close\r\n\r\n"
local req_keepalive = req.."Connection: keep-alive\r\n\r\n"
if options.show_sample then
    print("------- Sample request -------")
    io.write(req_close)
end
print("Generating input file with "..options.nreq.." request"..(options.nreq == 1 and "" or "s").."..")
local f = assert(io.open(infile, "wb"))
for i = 1, options.nreq do
    if i < options.nreq then
        f:write(req_keepalive)
    else
        f:write(req_close)
    end
end
f:close()
os.execute("rm responses-*.txt")
print("")

-- Run benchmark
print("---------- Benchmark ---------")
print("Benchmarking "..host..":"..port)
print(" * Parallel connections: "..options.nconns)
print(" * Requests/connection:  "..options.nreq)
if options.shutwr then
    print(" * Connections will be closed after requests have been sent")
end
if options.nocheck then
    print(" * Responses will not be stored checked")
end
print("Waiting for completion...")
local start = cputime_ns()
local results, err = multi_sendfile(infile, outfmt, host, port, options.nconns, options.shutwr, options.nocheck)
local stop = cputime_ns()
if not results then
    print("Benchmark failed: "..tostring(err))
    return 1
end
print("Benchmark successful, "..format_ns(stop - start, "%.2f"))
print("")

-- Calculate total/min/max/average, first and last timestamps
local total_sent, total_received, valid_entries = 0, 0, 0
local sum_duration = 0.0
local max_duration, avg_duration, min_duration
local max_duration_id, min_duration_id
local sum_connect = 0.0
local max_connect, avg_connect, min_connect
local max_connect_id, min_connect_id
local earliest_connect_start, earliest_connect_end, last_receive_end
local connection_errors = {}
for i, v in ipairs(results) do
    if type(v) == "string" then
        connection_errors[v] = true
    else
        total_sent = total_sent + v.total_sent
        total_received = total_received + v.total_received
        -- Duration of full connection: connect to close
        local duration = v.receive_end_ns - v.connect_start_ns
        if not min_duration or duration < min_duration then
            min_duration = duration
            min_duration_id = i
        end
        if not max_duration or duration > max_duration then
            max_duration = duration
            max_duration_id = i
        end
        sum_duration = sum_duration + duration
        -- Connect() call; may take a long time when they are queued
        local connect_duration = v.connect_end_ns - v.connect_start_ns
        if not min_connect or connect_duration < min_connect then
            min_connect = connect_duration
            min_connect_id = i
        end
        if not max_connect or connect_duration > max_connect then
            max_connect = connect_duration
            max_connect_id = i
        end
        sum_connect = sum_connect + connect_duration
        -- First and last timestamps
        if not earliest_connect_start or v.connect_start_ns < earliest_connect_start then
            earliest_connect_start = v.connect_start_ns
        end
        if not earliest_connect_end or v.connect_end_ns < earliest_connect_end then
            earliest_connect_end = v.connect_end_ns
        end
        if not last_receive_end or v.receive_end_ns > last_receive_end then
            last_receive_end = v.receive_end_ns
        end
        valid_entries = valid_entries + 1
    end
end
if valid_entries > 0 then
    avg_duration = sum_duration / valid_entries
    avg_connect = sum_connect / valid_entries
end

-- Detailed per-connection results
if options.show_conndetails then
    print("----- Connection details -----")
    for i, v in ipairs(results) do
        print("[Connection "..i.." of "..options.nconns.."]")
        if type(v) == "string" then
            print("    Failure: "..v)
        else
            local total_time    = v.receive_end_ns - v.connect_start_ns
            local send_time     = v.send_end_ns - v.send_start_ns
            local receive_time  = v.receive_end_ns - v.receive_start_ns
            print("  Bytes sent . . . . . . . "..format_bytes(v.total_sent))
            print("  Bytes received . . . . . "..format_bytes(v.total_received))
            print("  Connect time . . . . . . "..format_ns(v.connect_end_ns - v.connect_start_ns))
            print("  Send time  . . . . . . . "..format_ns(send_time))
            print("  Receive time . . . . . . "..format_ns(receive_time))
            print("  Total time . . . . . . . "..format_ns(total_time))
            print("  Send throughput  . . . . "..format_tp(v.total_sent, send_time).." (only useful on localhost)")
            print("  Receive throughput . . . "..format_tp(v.total_received, receive_time))
            print("  Req/second (connected) . "..format_rps(options.nreq, v.receive_end_ns - v.send_start_ns))
            print("  Req/second . . . . . . . "..format_rps(options.nreq, v.receive_end_ns - v.connect_start_ns))
            -- Generate timeline: First send (">"), then receive ("<", or "X" when ">"), then connect and close ("*", "|")
            local chars = {}
            local steps = 40
            local start_time = v.connect_start_ns
            local time_per_step = (v.receive_end_ns - v.connect_start_ns) / (steps - 1)
            local begin_send = (v.send_start_ns - start_time) / time_per_step
            local end_send = (v.send_end_ns - start_time) / time_per_step
            for i = math.floor(begin_send), math.floor(end_send) do
                chars[i + 1] = ">"
            end
            local begin_recv = (v.receive_start_ns - start_time) / time_per_step
            local end_recv = (v.receive_end_ns - start_time) / time_per_step
            for i = math.floor(begin_recv), math.floor(end_recv) do
                chars[i + 1] = chars[i + 1] and "X" or "<"
            end
            chars[math.floor((v.connect_end_ns - start_time) / time_per_step) + 1] = "*"
            chars[math.floor((v.receive_end_ns - start_time) / time_per_step) + 1] = "|"
            for i = 1, steps do
                chars[i] = chars[i] or "."
            end
            print("  ["..table.concat(chars).."]")
        end
    end
    print("")
end

-- Timing table to compare the different connections start/duration/end
if options.show_timings then
    print("-------- Timing table --------")
    local steps = 50
    local start_time = earliest_connect_start
    local total_time = last_receive_end - start_time
    local time_per_step = total_time / (steps - 1)
    local i_maxlen = #tostring(#results)
    -- Sort by connect()
    local sorted_conns = {}
    for i, v in ipairs(results) do
        if type(v) == "table" then
            table.insert(sorted_conns, { conn_idx = i, v = v})
        end
    end
    table.sort(sorted_conns, function(a, b)
        return a.v.connect_end_ns < b.v.connect_end_ns
    end)
    -- Print time span
    print("Duration: "..format_ns(total_time, "%.2f")..", "..format_ns(time_per_step, "%.2f").." per column.")
    -- Print table
    for entry_idx, entry in ipairs(sorted_conns) do
        local conn_idx, v = entry.conn_idx, entry.v
        local chars = {}
        local begin_send = (v.send_start_ns - start_time) / time_per_step
        local end_send = (v.send_end_ns - start_time) / time_per_step
        for i = math.floor(begin_send), math.floor(end_send) do
            chars[i + 1] = ">"
        end
        local begin_recv = (v.receive_start_ns - start_time) / time_per_step
        local end_recv = (v.receive_end_ns - start_time) / time_per_step
        for i = math.floor(begin_recv), math.floor(end_recv) do
            chars[i + 1] = chars[i + 1] and "X" or "<"
        end
        chars[math.floor((v.connect_end_ns - start_time) / time_per_step) + 1] = "*"
        chars[math.floor((v.receive_end_ns - start_time) / time_per_step) + 1] = "|"
        for i = 1, steps do
            chars[i] = chars[i] or "."
        end
        print(string.rep(" ", i_maxlen - #tostring(conn_idx)).."#"..conn_idx.." ["..table.concat(chars).."] "..string.format("%"..i_maxlen.."d", entry_idx))
    end
    print("")
end

-- Summary of entire benchmark
if valid_entries <= 0 then
    print("No connection was successful.")
    return 1
end
if options.show_summary then
    print("----------- Summary ----------")
    local benchmark_duration = last_receive_end - earliest_connect_start
    print("Successful connections: "..valid_entries.." out of "..options.nconns.." ("..(options.nconns - valid_entries).." failed).")
    if valid_entries < options.nconns then
        print("Encountered errors:")
        for k, v in pairs(connection_errors) do
            print(" * "..k)
        end
    end
    print("Total bytes sent . . . . . "..format_bytes(total_sent))
    print("Total bytes received . . . "..format_bytes(total_received))
    print("Benchmark duration . . . . "..format_ns(benchmark_duration))
    print("Send throughput  . . . . . "..format_tp(total_sent, benchmark_duration))
    print("Receive throughput . . . . "..format_tp(total_received, benchmark_duration))
    print("Aggregate req/second . . . "..format_rps(options.nreq * valid_entries, benchmark_duration))
    print("Longest connection . . . . "..format_ns(max_duration).." (#"..max_duration_id..")")
    print("Average connection . . . . "..format_ns(avg_duration))
    print("Shortest connection  . . . "..format_ns(min_duration).." (#"..min_duration_id..")")
    print("Longest connect()  . . . . "..format_ns(max_connect).." (#"..max_connect_id..")")
    print("Average connect()  . . . . "..format_ns(avg_connect))
    print("Shortest connect() . . . . "..format_ns(min_connect).." (#"..min_connect_id..")")
end

return 0
