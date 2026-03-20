# FIFO Support Implementation (Updated)

## Overview
Added support for writing stream data to one or multiple Linux FIFO (named pipe) files in addition to UDP sockets. This allows the application to output MPEG-TS streams to both network destinations and local named pipes, with support for multiple simultaneous FIFO outputs for different purposes (e.g., encoding and monitoring).

## Key Features

✅ **Multiple FIFO Support** - Send same stream to multiple FIFOs simultaneously
✅ **Flexible Output Modes** - UDP only, FIFO only, or both
✅ **Comma-Separated Paths** - Simple configuration using `fifo_paths=/path1, /path2, /path3`
✅ **Non-Blocking I/O** - Prevents SRT thread blocking
✅ **Backward Compatible** - Default UDP behavior unchanged

## Changes Made

### 1. **NetBridge.h** - Data Structures

Updated `Connection` struct:
```cpp
std::vector<int> mFifoFds;            // Multiple FIFO file descriptors
std::vector<std::string> mFifoPaths;  // Paths to FIFO files
OutputType mOutputType;               // Type of output
```

Updated `Config` struct:
```cpp
std::vector<std::string> mFifoPaths;  // List of FIFO paths (comma-separated in config)
OutputType mOutputType;               // UDP, FIFO, or BOTH
```

Added helper methods:
- `createAndOpenFifos()` - Create and open multiple FIFOs
- `closeFifos()` - Close all file descriptors
- `sendData()` - Route data to UDP and/or all FIFOs

### 2. **NetBridge.cpp** - Implementation

Key changes:
- `createAndOpenFifos()` iterates through vector of paths, creating each FIFO
- `closeFifos()` closes all file descriptors in the vector
- `sendData()` loops through `mFifoFds` vector to write to all configured FIFOs
- `startBridge()` and `addInterface()` now handle multiple FIFOs

### 3. **main.cpp** - Configuration Parsing

Added `parseFifoPaths()` helper function:
```cpp
std::vector<std::string> parseFifoPaths(const std::string& fifoPathString);
```
- Splits on commas
- Trims whitespace around paths
- Returns vector of cleaned paths

Updated config parsing in `addConfigSection()` and `addFlowSection()`:
- Parse `fifo_paths` parameter (comma-separated)
- Store in `mFifoPaths` vector

### 4. **config.ini** - Example Configuration

Examples demonstrating various scenarios:
- **config1** - UDP only (default)
- **config2** - Single FIFO
- **config3** - Multiple FIFOs (encoding + monitoring)
- **config4** - Both UDP and multiple FIFOs

## Configuration Examples

### Single FIFO
```ini
[config2]
listen_port=8001
listen_ip=0.0.0.0
key=th15i$4k3y
output_type=fifo
fifo_paths=/tmp/ts_stream.fifo
```

### Multiple FIFOs (Encoding and Monitoring)
```ini
[config3]
listen_port=8002
listen_ip=0.0.0.0
key=th15i$4k3y
output_type=fifo
fifo_paths=/tmp/ts_encode.fifo, /tmp/ts_monitor.fifo
```

### Dual Output (UDP + Multiple FIFOs)
```ini
[config4]
listen_port=8003
listen_ip=0.0.0.0
out_port=8104
out_ip=127.0.0.1
key=th15i$4k3y
output_type=both
fifo_paths=/tmp/ts_dual1.fifo, /tmp/ts_dual2.fifo
```

## Usage Examples

### Encoding Pipeline with Monitoring
```ini
[config_encoder]
listen_port=8000
output_type=fifo
fifo_paths=/tmp/ts_for_encoding.fifo, /tmp/ts_backup.fifo
```
Write to both an encoder process and a backup location

### Multiple Consumers
```ini
[flow_multi]
bind_to=config_main
tag=42
output_type=fifo
fifo_paths=/tmp/consumer1.fifo, /tmp/consumer2.fifo, /tmp/consumer3.fifo
```
Same stream delivered to three different consumers without duplication overhead

## Testing Multiple FIFOs

```bash
# Terminal 1: Run server
./srt_to_udp_server

# Terminal 2: Monitor first FIFO
cat /tmp/ts_encode.fifo | ffprobe -i - 2>&1 | head -20

# Terminal 3: Read from second FIFO
cat /tmp/ts_monitor.fifo > /tmp/recorded_stream.ts

# Terminal 4: Send SRT stream (adjust port/path as needed)
srt-file-transmit file.ts srt://localhost:8002
```

## Path Parsing Details

The `parseFifoPaths()` function handles:

**Input:**
```
"/tmp/ts_encode.fifo, /tmp/ts_monitor.fifo, /tmp/ts_backup.fifo"
```

**Processing:**
- Splits on comma delimiter
- Trims whitespace: `" /tmp/ts_monitor.fifo"` → `/tmp/ts_monitor.fifo`
- Removes empty strings
- Returns vector with 3 cleaned paths

**Output:**
```
["/tmp/ts_encode.fifo", "/tmp/ts_monitor.fifo", "/tmp/ts_backup.fifo"]
```

## Data Flow

```
SRT Input → MPEG-TS Stream
    ↓
startBridge() / addInterface()
    ├─→ Creates all FIFOs from mFifoPaths vector
    ├─→ Stores all fd in mFifoFds vector
    └─→ Sets mOutputType
    ↓
Data received (handleDataMPEGTS/MPSRTTS)
    ↓
sendData(connection, data)
    ├─→ If UDP/BOTH: Send to UDP socket
    └─→ If FIFO/BOTH: Loop through mFifoFds and write to each
```

## Technical Notes

### Non-Blocking I/O
- All FIFOs opened with `O_NONBLOCK` flag
- Prevents SRT receive thread from blocking
- EAGAIN/EWOULDBLOCK errors handled gracefully (normal when no reader)

### File Permissions
- FIFOs created with mode `0666` (readable/writable by all)
- Can be restricted via umask if needed

### Error Handling
- If any FIFO fails to create, entire config is rejected
- Each write failure is logged but doesn't affect other outputs

### Performance
- Multiple FIFOs have minimal overhead (just loop through write calls)
- No data duplication—same pointer written to each fd
- Non-blocking writes prevent thread blocking

## Migration from Single FIFO

**Old config:**
```ini
fifo_path=/tmp/single.fifo
```

**New config (single FIFO):**
```ini
fifo_paths=/tmp/single.fifo
```

**New config (multiple FIFOs):**
```ini
fifo_paths=/tmp/primary.fifo, /tmp/secondary.fifo
```

Simply change from `fifo_path` (singular) to `fifo_paths` (plural).

## Benefits

- **Flexible Distribution** - Deliver same stream to multiple consumers
- **Local Processing** - Avoid network overhead for local processes
- **Monitoring** - Dedicated FIFO for monitoring/analysis separate from processing
- **Redundancy** - Write to multiple locations simultaneously
- **Backward Compatible** - No breaking changes to existing UDP configurations
