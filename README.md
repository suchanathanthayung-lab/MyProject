# MyProject
# Concurrent Chat Server (C, Linux, select)

A simple concurrent TCP chat server and client written in C using `select()`.  
Developed as a learning project to demonstrate **system programming** and basic **system engineering** practices.

## Features
- Support multiple clients concurrently (`select()`)
- Each client has a unique ID (`cli-XXXX`)
- Broadcast messages from one client to all others
- Keep the last N messages (default 10) in a circular history
- Commands:
  - `viewlist` → show history (server + client)
  - `health` → server replies `OK`
- Configurable via environment variables:
  - `SERV_IP` (default: 127.0.0.1)
  - `SERV_PORT` (default: 18800)
  - `MAX_HIST` (default: 10)

## Build
```bash
gcc -Wall -O2 cserv.c -o cserv
gcc -Wall -O2 dcli.c -o dcli
```

## Run
### Start server
```bash
./cserv
```

### Start clients
```bash
./dcli 1234
./dcli        # auto-generate ID
```

## Example Usage
```
Dcli1234> hello
Dcli5678> hi
cli-1234 says: hello
cli-5678 says: hi
```

### View message history
- From client:
```
Dcli1234> viewlist
cli-1234 says: hello
cli-5678 says: hi
<END>
```

- From server stdin:
```
Cserv> viewlist
cli-1234 says: hello
cli-5678 says: hi
```

### Health check
- From client:
```
Dcli1234> health
OK
```
