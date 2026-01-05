# VSFS Metadata Journaling

This project implements **metadata journaling** for a simplified educational file system (VSFS).  
The goal is to ensure **crash consistency** by logging filesystem metadata updates before they are committed to disk.

The project was completed as part of a university **Operating Systems / File Systems** term assignment.

---

## Overview

VSFS (Very Simple File System) supports basic file and directory operations.  
In this project, a **write-ahead journal** is added to protect filesystem metadata against crashes during updates.

The journaling mechanism ensures that:
- Metadata updates are first written to a dedicated journal area
- Operations are either **fully committed or safely rolled back**
- The filesystem remains consistent after unexpected failures

---

## Key Features

- Metadata-only journaling (inodes, bitmaps, directory entries)
- Fixed-size journal region on disk
- Transaction-based logging and commit mechanism
- Recovery support to replay or discard incomplete transactions

---

## Project Structure

```text
.
├── journal.c      # Journaling logic (implemented by me)
├── mkfs.c         # Filesystem image creator (provided)
├── validator.c    # Consistency checker (provided)
├── vsfs.img       # Generated filesystem image
└── README.md
.
```
---

## Build & Run

This project is written in **C** and tested on **Linux** using `gcc`.

### Build

Compile all components using the GNU Compiler Collection:

```bash
gcc -o mkfs mkfs.c
gcc -o journal journal.c
gcc -o validator validator.c
```
```bash
./mkfs
```
This initializes a fresh VSFS disk image with a journal region.
```bash
./journal create file1
./journal create file2
```
Each operation is logged to the journal before being committed to disk.
```bash
./validator
```
The validator checks the filesystem image for metadata consistency and verifies that journaling guarantees crash-safe updates.
