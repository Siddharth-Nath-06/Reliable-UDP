# Reliable Data Transfer over UDP

## 0. Common Architecture

### t0: Common Architecture and Infrastructure
- t0.1: Project structure and module organization
- t0.2: Common data types and interfaces
- t0.3: Transport API
- t0.4: Configuration and shared parameters
- t0.5: Error handling and status conventions
- t0.6: Logging and common instrumentation interface

## 1. File Manager

### t1: File Manager
- t1.1: File input and output
- t1.2: File stream chunking
- t1.3: File hashing
- t1.4: File integrity verification

## 2. Packet Layer

### t2: Packet Encapsulation and Decapsulation
- t2.1: Packet field definition and filling
- t2.2: Packet serialization and deserialization
- t2.3: Packet verification
  - t2.3.1: Checksum
  - t2.3.2: Malformed packet validation
- t2.4: ACK packet handling
- t2.5: Sequence number representation

## 3. ARQ Protocols

### t3: Reliable Transfer Protocols
- t3.1: Stop-and-Wait ARQ
- t3.2: Go-Back-N ARQ
- t3.3: Selective Repeat ARQ
- t3.4: Timer infrastructure

## 4. RTT and RTO

### t4: Adaptive Timeout Estimation
- t4.1: RTT measurement
- t4.2: Jacobson/Karels RTT estimation
- t4.3: RTO calculation
- t4.4: Karn's algorithm
- t4.5: RTT and RTO instrumentation

## 5. Testing and Monitoring

### t5: Testing and Experiment Framework
- t5.1: Unit testing
- t5.2: Protocol correctness testing
- t5.3: Experiment configurations
- t5.4: Runtime monitoring and instrumentation
- t5.5: Experiment data organization

## 6. Channel Emulator

### t6: Network Channel Emulation
- t6.1: Packet loss
- t6.2: Packet duplication
- t6.3: Packet reordering
- t6.4: Packet corruption
- t6.5: Network delay
- t6.6: Delay jitter
- t6.7: Deterministic random seed
- t6.8: Channel configuration

## 7. Driver

### t7: Application Driver
- t7.1: Command-line/configuration interface
- t7.2: Module initialization
- t7.3: Protocol selection
- t7.4: Transfer execution
- t7.5: Runtime logging
- t7.6: Result collection

## 8. Graphs and Results

### t8: Experimental Analysis
- t8.1: Experimental data processing
- t8.2: Goodput analysis
- t8.3: Retransmission analysis
- t8.4: RTT and RTO analysis
- t8.5: Protocol performance comparison
- t8.6: Graph generation

## 9. Integration

### t9: System Integration
- t9.1: Module integration
- t9.2: End-to-end file transfer
- t9.3: Cross-protocol testing
- t9.4: File integrity validation
- t9.5: Reproducibility validation
- t9.6: Final system validation

## 10. Documentation

### t10: Project Documentation
- t10.1: Project overview
- t10.2: System architecture
- t10.3: Packet format specification
- t10.4: Protocol documentation
- t10.5: Testing and experiment methodology
- t10.6: Results and analysis
- t10.7: Build and usage documentation
