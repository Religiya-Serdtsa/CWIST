-- wrk tail latency reporting script for CWIST benchmarks
done = function(summary, latency, requests)
    io.write("--------------------------------------------------\n")
    io.write(string.format("Requests/sec:  %12.2f\n", summary.requests / (summary.duration / 1000000)))
    io.write(string.format("Avg Latency:   %12.3f ms\n", (latency.mean / 1000)))
    io.write(string.format("Min Latency:   %12.3f ms\n", (latency.min / 1000)))
    io.write(string.format("Max Latency:   %12.3f ms\n", (latency.max / 1000)))
    io.write("Latency Distribution:\n")
    for _, p in ipairs({ 50, 75, 90, 99, 99.9, 99.99, 99.999 }) do
        io.write(string.format("  %7g%%:      %12.3f ms\n", p, latency:percentile(p) / 1000))
    end
    io.write("--------------------------------------------------\n")
end
