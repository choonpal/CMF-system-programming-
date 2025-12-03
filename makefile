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

SRCS_CLIENT = tui.c dir_manager.c chat_manager.c input_manager.c utils.c socket_client.c auth.c
OBJS_CLIENT = $(SRCS_CLIENT:.c=.o)

SRCS_SERVER = chat_server.c auth.c
OBJS_SERVER = $(SRCS_SERVER:.c=.o)

# ==========================
#   기본 빌드 대상
# ==========================
all: $(APP_CLIENT) $(APP_SERVER)

$(APP_CLIENT): $(OBJS_CLIENT)
	$(CC) $(OBJS_CLIENT) -o $@ $(LIBS) -lcrypto

$(APP_SERVER): $(OBJS_SERVER)
	$(CC) $(OBJS_SERVER) -o $@ -lpthread -lcrypto

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ==========================
#   실행 명령
# ==========================
ARGS = $(filter-out $@,$(MAKECMDGOALS))

run-server: $(APP_SERVER)
	@set -- $(ARGS); \
	if [ $$# -eq 0 ]; then \
	  echo "🚀 Running $(APP_SERVER)"; \
	  ./$(APP_SERVER); \
	elif [ $$# -eq 1 ]; then \
	  case "$$1" in \
	    *:*) echo "🚀 Running $(APP_SERVER) → $$1"; ./$(APP_SERVER) "$$1" ;; \
	    *[!0-9]*) echo "🚀 Running $(APP_SERVER) → $$1"; ./$(APP_SERVER) "$$1" ;; \
	    *) echo "🚀 Running $(APP_SERVER) → 127.0.0.1:$$1"; ./$(APP_SERVER) "127.0.0.1" "$$1" ;; \
	  esac; \
	else \
	  echo "🚀 Running $(APP_SERVER) → $$1:$$2"; \
	  ./$(APP_SERVER) "$$1" "$$2"; \
	fi

ARGS = $(filter-out $@,$(MAKECMDGOALS))

run-client: $(APP_CLIENT)
	@set -- $(ARGS); \
	if [ $$# -eq 0 ]; then \
	  echo "💬 Running $(APP_CLIENT) → 127.0.0.1:5050"; \
	  ./$(APP_CLIENT); \
	elif [ $$# -eq 1 ]; then \
	  echo "💬 Running $(APP_CLIENT) → $$1"; \
	  ./$(APP_CLIENT) "$$1"; \
	else \
	  echo "💬 Running $(APP_CLIENT) → $$1:$$2"; \
	  ./$(APP_CLIENT) "$$1" "$$2"; \
	fi

# make가 '127.0.0.1' 같은 추가 목표를 빌드하려고 하지 않도록 삼킴
%:: ; @:

# make run-client                      # 127.0.0.1:5050
# make run-client HOST=192.168.0.42    # 192.168.0.42:5050
# make run-client HOST=192.168.0.42 PORT=6000   # 192.168.0.42:6000
# 세 개의 입력 모두 다 가능

# ==========================
#   정리 명령
# ==========================
clean:
	rm -f $(OBJS_CLIENT) $(OBJS_SERVER) $(APP_CLIENT) $(APP_SERVER)
	@echo "🧹 Cleaned build files"

.PHONY: all clean run-server run-client
