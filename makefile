# Compilatorul folosit
CC = gcc

# Flag-uri de compilare:
# -Wall -Wextra : Activeaza toate avertismentele (fii atent la ele!)
# -g            : Adauga informatii de debugging (util pentru gdb/valgrind)
# -pthread      : Linkeaza libraria de thread-uri (CRITIC pentru server)
# -D_GNU_SOURCE : Rezolva eroarea cu pthread_rwlock_t si alte extensii Linux
CFLAGS = -Wall -Wextra -g -pthread 

# Numele executabilelor
SERVER_BIN = server
CLIENT_BIN = client

# Directorul de cache (il definim ca sa putem face clean corect)
CACHE_DIR = ./cache

# Tinta implicita (ce se intampla cand scrii doar 'make')
all: $(SERVER_BIN) $(CLIENT_BIN)

# Regula pentru server
$(SERVER_BIN): server.c
	@echo "[BUILD] Compiling Server..."
	$(CC) $(CFLAGS) server.c -o $(SERVER_BIN)

# Regula pentru client
$(CLIENT_BIN): client.c
	@echo "[BUILD] Compiling Client..."
	$(CC) $(CFLAGS) client.c -o $(CLIENT_BIN)

# Regula pentru curatenie (sterge executabilele si folderul cache)
clean:
	@echo "[CLEAN] Removing binaries and cache..."
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
	rm -rf $(CACHE_DIR)

# Regula pentru a rula serverul rapid (optional)
run: $(SERVER_BIN)
	@echo "[RUN] Starting Server..."
	./$(SERVER_BIN)

# Spunem make-ului ca aceste tinte nu sunt fisiere reale
.PHONY: all clean run