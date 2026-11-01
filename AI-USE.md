# AI Use Log
## How to Use

This file documents the use of AI-assisted tools during the development of the project.

Each significant instance of AI use should be documented as a separate case. Team members should copy the template below, complete all applicable fields, and add the completed case to this file.

---

### AI-USE-[ID]: [Short Description]

| Field | Details |
|---|---|
| **Contributor / Date** | [Initials] - [DD-MM-YYYY] |
| **AI Tool** | [Tool and model, if known] |
| **Purpose** | [Why AI was used and what problem it addressed] |
| **Prompt / Context** | [What was asked and what relevant project context was provided] |
| **Files / Location** | [File paths, functions, classes, sections, or line ranges affected] |
| **AI Contribution** | [What AI generated, suggested, modified, explained, or reviewed] |
| **Human Changes / Verification** | [What was changed after AI output and how the result was verified] |
| **Final Use** | [What was ultimately incorporated into the project, including any rejected AI output] |
---

### Notes
- ID should be of the form \<your-initials-without-dot>-\<task-number-from-PROJECT.md>-\<n-th-log-for-the-task>
- For Instance, there are four collaborators on this project, so initials will be:
    - Siddharth Nath: SN
    - Hardik Kumar: HK
    - Pushpal Mukherjee: PM
    - Niharika Singh: NS
- These will be your respective initials for the purpose of reference in the log.
- An AI use for the task **t4.6**, done by Siddharth Nath (**SN**), for example will have ID: **SN-t4.6-0**, then the next use for the same task will be **SN-t4.6-1**, **SN-t4.6-2**, and so on.
- Below, the very first log entry is an example that you can use as reference.

## Log Entries

### AI-USE-SN-t0.1-0: Template for comprehensively logging AI use

| Field | Details |
|---|---|
| **Contributor / Date** | SN - 26-09-2026 |
| **AI Tool** | ChatGPT 5 |
| **Purpose** | To find a comprehensive template to log AI Use |
| **Prompt / Context** | Determine a concise format that team members can use to document individual cases of AI-USE for an academic project repository. |
| **Files / Location** | File: AI-USE.md |
| **AI Contribution** | Generated a table with mutiple fields for exhaustively describing a case of AI use. |
| **Human Changes / Verification** | Trimming of certain fields, modifying descriptions and field uses to suit our use-case better. |
| **Final Use** | The modified template table to be used when logging cases of AI use. |
---

<hr style="border: 2px solid; margin: 32px 0;">

### AI-USE-SN-t0.1-1: Generation of Mermaid Graph in Markdown files

| Field | Details |
|---|---|
| **Contributor / Date** | SN - 26-09-2026 |
| **AI Tool** | Gemini 3.1 Pro |
| **Purpose** | To convert physically drawn flowchart architecture diagrams to Markdown supported mermaid graph. |
| **Prompt / Context** | Convert the graphs/flowcharts in the given photos to mermaid graphs for markdown. Make sure to maintain alignment, apply proper color, and ensure the edges/arrows are not tangled. |
| **Files / Location** | **File:** docs/architecture.md, **Sections:** 2, 3, 4.3, 4.4, 5, 6. |
| **AI Contribution** | Generated mermaid graphs (Flowchart TD) versions of physically designed architecture diagrams. |
| **Human Changes / Verification** | Check rendered graph against physically drawn version. |
| **Final Use** | The Final graph was put in the markdown file. |
---

<hr style="border: 2px solid; margin: 32px 0;">

### AI-USE-SN-t2.3.1-0: Understanding of checksum verification logic

| Field | Details |
|---|---|
| **Contributor / Date** | SN - 23-10-2026 |
| **AI Tool** | ChatGPT |
| **Purpose** | To understand why checksum verification does not check for 0xFFFF |
| **Prompt / Context** | Check why the test for checksum fails. |
| **Files / Location** |  src\packet\packet_verifier.c, src\packet\packet_serializer.c, src\packet\packet_checksum.c |
| **AI Contribution** | Identify the fact that odd offset checksum cannot be checked by checksum of the full packet with existing checksum and checking against 0xFFFF. |
| **Human Changes / Verification** | Modify the code to accomodate the fact. Recalculate checksum for recieved packets by extracting the checksum and making it zero for calculation. |
| **Final Use** | The changed concept for verifying using checksum. |
---

<hr style="border: 2px solid; margin: 32px 0;">

## AI-USE-HK-t1.1-0: File Input and Output

| Field | Details |
|---|---|
| **Contributor / Date** | HK - 08-10-2026 |
| **AI Tool** | ChatGPT (GPT-6) |
| **Purpose** | Assistance with implementing and reviewing source-file access for reliable file transfer. |
| **Prompt / Context** | Develop the File Manager in accordance with the project architecture. It should handle source files without taking responsibility for packet sequencing, ARQ, or UDP communication. |
| **Files / Location** | `src/file/file_manager.h`, `src/file/file_manager.c`, `tests/test_file_manager.c` |
| **AI Contribution** | Explained and assisted with file opening, size tracking, reading, and closing behavior. |
| **Human Changes / Verification** | The implementation was reviewed against the module responsibilities. The uploaded test suite was compiled and run during this review. |
| **Final Use** | File input and output functionality was retained in the File Manager module. |

---

## AI-USE-HK-t1.2-0: File Stream Chunking

| Field | Details |
|---|---|
| **Contributor / Date** | HK - 08-10-2026 |
| **AI Tool** | ChatGPT (GPT-6) |
| **Purpose** | Assistance with reading a source file as bounded chunks for transfer. |
| **Prompt / Context** | Implement chunk reading while keeping file management separate from packet construction and sequence-number handling. |
| **Files / Location** | `src/file/file_manager.h`, `src/file/file_manager.c`, `tests/test_file_manager.c` |
| **AI Contribution** | Explained and assisted with chunk-size validation, remaining-byte calculation, allocation, and end-of-file behavior. |
| **Human Changes / Verification** | The implementation was reviewed against `MAX_PAYLOAD_SIZE` and the `FileChunk` interface. The uploaded test suite was compiled and run during this review. |
| **Final Use** | The chunk-reading functionality was retained in the File Manager module. |

---

## AI-USE-HK-t1.3-0: File Hashing

| Field | Details |
|---|---|
| **Contributor / Date** | HK - 08-10-2026 |
| **AI Tool** | ChatGPT (GPT-6) |
| **Purpose** | Assistance with calculating a source-file hash for integrity checking. |
| **Prompt / Context** | Add SHA-256 support for the source file and preserve the current read position while calculating the digest. |
| **Files / Location** | `src/file/file_manager.c`, `src/file/sha256.c`, `src/file/sha256.h`, `tests/test_file_manager.c`, `tests/test_sha256.c` |
| **AI Contribution** | Explained and assisted with SHA-256 integration and reading the complete file for digest calculation. |
| **Human Changes / Verification** | The hash implementation and its integration were reviewed. The uploaded File Manager and SHA-256 tests were compiled and run during this review. |
| **Final Use** | SHA-256 calculation was retained for source-file integrity information. |

---

## AI-USE-HK-t1.4-0: File Integrity Verification Support

| Field | Details |
|---|---|
| **Contributor / Date** | HK - 08-10-2026 |
| **AI Tool** | ChatGPT (GPT-6) |
| **Purpose** | Assistance with providing file-integrity information for later verification of the transferred file. |
| **Prompt / Context** | Support file integrity checking without moving packet or ARQ responsibilities into the File Manager. |
| **Files / Location** | `src/file/file_manager.c`, `src/file/file_manager.h`, `src/file/sha256.c`, `src/file/sha256.h`, `tests/test_file_manager.c`, `tests/test_sha256.c` |
| **AI Contribution** | Explained and assisted with exposing the source-file SHA-256 digest for comparison with the reconstructed file's digest. |
| **Human Changes / Verification** | The digest API and file-position preservation behavior were reviewed. The uploaded tests were compiled and run during this review. |
| **Final Use** | The File Manager's SHA-256 digest functionality was retained as the source-side integrity reference. |

---

## AI-USE-HK-t6.1-0: Channel Emulator Implementation and Testing

| Field | Details |
|---|---|
| **Contributor / Date** | HK - 30-10-2026 |
| **AI Tool** | ChatGPT (GPT-6) |
| **Purpose** | Assistance with implementing, debugging, and testing the network Channel Emulator. |
| **Prompt / Context** | Implement a datagram-level intermediary that can emulate unreliable network behavior without implementing ARQ or changing transport protocol semantics. The work covers related Channel Emulator tasks `t6.1` through `t6.8`. |
| **Files / Location** | `src/channel/channel_emulator.c`, `src/channel/channel_emulator.h`, `tests/test_channel_emulator.c` |
| **AI Contribution** | Assisted with queue/datagram handling and the configured loss, duplication, reordering, corruption, delay, jitter, deterministic seed, and configuration behavior. Also assisted with test design and debugging. |
| **Human Changes / Verification** | The implementation was reviewed and adapted. The uploaded Channel Emulator test suite was compiled and all 9 tests passed during verification. |
| **Final Use** | The Channel Emulator implementation and its tests were retained. |

---