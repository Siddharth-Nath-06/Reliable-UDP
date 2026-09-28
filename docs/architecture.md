# System Architecture

## 1. Overview

The system implements reliable file transfer over UDP using interchangeable ARQ protocols. The architecture separates file handling, packet handling, ARQ reliability, transport, channel emulation, timeout estimation, and experiment analysis.

## 2. Architecture

```mermaid
%%{init: {
  'layout': 'elk',
  'theme': 'base',
  'themeVariables': {
    'primaryColor': '#083358',
    'primaryTextColor': '#ffffff',
    'primaryBorderColor': '#19507d',
    'lineColor': '#8ea7c2'
  }
}}%%
flowchart TD

    A([Driver])
    B[File Manager]

    subgraph PH[Packet Handler]
        C1[Packet Creator]
        C2[Packet Verifier]
        C3[Packet Dewrapper]
    end

    D[ARQ Factory]
    E[Stop-and-Wait]
    F[Go-Back-N]
    G[Selective Repeat]

    H[Transport API]
    I[UDP Transport]
    J[Channel Emulator]

    K[Timer Manager]
    L[RTO Manager]
    M[Monitoring]
    N[Experiment Data]
    O[Analysis and Graphs]

    P[File Reconstructor]

    A --> B
    A --> D

    D --> E
    D --> F
    D --> G

    B --> C1
    C1 --> E
    C1 --> F
    C1 --> G

    E --> H
    F --> H
    G --> H

    H --> I
    I --> J
    J --> I
    I --> H

    H --> C2
    C2 --> E
    C2 --> F
    C2 --> G

    E --> C3
    F --> C3
    G --> C3

    C3 --> P

    E --> K
    F --> K
    G --> K

    E --> L
    F --> L
    G --> L

    A --> M
    E --> M
    F --> M
    G --> M
    J --> M
    L --> M

    M --> N
    N --> O
```

The main architecture shows component relationships. The detailed transfer path is defined by the data-flow diagrams below.

## 3. Transfer Lifecycle

The complete transfer progresses through connection establishment, data transfer, acknowledgement exchange, reconstruction, integrity verification, and termination.

```mermaid
flowchart TD

    A[Transfer Configuration]
    B[Connection Established]
    C[Transfer Metadata Available]
    D[File Chunks Available]
    E[Packets Exchanged]
    F[File Reconstructed]
    G[File Integrity Verified]
    H[Connection Terminated]

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    G --> H
```

Control packets such as SYN, ACK, NACK, and FIN are exchanged through the same packet and transport path as data packets.

## 4. Data Flow

### 4.1 Sender

```mermaid
flowchart TD

    A[File Manager]
    B[File Chunk]
    C[Packet Creator]
    D[Packet]
    E[Selected ARQ]
    F[Configured Packet]
    G[Transport API]
    H[UDP Transport]
    I[Channel Emulator]

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    G --> H
    H --> I
```


### 4.2 Receiver

```mermaid
flowchart TD

    A[UDP Transport]
    B[Serialized Packet]
    C[Packet Verifier]
    D[Verified Packet]
    E[Selected ARQ]
    F[Accepted Packet]
    G[Packet Dewrapper]
    H[File Payload]
    I[File Reconstructor]

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    G --> H
    H --> I
```

The Transport API provides received serialized packet data to the Packet Verifier. The verifier checks the packet checksum. A corrupted packet is not passed to ARQ as trusted packet data.

A verified packet is passed to the selected ARQ implementation. ARQ determines whether the packet is accepted, buffered, or discarded according to the protocol. Only an accepted packet is passed to the Packet Dewrapper, which extracts the file payload for the File Reconstructor.

### 4.3 Control Packet Feedback

```mermaid
flowchart TD

    A[Receiver ARQ]
    B[Packet Creator]
    C[Control Packet]
    D[Sender ARQ]

    A --> B
    B --> C
    C --> D
```

The receiver ARQ generates ACK, NACK, and connection-control packets through the Packet Creator. The resulting control packet follows the same Transport API, UDP Transport, Channel Emulator, Packet Verifier, and ARQ path in the reverse direction.

The ARQ layer determines when a control packet is required and what protocol information it carries. The Packet Handler defines its packet representation.

## 5. Module Responsibilities

### 5.1 File Manager

Responsible for file-level operations:

- File input
- File chunking
- File hashing

The File Manager produces chunks for transmission and provides file information required for final integrity verification.

### 5.2 Packet Handler

The Packet Handler provides the packet operations required at different stages of transfer.

#### Packet Creator

- Initializes a packet from a file chunk or control information
- Provides the packet representation used by ARQ
- Provides serialization of the configured packet for transmission

#### Packet Verifier

- Checks the packet checksum
- Rejects corrupted packets

The verifier does not interpret an untrusted sequence or acknowledgement value from a corrupted packet.

#### Packet Dewrapper

- Extracts the payload from an accepted packet
- Passes recovered file data to the File Reconstructor

The Packet Handler does not implement ARQ behavior, retransmission, or window management.

### 5.3 ARQ Protocols

The selected ARQ implementation controls reliable transfer between the two endpoints:

- Stop-and-Wait
- Go-Back-N
- Selective Repeat

ARQ is responsible for:

- Configuring protocol-specific packet fields
- Sequence and acknowledgement handling
- Window management
- ACK and NACK handling
- Retransmission
- Duplicate and out-of-order handling
- Protocol state
- Determining whether a received packet is accepted, buffered, or discarded

The ARQ layer interacts with the Transport API rather than directly with UDP sockets.

### 5.4 Transport API

The Transport API provides the interface between ARQ and UDP communication.

Responsibilities include:

- Sending serialized packets
- Receiving serialized packets
- Address management

### 5.5 UDP Transport

UDP Transport is responsible only for UDP communication:

- UDP socket creation
- Binding
- Sending bytes
- Receiving bytes

It does not implement reliability, sequencing, retransmission, or timeout estimation.

### 5.6 Channel Emulator

The Channel Emulator acts as an intermediary between the two UDP endpoints.

```mermaid
flowchart TD

    A[UDP Sender]
    B[Channel Emulator]
    C[UDP Receiver]

    A -->|UDP Datagram| B
    B -->|UDP Datagram| C
```

It can introduce controlled network conditions:

- Packet loss
- Packet duplication
- Packet corruption
- Packet reordering
- Network delay
- Delay jitter

The same channel conditions apply to reverse-direction control packets.

### 5.7 Timer Manager

Provides timer functionality required by the ARQ protocols:

- Timer creation
- Timer start
- Timer restart
- Timer cancellation
- Timer expiration detection

Timer behavior may differ between Stop-and-Wait, Go-Back-N, and Selective Repeat.

### 5.8 RTO Manager

Provides adaptive retransmission timeout estimation:

- RTT measurement
- Jacobson/Karels estimation
- RTO calculation
- Karn's algorithm
- RTO state management

The ARQ protocols use the RTO Manager to obtain retransmission timeout values.

### 5.9 File Reconstructor

Responsible for receiver-side file reconstruction:

- Accepting file payloads from the Packet Dewrapper
- Ordering and writing received file data according to transfer state
- Final file integrity verification

### 5.10 Monitoring and Analysis

Runtime components produce monitoring data for experiments.

```mermaid
flowchart TD

    A[ARQ]
    B[Channel Emulator]
    C[RTO Manager]
    D[Monitoring]
    E[Experiment Data]
    F[Analysis and Graphs]

    A --> D
    B --> D
    C --> D
    D --> E
    E --> F
```

Relevant measurements include:

- Packets sent and received
- Retransmissions
- Timeouts
- RTT samples
- RTO values
- Transfer duration
- Bytes transferred
- Goodput

## 6. Timing Architecture

```mermaid
flowchart TD

    A[Selected ARQ]
    B[Timer Manager]
    C[RTO Manager]
    D[RTT Measurement]
    E[Jacobson/Karels]
    F[Karn's Algorithm]

    A --> B
    A --> C
    C --> D
    C --> E
    C --> F
```

The Timer Manager handles timer operation. The RTO Manager determines retransmission timeout values from RTT measurements and applies the selected estimation rules.

## 7. Architectural Principles

1. File handling is separate from packet and ARQ logic.
2. Packet creation, verification, and payload extraction are separate from ARQ behavior.
3. ARQ controls reliable transfer and protocol-specific packet fields.
4. The Packet Handler represents and serializes packets and validates received packet checksums.
5. Corrupted packet contents are not treated as trusted input to ARQ.
6. Transport provides UDP communication only.
7. The Channel Emulator is an independent network intermediary.
8. Timers and RTO estimation are separate services used by ARQ.
9. File reconstruction and final integrity verification occur at the receiver.
10. Monitoring and analysis do not determine protocol behavior.
