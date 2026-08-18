CC = gcc
CFLAGS = -Wall -g
LDLIBS = -lm -lssh -lpthread

OBJS = main.o headers/ssh_connection.o headers/ssh_session.o headers/history.o headers/line_editor.o headers/dir_cache.o headers/stream.o

main: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o main $(LDLIBS)

clean:
	rm -f main *.o
