# Movidius Benchmark Tool - Comprehensive Performance Analysis

Interactive TUI (Terminal User Interface) for benchmarking and analyzing Movidius Neural Compute Sticks with real-time metrics and comprehensive analytics.

## Features

### Real-Time Monitoring
- **Multi-Device Support**: Monitor multiple Movidius devices simultaneously
- **Thermal Tracking**: Real-time temperature monitoring with 60-second history graphs
- **Memory Usage**: Track memory consumption with critical threshold alerts
- **Throughput Metrics**: Live inference/second counters with historical trends
- **Load Distribution**: Visualize work distribution across devices

### Comprehensive Analytics
- **Latency Statistics**: P50, P95, P99 percentiles with standard deviation
- **Performance Issues Detection**: Automatic identification of thermal, memory, and performance bottlenecks
- **Health Scoring**: Overall system health score (0-100) with actionable recommendations
- **Bottleneck Analysis**: Identifies input starvation, output stalling, and inefficiencies
- **Load Balancing**: Monitors and reports on work distribution quality

### Export & Reporting
- **JSON Export**: Export comprehensive performance reports with full metrics
- **Detailed Diagnostics**: Includes:
  - Per-device thermal, memory, performance, and resource metrics
  - Pool-wide statistics and load balance analysis
  - Detected issues with severity levels (Critical, Warning, Info)
  - Actionable recommendations for optimization
  - Historical trends and statistical analysis

## Quick Start

```bash
# Launch with convenience script
cd movidius-rs
./scripts/benchmark.sh

# Or run directly
cargo run --release -p movidius-bench
```

## Controls

| Key | Action |
|-----|--------|
| `q` | Quit the application |
| `r` | Reset all metrics |
| `Space` | Pause/Resume monitoring |
| `↑/↓` | Select different device |
| `s` | Cycle scheduling strategy (RoundRobin → LeastLoaded → PerformanceBased) |
| `a` | Toggle analysis view (future feature) |
| `e` | **Export comprehensive JSON report** |

## Understanding the UI

### Header
- Device count and current scheduling strategy
- Run status (RUNNING/PAUSED)
- Session uptime

### Left Panel
- **Device List**: Shows all detected devices with health status
  - `✓` Healthy
  - `⚠` Warning (thermal/memory pressure)
  - `✗` Error
- **Device Details**: For selected device shows:
  - Real-time temperature
  - Throttling status
  - Memory usage (MB and percentage)
  - Resource allocation (graphs and FIFOs)

### Right Panel
- **Pool Statistics**: Combined metrics across all devices
- **Throughput Graph**: 60-second sparkline of inferences/second
- **Temperature Graph**: 60-second thermal history
- **Load Distribution**: Bar chart showing work distribution

## Exported Report Structure

When you press `e`, a JSON file is created: `movidius_benchmark_YYYYMMDD_HHMMSS.json`

```json
{
  "start_time": 1234567890,
  "duration_secs": 123.45,
  "health_score": 85,
  "metrics": {
    "devices": [
      {
        "device_id": 0,
        "thermal": {
          "current_temp": 65.2,
          "avg_temp": 62.8,
          "peak_temp": 68.1,
          "is_throttling": false
        },
        "memory": {
          "used": 134217728,
          "total": 536870912,
          "percent": 25.0
        },
        "performance": {
          "throughput": 125.3,
          "avg_latency_ms": 7.98,
          "p95_latency_ms": 12.4,
          "p99_latency_ms": 15.7,
          "efficiency": 0.85
        }
      }
    ]
  },
  "issues": [
    {
      "severity": "Warning",
      "device_id": 1,
      "category": "Thermal",
      "description": "Device 1 temperature elevated: 76.2°C",
      "recommendation": "Monitor temperature, ensure adequate airflow"
    }
  ],
  "recommendations": [
    "✓ System operating optimally - no recommendations at this time"
  ]
}
```

## Performance Metrics Explained

### Latency Percentiles
- **P50** (median): 50% of requests complete in this time or less
- **P95**: 95% of requests complete in this time or less (good for SLA monitoring)
- **P99**: 99% of requests - shows tail latency behavior

### Health Score (0-100)
- **90-100**: Excellent - optimal operation
- **70-89**: Good - minor issues detected
- **50-69**: Fair - attention needed
- **Below 50**: Poor - critical issues present

### Issue Severity
- **Critical**: Immediate action required (throttling, memory critical >90%)
- **Warning**: Monitor closely (high temperature >75°C, memory >80%)
- **Info**: Minor observations, no immediate action needed

## Optimization Tips

### Based on Export Reports

1. **Thermal Issues Detected?**
   - Improve ventilation/cooling
   - Reduce workload intensity
   - Check for dust buildup

2. **Memory Pressure?**
   - Reduce batch size
   - Optimize model size
   - Consider adding more devices to pool

3. **Low Efficiency (<50%)?**
   - Check FIFO depth settings
   - Look for input/output bottlenecks
   - Verify proper threading configuration

4. **Load Imbalance?**
   - Try different scheduling strategies
   - Check if one device is throttling
   - Verify hardware capabilities match

5. **High Latency Variance?**
   - Usually indicates thermal throttling
   - Can indicate memory pressure
   - Check system load

## Feeding Results Back

After running benchmarks, you can:

1. Export reports with `e` key
2. Analyze the JSON files for trends
3. Compare baseline vs. optimized configurations
4. Share reports for collaborative optimization

The comprehensive metrics allow you to:
- Identify performance regressions
- Validate optimization improvements
- Understand system behavior under load
- Make data-driven tuning decisions

## Technical Details

### Metrics Collection
- Samples collected every 100ms (10Hz)
- Latency tracking: 1000 sample rolling window
- Temperature history: 60 seconds
- All metrics atomic and thread-safe

### Scheduling Strategies
- **RoundRobin**: Simple rotation, predictable distribution
- **LeastLoaded**: Select device with lowest queue depth
- **PerformanceBased**: Route to highest-performing device

### Optimal Configuration (from ncappzoo)
- 3 threads per device
- 6 async requests per thread
- Total: 18 concurrent requests per device
- For dual-device: 36 total concurrent operations

## Requirements

- Movidius Neural Compute Stick (Myriad X or later)
- Kernel module loaded: `modprobe movidius_x_vpu`
- Device files present: `/dev/movidius_x_vpu_*`
- Terminal with UTF-8 support for proper rendering

## Building from Source

```bash
cargo build --release -p movidius-bench
```

Binary will be at: `target/release/movidius-bench`

## Troubleshooting

### "No devices found"
- Check USB connection
- Verify kernel module: `lsmod | grep movidius`
- Check device files: `ls /dev/movidius_x_vpu_*`

### UI rendering issues
- Ensure terminal supports UTF-8
- Try resizing terminal window
- Some SSH sessions may have rendering limitations

### Export fails
- Check write permissions in current directory
- Ensure sufficient disk space
- Verify `chrono` crate is properly installed
