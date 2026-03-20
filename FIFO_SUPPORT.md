# FIFO Support Implementation

## Overview
Added support for writing stream data to Linux FIFO (named pipe) files in addition to UDP sockets. This allows the application to output MPEG-TS streams to both network destinations and local named pipes.

## Changes Made

### 1. **NetBridge.h** - Data Structures
- Added `OutputType` enum with three modes:
  - `UDP` - Output only to UDP
  - `FIFO` - Output only to FIFO
  - `BOTH` - Output to both UDP and FIFO simultaneously

- Updated `Connection` struct:
  - Added `mFifoFd` - File descriptor for FIFO
  - Added `mFifoPath` - Path to FIFO file
  - Added `mOutputType` - Type of output for this connection

- Updated `Config` struct:
  - Added `mOutputType` - Output mode for configuration
  - Added `mFifoPath` - FIFO file path

- Added private helper methods:
  - `createAndOpenFifo()` - Create and open FIFO file
  - `writeFifo()` - Write data to FIFO
  - `closeFifo()` - Close FIFO file descriptor
  - `sendData()` - Send data to connection (UDP and/or FIFO)

### 2. **NetBridge.cpp** - Implementation
- Added includes for FIFO support: `<sys/stat.h>`, `<fcntl.h>`, `<unistd.h>`, `<errno.h>`

- Implemented FIFO helper methods:
  - `createAndOpenFifo()` - Creates FIFO with mkfifo() and opens with O_NONBLOCK flag
  - `writeFifo()` - Handles non-blocking writes with error handling
  - `sendData()` - Unified function to send data to UDP and/or FIFO based on output type

- Updated `startBridge()`:
  - Creates FIFO if output type is FIFO or BOTH
  - Initializes UDP socket only if needed

- Updated `addInterface()`:
  - Similar FIFO handling as startBridge

- Updated data handlers:
  - `handleDataMPEGTS()` - Now uses `sendData()` instead of direct UDP sends
  - `handleDataMPSRTTS()` - Now uses `sendData()` instead of direct UDP sends

### 3. **main.cpp** - Configuration Parsing
- Updated `addConfigSection()`:
  - Parses new `output_type` config option (default: "udp")
  - Parses new `fifo_path` config option

- Updated `addFlowSection()`:
  - Same configuration parsing as addConfigSection

### 4. **config.ini** - Example Configuration
Added three example configurations demonstrating FIFO usage:

- **config1** - Traditional UDP output
- **config2** - FIFO-only output to `/tmp/ts_stream.fifo`
- **config3** - Dual output to both UDP and FIFO
- **flow1, flow2** - UDP flows (examples)
- **flow3** - FIFO flow example

## Configuration Examples

### UDP Output (Default)
```ini
[config1]
listen_port=8000
listen_ip=0.0.0.0
out_port=8100
out_ip=127.0.0.1
key=th15i$4k3y
output_type=udp
```

### FIFO-Only Output
```ini
[config2]
listen_port=8001
listen_ip=0.0.0.0
key=th15i$4k3y
output_type=fifo
fifo_path=/tmp/ts_stream.fifo
```

### Dual Output (UDP + FIFO)
```ini
[config3]
listen_port=8002
listen_ip=0.0.0.0
out_port=8103
out_ip=127.0.0.1
key=th15i$4k3y
output_type=both
fifo_path=/tmp/ts_stream_dual.fifo
```

## Usage Notes

1. **FIFO Creation**: The application will create the FIFO file if it doesn't exist
2. **Non-Blocking I/O**: FIFOs are opened with `O_NONBLOCK` flag to prevent blocking on the SRT receive thread
3. **Error Handling**: Silent failure (EAGAIN) is acceptable - means no reader on the FIFO
4. **File Permissions**: FIFOs are created with 0666 permissions (rwx for all)
5. **Both Mode**: When using "both" output type, data is sent to both UDP and FIFO simultaneously

## Benefits

- **Flexible Output**: Stream data can now be written to files, pipes, or network sockets
- **Local Processing**: FIFO allows local processes to read MPEG-TS data easily
- **No UDP Complexity**: Avoids network stack overhead for local inter-process communication
- **Backward Compatible**: Default behavior remains UDP-only if not configured

## Technical Details

### FIFO Behavior
- Uses non-blocking I/O (`O_NONBLOCK`) to prevent SRT thread blocking
- Handles `EAGAIN` / `EWOULDBLOCK` gracefully (no reader connected)
- Automatically creates FIFO with `mkfifo()` if it doesn't exist
- File descriptors are stored per-connection and used throughout the session

### Data Flow
```
SRT Server  → handleDataMPEGTS/MPSRTTS → sendData() → UDP Socket / FIFO FD
           ↓
    Monitor stream data and route based on stream_id/tag
```

## Testing

To test FIFO output:

```bash
# Terminal 1: Run the server
./srt_to_udp_server

# Terminal 2: Read from FIFO
cat /tmp/ts_stream.fifo | xxd   # or pipe to ffplay, etc.

# Terminal 3: Send SRT stream
# Use your SRT client to send data to the configured listen port
```
