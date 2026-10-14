# Makefile — сборка cgi-server и примера CGI.
#
#   make           собрать сервер и пример
#   make server    только сервер: ./cgi-server
#   make example   только пример CGI: example/cgi.so и example/cgi
#   make run       собрать всё и запустить сервер с примером
#   make clean     удалить всё собранное
#
# Переменные можно переопределить: make run PORT=9000 ROOT=/var/www
# Отладочная сборка (сначала make clean):
#   make CFLAGS="-O0 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

CFLAGS ?= -O2 -g
WARN   := -Wall -Wextra -Wpedantic
STD    := -std=c11

ALL_CFLAGS = $(STD) $(WARN) $(CFLAGS)
LDLIBS    += -pthread

# В glibc до версии 2.34 функции dlopen()/dlsym() лежат в отдельной libdl.
ifeq ($(shell uname -s),Linux)
LDLIBS += -ldl
endif

# ---- сервер ----
SERVER := cgi-server
SRC    := socket.c import.c thread.c
OBJ    := $(SRC:.c=.o)
DEP    := $(OBJ:.o=.d)

# ---- пример CGI: один файл, две сборки ----
CGI_SRC := example/main.c
CGI_LIB := example/cgi.so
CGI_BIN := example/cgi

# ---- параметры для make run ----
PORT ?= 8080
ROOT ?= .

.PHONY: all server example run clean

all: server example

server: $(SERVER)

$(SERVER): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# -MMD -MP: заодно записать зависимости от .h в файл .d,
# чтобы после правки заголовка пересобиралось всё нужное.
%.o: %.c
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -pthread -MMD -MP -c -o $@ $<

example: $(CGI_LIB) $(CGI_BIN)

# CGI как динамическая библиотека — её импортирует сервер (ключ -l).
#   -fPIC    позиционно-независимый код (обязателен для .so)
#   -shared  собрать разделяемую библиотеку вместо программы
$(CGI_LIB): $(CGI_SRC) import.h
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $(CGI_SRC)

# Тот же файл как обычная программа — работает его main().
$(CGI_BIN): $(CGI_SRC) import.h
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(CGI_SRC)

run: all
	./$(SERVER) -p $(PORT) -r $(ROOT) -l ./$(CGI_LIB)

clean:
	rm -f $(SERVER) $(OBJ) $(DEP) $(CGI_LIB) $(CGI_BIN)

-include $(DEP)
