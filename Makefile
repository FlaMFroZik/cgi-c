# Makefile — сборка FastCGI-приложения-библиотеки и примера.
#
#   make           собрать библиотеку и пример
#   make server    только библиотеку: ./libfcgi-server.a
#   make example   только пример: example/fcgi-server
#   make run       собрать всё и запустить пример
#   make clean     удалить всё собранное
#
# Приложение здесь — не готовая программа, а БИБЛИОТЕКА для
# вашего main.c: компоновка происходит на этапе сборки, а не
# во время работы (никаких .so и dlopen). Своя программа
# собирается так:
#
#   make server                                # -> libfcgi-server.a
#   cc -o myapp mymain.c libfcgi-server.a -pthread
#
# Переменные можно переопределить: make run PORT=9000 ROOT=/var/www
# Запуск через Unix-сокет вместо TCP: make run SOCK=/tmp/myapp.sock
# Отладочная сборка (сначала make clean):
#   make CFLAGS="-O0 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

AR     ?= ar
CFLAGS ?= -O2 -g
WARN   := -Wall -Wextra -Wpedantic
STD    := -std=c11

ALL_CFLAGS = $(STD) $(WARN) $(CFLAGS)
LDLIBS    += -pthread

# ---- библиотека сервера ----
LIB     := libfcgi-server.a
LIB_SRC := fcgi.c thread.c
LIB_OBJ := $(LIB_SRC:.c=.o)
DEP     := $(LIB_OBJ:.o=.d)

# ---- пример: main.c, скомпонованный с библиотекой ----
EXAMPLE_SRC := example/main.c
EXAMPLE_BIN := example/fcgi-server

# ---- параметры для make run ----
PORT ?= 9000
ROOT ?= example
SOCK ?=

.PHONY: all server example run clean

all: server example

server: $(LIB)

# Статическая библиотека: просто архив объектных файлов.
# Компоновщик возьмёт из неё код сервера при сборке вашей
# программы — до запуска.
$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

# -MMD -MP: заодно записать зависимости от .h в файл .d,
# чтобы после правки заголовка пересобиралось всё нужное.
%.o: %.c
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -pthread -MMD -MP -c -o $@ $<

example: $(EXAMPLE_BIN)

# main.c компилируется и компонуется с libfcgi-server.a за
# один вызов cc. Библиотека стоит после исходника:
# компоновщик разбирает архивы слева направо и берёт из них
# только то, что нужно уже собранным объектам.
$(EXAMPLE_BIN): $(EXAMPLE_SRC) fcgi.h $(LIB)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(EXAMPLE_SRC) $(LIB) $(LDLIBS)

run: all
ifdef SOCK
	./$(EXAMPLE_BIN) -s $(SOCK) -r $(ROOT)
else
	./$(EXAMPLE_BIN) -p $(PORT) -r $(ROOT)
endif

clean:
	rm -f $(LIB) $(LIB_OBJ) $(DEP) $(EXAMPLE_BIN)

-include $(DEP)
