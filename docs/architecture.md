# System Architecture

## 1. Overview

The system implements reliable file transfer over UDP using interchangeable ARQ protocols. Either of the two machines can initiate a file transfer, so sender and receiver are roles for a particular transfer rather than permanent machine identities. For each transfer, the machine currently sending the file runs the Channel Emulator locally. The architecture separates file handling, packet handling, ARQ reliability, transport, channel emulation, timeout estimation, file reconstruction, and experiment analysis.

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
        C3[Packet Serializer]
        C4[Packet Dewrapper]
    end

    D[ARQ Factory]
    E[Stop-and-Wait]
    F[Go-Back-N]
    G[Selective Repeat]

    H[Transport API]
    I[UDP Transport]
    J[Channel Emulator]
    J2[Remote UDP Endpoint]

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

    B --> E
    B --> F
    B --> G

    E --> C1
    F --> C1
    G --> C1

    C1 --> C3

    C3 --> H
    H --> I
    I --> J
    J --> I
    I --> H

    J --> J2

    H --> C3
    C3 --> C2

    C2 --> E
    C2 --> F
    C2 --> G

    E --> C4
    F --> C4
    G --> C4

    C4 --> P

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

**Transfer-role rule:** Either machine can initiate a transfer. The machine sending the file for that transfer runs its Channel Emulator locally. File data travels from the current sender through that emulator to the current receiver. Return traffic, including ACK and NACK packets, travels back through the same emulator so channel conditions can be applied in both directions. If the transfer direction reverses, the sender, receiver, and location of the Channel Emulator reverse as well.

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

Control packets such as SYN, ACK, NACK, and FIN are exchanged through the same Packet Handler and transport path as data packets.

## 4. Data Flow

### 4.1 Current Sender

```mermaid
flowchart TD

    A[File Manager]
    B[File Chunk]
    C[Selected ARQ]
    D[Packet Creator]
    E[Packet]
    F[Packet Serializer]
    G[Serialized Packet]
    H[Transport API]
    I[UDP Transport]
    J[Channel Emulator]

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    G --> H
    H --> I
    I --> J
```

The File Manager on the current sender provides raw file chunks to the selected ARQ protocol. ARQ determines whether and when a chunk is sent, assigns the required protocol information such as the sequence number and packet type, and requests construction of the corresponding packet. The sender's UDP Transport sends the serialized datagram to the Channel Emulator running on that same machine.

The Packet Creator constructs the logical packet from the information provided by ARQ. The Packet Serializer converts the packet into the wire-format byte sequence for transmission.

ARQ does not require the File Manager to construct or partially construct protocol packets.

### 4.2 Current Receiver

```mermaid
flowchart TD

    A[UDP Transport]
    B[Serialized Packet]
    C[Packet Serializer]
    D[Packet]
    E[Packet Verifier]
    F[Verified Packet]
    G[Selected ARQ]
    H[Accepted Packet]
    I[Packet Dewrapper]
    J[File Payload]
    K[File Reconstructor]

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    G --> H
    H --> I
    I --> J
    J --> K
```

The current receiver's Transport API provides received serialized packet data to the Packet Handler. The Packet Serializer deserializes the byte sequence into a packet representation.

The Packet Verifier checks the packet structure and checksum. A corrupted packet is treated as invalid and its untrusted fields are not used by ARQ.

A verified packet is passed to the selected ARQ implementation. ARQ determines whether the packet is accepted, buffered, or discarded according to the protocol. Only an accepted packet is passed to the Packet Dewrapper.

The Packet Dewrapper extracts the file payload and provides the resulting file data to the File Reconstructor.

ARQ is responsible for packet ordering and duplicate handling. The File Reconstructor does not implement ARQ behavior.

### 4.3 Control Packet Feedback

```mermaid
flowchart TD

    A[Current Receiver ARQ]
    B[Packet Creator]
    C[Control Packet]
    D[Packet Serializer]
    E[Receiver Transport]
    F[Channel Emulator<br/>On current sender's machine]
    G[Current Sender Transport]
    H[Packet Serializer]
    I[Packet Verifier]
    J[Current Sender ARQ]

    A --> B
    B --> C
    C --> D
    D --> E
    E -->|UDP to emulator endpoint| F
    F -->|Forwarded datagram| G
    G --> H
    H --> I
    I --> J
```

The current receiver's ARQ generates ACK, NACK, and connection-control packets through the Packet Creator. The Packet Handler serializes the resulting packet. The receiver sends the control datagram to the Channel Emulator endpoint on the current sender's machine; the emulator applies the configured channel behavior and forwards it to the current sender.

The current sender deserializes and verifies the control packet before passing it to its ARQ implementation.

The ARQ layer determines when a control packet is required and what protocol information it carries. The Packet Handler defines its packet representation and wire format.

## 5. Module Responsibilities

### 5.1 File Manager

Responsible for file-level operations:

- File input
- File chunking
- File hashing

The File Manager produces raw `FileChunk`s for transmission and provides file information required for final integrity verification.

The File Manager does not assign packet sequence numbers or construct protocol packets.

### 5.2 Packet Handler

The Packet Handler provides packet operations required at different stages of transfer.

#### Packet Creator

- Constructs packets from protocol information and payload data provided by ARQ
- Initializes packet fields
- Constructs both data and control packets

#### Packet Serializer

- Serializes packets into the defined wire format
- Deserializes received byte sequences into packets
- Handles network byte order
- Calculates and inserts the packet checksum
- Performs basic wire-format and buffer-length validation

#### Packet Verifier

- Validates received packet structure
- Verifies the packet checksum
- Rejects corrupted or invalid packets

The verifier does not interpret an untrusted sequence or acknowledgement value from a corrupted packet.

#### Packet Dewrapper

- Extracts the payload from an accepted DATA packet
- Converts the payload into file data for the File Reconstructor

The Packet Handler does not implement ARQ behavior, retransmission, ordering, or window management.

### 5.3 ARQ Protocols

The selected ARQ implementation controls reliable transfer between the two endpoints:

- Stop-and-Wait
- Go-Back-N
- Selective Repeat

ARQ is responsible for:

- Protocol state
- Sequence and acknowledgement handling
- Window management
- ACK and NACK handling
- Retransmission
- Duplicate and out-of-order handling
- Determining whether a received packet is accepted, buffered, or discarded
- Determining when protocol packets should be created
- Assigning protocol-specific packet information

ARQ interacts with the Transport API rather than directly with UDP sockets.

### 5.4 Transport API

The Transport API provides the interface between ARQ and UDP communication.

The Driver initializes the Transport with the local endpoint and configured peer/next-hop endpoint. After initialization, the selected ARQ implementation is the only module that uses the Transport for packet transmission and reception. The Driver closes the Transport when the transfer lifecycle ends.

For the current sender, the peer/next-hop endpoint is the Channel Emulator running locally. For the current receiver, the peer/next-hop endpoint for return control traffic is the Channel Emulator endpoint on the current sender's machine. The Transport does not decide transfer roles or configure channel behavior.

Responsibilities include:

- Sending serialized packets
- Receiving serialized packets
- Local endpoint binding
- Configured next-hop address management

The Transport API does not interpret packet contents, emulate network conditions, or implement reliability.

### 5.5 UDP Transport

UDP Transport is responsible only for UDP communication:

- UDP socket creation
- Binding to the configured local endpoint
- Sending bytes to the configured next-hop endpoint
- Receiving bytes from UDP

The configured next-hop endpoint depends on the transfer role. The current sender sends to its local Channel Emulator. The current receiver sends return traffic to that emulator's reachable endpoint on the current sender's machine. The Channel Emulator forwards datagrams between the two Transport endpoints and applies the configured channel conditions.

It does not implement reliability, sequencing, retransmission, checksum verification, channel emulation, or timeout estimation.

### 5.6 Channel Emulator

The Channel Emulator runs on the **same machine as the current sender** for each file transfer. The sender and receiver are not fixed machine identities: either machine can initiate a transfer, and the emulator's location follows the machine sending the file.

For a transfer from Machine A to Machine B:

```mermaid
flowchart LR

    A[Machine A<br/>Current Sender ARQ]
    B[Sender UDP Transport]
    C[Channel Emulator<br/>Machine A]
    D[Receiver UDP Transport]
    E[Machine B<br/>Current Receiver ARQ]

    A -->|Serialized datagram| B
    B -->|Local UDP| C
    C -->|Forwarded UDP| D
    D --> E
```

Return traffic, including ACK and NACK packets, passes through the same emulator in the opposite direction:

```mermaid
flowchart LR

    A[Machine B<br/>Current Receiver ARQ]
    B[Receiver UDP Transport]
    C[Channel Emulator<br/>Machine A]
    D[Sender UDP Transport]
    E[Machine A<br/>Current Sender ARQ]

    A --> B
    B -->|UDP to emulator endpoint on A| C
    C -->|Forwarded UDP| D
    D --> E
```

If Machine B initiates a transfer to Machine A, the arrangement reverses: the Channel Emulator runs on Machine B, and both data and return control traffic pass through that emulator.

The Channel Emulator can introduce controlled network conditions:

- Packet loss
- Packet duplication
- Packet corruption
- Packet reordering
- Network delay
- Delay jitter

The same configured channel conditions apply to traffic in both directions for a transfer. The Channel Emulator does not implement ARQ or retransmission logic.

The Channel Emulator therefore exposes or uses the UDP endpoints needed to receive datagrams from the current sender's Transport, forward them to the current receiver's Transport, and route return traffic through the same emulator. The exact emulator configuration and forwarding behavior are outside the Transport module.

### 5.7 Timer Manager

Provides timer functionality required by the ARQ protocols:

- Timer creation
- Timer start
- Timer restart
- Timer cancellation
- Timer expiration detection

Timer behavior may differ between Stop-and-Wait, Go-Back-N, and Selective Repeat.

The Timer Manager provides timing mechanisms but does not determine retransmission policy.

### 5.8 RTO Manager

Provides adaptive retransmission timeout estimation:

- RTT measurement
- Jacobson/Karels estimation
- RTO calculation
- Karn's algorithm
- RTO state management

The ARQ protocols use the RTO Manager to obtain retransmission timeout values.

The RTO Manager does not determine when retransmission occurs.

### 5.9 File Reconstructor

Responsible for receiver-side file reconstruction:

- Accepting file payloads from the Packet Dewrapper
- Writing and reassembling received file data
- Final file integrity verification

The File Reconstructor receives data after ARQ has handled packet ordering, duplication, buffering, and acceptance.

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

Monitoring and analysis do not determine protocol behavior.

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

ARQ uses the resulting timing information to make retransmission decisions.

## 7. Architectural Principles

1. File handling is separate from packet and ARQ logic.
2. File chunks contain raw file data and do not contain protocol sequence numbers.
3. ARQ receives file chunks, controls reliable transfer, and determines when protocol packets are created.
4. Packet creation, serialization, verification, and payload extraction are separate from ARQ behavior.
5. The Packet Handler represents, validates, and serializes packets but does not determine protocol behavior.
6. Corrupted packet contents are not treated as trusted input to ARQ.
7. Transport provides UDP communication only.
8. Either machine may be the current sender; the Channel Emulator runs on that machine and applies channel conditions to both data and return control traffic.
9. Timers and RTO estimation are separate services used by ARQ.
10. File reconstruction and final integrity verification occur at the receiver after ARQ has handled packet ordering and acceptance.
11. Monitoring and analysis do not determine protocol behavior.