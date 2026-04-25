# CS-4390 P2P File Sharing Project

This repository now contains a complete, runnable tracker + peer implementation for a socket-based P2P file-sharing demo.

## Implemented components

- **`tracker`**: central index server that handles:
  - `CREATETRACKER`
  - `UPDATETRACKER`
  - `REQ LIST` / `LIST`
  - `GET <filename>`
- **`peer`**: client + simple file server utility that supports:
  - Tracker requests (`list`, `createtracker`, `updatetracker`, `get`)
  - Peer file serving (`serve`)
  - File download from a `.track` response (`download`)

## Build

```bash
make
```

## Config files (included)

- `tracker.conf`
  - `PORT=3490`
  - `DB_DIR=tracker_db`
- `peer.conf`
  - `TRACKER_IP=127.0.0.1`
  - `TRACKER_PORT=3490`

You can now run using either direct host/port args or config file mode:

```bash
./tracker tracker.conf
./peer peer.conf list
```

## Run demo end-to-end

### Terminal 1: start tracker

```bash
./tracker tracker.conf
```

### Terminal 2: start peer file server for shared files

```bash
mkdir -p peer1_shared
cp /path/to/your/testfile.txt peer1_shared/
./peer serve peer1_shared 5001
```

### Terminal 3: create tracker entry for file

```bash
./peer 127.0.0.1 3490 createtracker peer1_shared/testfile.txt sample_file 127.0.0.1 5001
```

### Terminal 4: list available files

```bash
./peer 127.0.0.1 3490 list
```

### Terminal 5: fetch tracker metadata for file

```bash
./peer 127.0.0.1 3490 get testfile.txt testfile.track
```

### Terminal 6: download actual file from peer listed in track file

```bash
./peer download testfile.track downloaded_testfile.txt
```

## One-command demonstration script

```bash
./demo.sh
```

This script builds, starts services, creates tracking metadata, lists files, fetches `.track`, downloads content from a peer, and verifies integrity.

## Protocol summary

### Peer -> Tracker

- `REQ LIST\n`
- `CREATETRACKER <filename> <filesize> <desc_no_spaces> <md5> <ip> <port>\n`
- `UPDATETRACKER <filename> <ip> <port>\n`
- `GET <filename>\n`

### Tracker -> Peer

- List response:
  - `BEGIN_LIST <count>`
  - `<filename>|<size>|<md5>|<description>|<peer_count>` repeated
  - `END_LIST`
- Get response:
  - `BEGIN_TRACKER <filename> <size> <md5> <description> <peer_count>`
  - `PEER <ip> <port>` repeated
  - `END_TRACKER`
- Status lines:
  - `OK ...`
  - `ERR ...`

## Notes

- The tracker persists a human-readable `.track` file in `tracker_db/` for each tracked file.
- `peer createtracker` computes file size + md5 automatically (`md5sum`).
- Description strings passed on CLI are normalized by replacing spaces with underscores.
