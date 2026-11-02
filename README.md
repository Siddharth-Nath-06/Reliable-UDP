# Reliable-UDP

Reliable file transfer over UDP using multiple Automatic Repeat reQuest (ARQ) protocols.

The project implements a modular system for reliable file transfer over an unreliable UDP channel, with the goal of experimentally comparing **Stop-and-Wait, Go-Back-N, and Selective Repeat** under configurable network conditions.

The system separates file handling, packet processing, ARQ reliability, UDP transport, channel emulation, timing, reconstruction, monitoring, and experiment analysis.

## Project Goals

- Implement and compare Stop-and-Wait, Go-Back-N, and Selective Repeat ARQ.
- Provide reliability through acknowledgements, sequence numbers, retransmission, and receiver-side ordering.
- Measure round-trip time (RTT) and estimate retransmission timeouts (RTO) using Jacobson/Karels estimation and Karn's algorithm.
- Emulate unreliable network conditions, including packet loss, duplication, corruption, reordering, delay, and jitter.
- Reconstruct transferred files and verify their integrity.
- Collect runtime measurements and analyze the performance of different protocols.

The primary experimental question is:

> How do Stop-and-Wait, Go-Back-N, and Selective Repeat differ in transfer performance as channel loss and window size vary?

## Documentation

| Document | Description |
|---|---|
| [Project specification](PROJECT.md) | Project scope, requirements, and implementation tasks |
| [System architecture](docs/architecture.md) | Components, responsibilities, transfer lifecycle, and data flow |
| [Module organization](docs/module_organization.md) | Source layout, module dependencies, and interface boundaries |
| [Packet format](docs/packet_format.md) | Packet header, flags, field sizes, and wire format |
| [Interface contract rules](docs/interface_contract_rules.md) | Shared types, ownership, module boundaries, and interface rules |
| [Experiment plan](docs/experiments/EXPERIMENT_PLAN.md) | Experimental design and planned comparisons |
| [Experiment automation guide](docs/experiments/AUTOMATION_README.md) | Experiment execution and automation workflow |
| [Final experiment report](docs/experiments/FINAL_REPORT.md) | Experiment report and conclusions |
| [AI usage and contributions](AI-USE.md) | Documentation of AI-assisted work |

## Architecture Overview

The system uses a modular architecture in which each component owns a distinct responsibility.

```mermaid
flowchart TD
    DRIVER[Driver / Integration]
    CONFIG[Configuration]
    FILE[File Manager]
    ARQ[ARQ Protocol]
    PACKET[Packet Handler]
    TRANSPORT[UDP Transport]
    CHANNEL[Channel Emulator]
    TIMER[Timer Manager]
    RTO[RTO Manager]
    RECON[File Reconstructor]
    MONITOR[Monitoring]
    DATA[Experiment Data]
    ANALYSIS[Analysis and Graphs]

    DRIVER --> CONFIG
    DRIVER --> FILE
    DRIVER --> ARQ
    DRIVER --> CHANNEL
    DRIVER --> MONITOR

    FILE --> ARQ
    ARQ --> PACKET
    PACKET --> TRANSPORT
    TRANSPORT --> CHANNEL
    CHANNEL --> TRANSPORT

    ARQ --> TIMER
    ARQ --> RTO
    ARQ --> RECON

    ARQ --> MONITOR
    CHANNEL --> MONITOR
    RTO --> MONITOR
    MONITOR --> DATA
    DATA --> ANALYSIS