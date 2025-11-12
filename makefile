# ==========================
#   TalkShell Makefile
# ==========================

APP_CLIENT = tui_chatops
APP_SERVER = chat_server

CFLAGS = -Wall -Wextra -O2 -D_XOPEN_SOURCE=700
LIBS = -lncursesw -lpthread

# detect OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LIBS = -lncurses -lpthread
endif

ifdef USE_INOTIFY
  CFLAGS += -DUSE_INOTIFY
endif

SRCS_CLIENT = tui.c dir_manager.c chat_manager.c input_manager.c utils.c socket_client.c
OBJS_CLIENT = $(SRCS_CLIENT:.c=.o)

SRCS_SERVER = chat_server.c
OBJS_SERVER = $(SRCS_SERVER:.c=.o)

# ==========================
#   기본 빌드 대상
# ==========================
all: $(APP_CLIENT) $(APP_SERVER)

$(APP_CLIENT): $(OBJS_CLIENT)
	$(CC) $(OBJS_CLIENT) -o $@ $(LIBS)

$(APP_SERVER): $(OBJS_SERVER)
	$(CC) $(OBJS_SERVER) -o $@ -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ==========================
#   실행 명령
# ==========================
run-server: $(APP_SERVER)
	@echo "🚀 Running TalkShell Server on port 5050..."
	./$(APP_SERVER)

run-client: $(APP_CLIENT)
	@echo "💬 Running TalkShell Client (TUI)..."
	./$(APP_CLIENT) 192.168.71.78 

# ==========================
#   정리 명령
# ==========================
clean:
	rm -f $(OBJS_CLIENT) $(OBJS_SERVER) $(APP_CLIENT) $(APP_SERVER)
	@echo "🧹 Cleaned build files"

.PHONY: all clean run-server run-client
