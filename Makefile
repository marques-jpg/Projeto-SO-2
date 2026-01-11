# Root Makefile - Builds both server and client from subdirectories

.PHONY: all server client clean

all: server client

server:
	@echo "=== Building Server ==="
	$(MAKE) -C Projeto-SO

client:
	@echo "=== Building Client ==="
	$(MAKE) -C client-base

clean:
	@echo "=== Cleaning Server ==="
	$(MAKE) -C Projeto-SO clean
	@echo "=== Cleaning Client ==="
	$(MAKE) -C client-base clean
	@echo "=== Cleaning complete ==="

# Helper targets for running
run-server: server
	@cd Projeto-SO && ./bin/PacmanIST files 2 /tmp/server_fifo

run-client: client
	@cd client-base && ./bin/client 1 /tmp/server_fifo