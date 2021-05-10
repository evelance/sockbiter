CFLAGS_HELP     = -Wall -Wextra -pedantic -fwrapv -Wshadow -Wsign-compare -Wundef -Wredundant-decls -Wno-unused-function

CFLAGS_RELEASE  = -O2 -march=native
CFLAGS_SANITIZE = -fsanitize=address -fsanitize=null -fsanitize=pointer-compare -fsanitize=pointer-subtract -fsanitize=undefined -fstack-protector-strong -fanalyzer
CFLAGS_GDB      = -ggdb -fno-omit-frame-pointer

CFLAGS          = $(CFLAGS_RELEASE) $(CFLAGS_HELP)

LUA_HOME        = lua-5.4.3-no-doc
LUA_FL          = -I$(LUA_HOME)/src/
LUA_LIB         = $(LUA_HOME)/src/liblua.a

sockbiter: sockbiter.c $(LUA_LIB) Makefile
	$(CC) $(CFLAGS) $(LUA_FL) -o $@ $< -pthread $(LUA_LIB) -ldl -lm

$(LUA_LIB): $(LUA_HOME)/src/*.c $(LUA_HOME)/src/*.h $(LUA_HOME)/src/*.hpp $(LUA_HOME)/Makefile $(LUA_HOME)/src/Makefile
	cd $(LUA_HOME) && $(MAKE) linux

clean:
	rm -rf *.o sockbiter requests.txt responses-*.txt
