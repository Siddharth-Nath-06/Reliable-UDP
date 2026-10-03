# Interface Contract Rules

## Purpose

This document defines the architectural rules and guidelines that module owners should refer to when designing their public interfaces.

It establishes **what an interface must preserve** without prescribing the specific functions, structures, or implementation details of each module. Module owners are responsible for designing the APIs appropriate to their implementation.

## 1. General Interface Rules

- Each module owns its implementation and public interface.
- Public headers should expose only functionality required by other modules.
- Internal implementation details should remain private to the module.
- Shared types should use definitions from `common/types.h` where applicable.
- Modules must not directly access another module's internal state.
- Interfaces should expose the smallest API necessary to use the module correctly.

## 2. Ownership and Lifetime

- Every dynamically allocated object must have a clearly defined owner.
- The module responsible for creating an object should define how it is released.
- Ownership transfer must be explicit at the interface boundary.
- Borrowed data must not be freed or modified unless explicitly permitted by the interface.

## 3. Shared Types

`common/types.h` contains only types that genuinely cross module boundaries.

Currently shared types include:

- `PacketNumber`
- `PayloadLength`
- `Checksum`
- `TimerId`
- `FileChunk`
- `Packet`
- `ArqPacketInput`
- `ArqProtocol`

Module-specific types should remain within their respective modules.

## 4. Packet

The packet module owns:

- Packet construction
- Checksum calculation and verification
- Packet validation
- Serialization and deserialization
- Payload extraction

The packet module must follow the frozen packet format and maximum payload size defined in `common/constants.h`.

Sequence numbers are assigned by ARQ. `FileChunk` contains only raw file data and its length.

## 5. ARQ

The ARQ module owns:

- Protocol state
- Sequence-number assignment
- ACK/NACK behavior
- Window management
- Ordering and duplicate handling
- Retransmission decisions
- Timeout responses
- Delivery of accepted file data

ARQ must not implement UDP communication directly.

## 6. Packet Validation

Packet verification determines whether a received packet is valid.

If verification fails:

- The packet must be treated as invalid.
- ARQ must not rely on fields from the corrupted packet.
- The packet module must not decide the protocol response.
- ARQ determines whether and how to respond.

## 7. Transport

The transport module owns UDP communication, including:

- Socket management
- Datagram transmission
- Datagram reception
- Endpoint/address handling

Transport must not implement packet interpretation, checksum handling, ARQ behavior, or retransmission logic.

## 8. Channel

The channel module represents an unreliable communication path and may affect both data and control traffic.

It may provide:

- Loss
- Duplication
- Corruption
- Delay
- Jitter
- Reordering

The channel must not contain ARQ or retransmission logic.

## 9. Timer and RTO

The timer module provides timing mechanisms.

The RTO module provides RTT estimation and retransmission-timeout calculation.

Neither module decides when a packet should be retransmitted. Retransmission decisions belong to ARQ.

## 10. File and Reconstruction

### File Manager

The file module owns source-file access and splitting data into `FileChunk`s.

It does not assign packet sequence numbers.

### Reconstructor

The reconstruction module owns destination-file assembly and final file integrity verification.

ARQ is responsible for delivering accepted file data in the required order.

## 11. Monitor

The monitor is observational and may collect runtime measurements and transfer statistics.

Monitoring must not control protocol behavior or make retransmission decisions.

## 12. Dependencies

Modules should interact through public interfaces rather than implementation details.

In particular:

- `packet` must not depend on `arq`
- `transport` must not depend on `arq`
- `channel` must not depend on `arq`
- `timer` must not depend on `arq`
- `rto` must not depend on `arq`

The driver/integration layer coordinates the system.

## 13. Interface Evolution

Module owners may revise their interfaces as implementation requirements become clear.

Interfaces crossing module boundaries should be reviewed and agreed upon before dependent modules rely on them.

The shared architectural contracts and types in `common/` should not be changed without coordination with all affected modules.