# Multiclient Pacman Game

## Overview
This project is a C implementation of a Pacman-inspired game built on a Client-Server architecture for my Operating Systems class. Its main focus is to demonstrate Inter-Process Communication (IPC) using Named Pipes (FIFOs), concurrency through Multithreading, and synchronization tools such as Mutexes, Condition Variables, and Semaphores.

## Architecture and Features

### Server
The server (`game.c`) acts as the central game engine, handling multiple clients simultaneously:
* **Connection Management (Producer-Consumer)**: The server receives initial connection requests from new clients via a central FIFO. These requests are safely placed in a circular buffer (using Mutexes and Semaphores) and subsequently consumed by worker threads.
* **Robust Multithreading**: Each individual game session operates with several threads:
  * One to evaluate Pacman's movement.
  * Multiple threads for each Ghost to calculate positions and collisions independently.
  * A render thread responsible for capturing the current board state (snapshot) and sending it in binary format to the respective client.
* **Top 5 Score Generation**: The server is set up to handle the `SIGUSR1` system signal. When triggered, the server identifies active users and exports the top 5 profiles sorted by score to a `top_scores.txt` file.
* **Level Files**: It autonomously reads saved levels with the `.lvl` extension.

### Client
The client (`client_main.c`) serves as the user's screen and game command issuer:
* **Terminal-based Interface**: Uses terminal manipulation libraries (like `ncurses`) to fluidly render the maps received from the server. An independent thread is dedicated solely to waiting for updates and redrawing the map display.
* **Two Usage Modes**: The player can interact by sending keystrokes in real-time via the keyboard or automatically submit text files containing previously recorded command sequences.
* **Signal Handling**: Handles obstructive signals like server shutdown (`SIGPIPE`) to prevent abrupt program crashes, and catches `SIGINT` (Ctrl+C) to ensure terminal formatting configurations are restored before exiting.

### Communication Protocol
The Client-Server interaction relies on a custom message format identified by fixed OpCodes:
* `OP_CODE_CONNECT`: Connect the client.
* `OP_CODE_DISCONNECT`: Disconnect or close the client.
* `OP_CODE_PLAY`: Request and transmit a specific move.
* `OP_CODE_BOARD`: Update the full board state to the player's screen.

## Getting Started

### 1. Starting the Server
With the binaries built and the levels in the respective folder, start the server:
```bash
./server <levels_dir> <max_games> <registration_fifo>
