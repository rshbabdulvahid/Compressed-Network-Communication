default: client server
client: lab1b-client.c
	gcc -Wall -Wextra -lz lab1b-client.c -o lab1b-client
server: lab1b-server.c
	gcc -Wall -Wextra -lz lab1b-server.c -o lab1b-server
clean:
	rm -f lab1b-client lab1b-server
