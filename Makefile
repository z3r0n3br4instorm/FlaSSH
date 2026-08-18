CC = gcc
CFLAGS = -Wall -g
LDLIBS = -lm -lssh

main: main.o headers/ssh_connection.o headers/ssh_session.o
	$(CC) $(CFLAGS) main.o headers/ssh_connection.o headers/ssh_session.o -o main $(LDLIBS)

clean:
	rm -f main *.o
