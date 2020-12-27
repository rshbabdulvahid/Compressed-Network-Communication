
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <getopt.h>
#include <sys/types.h>
#include <poll.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <zlib.h>

static int sockfd;
static int new_socket;
z_stream out_stream;
z_stream in_stream;

void sig_handler(int sig) {
  fprintf(stderr, "Caught signal %d\n", sig);
  shutdown(sockfd, 2);
  shutdown(new_socket, 2);
  deflateEnd(&out_stream);
  inflateEnd(&in_stream);
  exit(1);
}

int main(int argc, char* argv[]) {
  int flag = 0;
  int c;
  int bytes;
  char* program = NULL;
  char* port_str = NULL;
  int port = 0;
  char lf = 10;
  struct sockaddr_in address;
  int address_length = sizeof(address);
  int compress = 0;

  while(1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"shell", required_argument, 0, 's'},
      {"port", required_argument, 0, 'p'},
      {"compress", no_argument, 0, 'c'},
      {0, 0, 0, 0}};
    c = getopt_long(argc, argv, "", long_options, &option_index);
    if (c == -1)
      break;
    switch (c) {
    case 's':
      flag = 1;
      program = optarg;
      break;
    case 'p':
      port_str = optarg;
      port = atoi(port_str);
      break;
    case 'c':
      compress = 1;
      break;
    case '?':
      fprintf(stderr, "Unknown argument passed!\n");
      exit(1);
    default:
      break;
    }
  }
  
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    printf("Error while creating socket!\n");
    exit(1);
  }
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if (bind(sockfd, (struct sockaddr *) &address, address_length) < 0) {
    printf("Binding error!\n");
    exit(1);
  }
  
  if (listen(sockfd, 5) < 0) {
    printf("Listening error!\n");
    exit(1);
  }

  new_socket = accept(sockfd, (struct sockaddr *) &address, (socklen_t *) &address_length);
  if (new_socket < 0) {
    printf("Error while accepting!\n");
    exit(1);
  }

  if (compress == 1) {
    
    out_stream.zalloc = Z_NULL;
    out_stream.zfree = Z_NULL;
    out_stream.opaque = Z_NULL;

    int deflate_return = deflateInit(&out_stream, Z_DEFAULT_COMPRESSION);
    if (deflate_return != Z_OK) {
      fprintf(stderr, "Unable to create compression stream\n");
      exit(1);
    }
    
    out_stream.zalloc = Z_NULL;
    out_stream.zfree = Z_NULL;
    out_stream.opaque = Z_NULL;

    int inflate_return = inflateInit(&in_stream);
    if (inflate_return != Z_OK) {
      fprintf(stderr, "Unable to create decompression stream\n");
      exit(1);
    }
  }

  //STARTING CHILD PROCESSING
  if (flag == 1) {
    int fd0[2], fd1[2], pid;
    pipe(fd0); //P --> C
    pipe(fd1); //C --> P
    pid = fork();
    if (pid < 0) {
      fprintf(stderr, "Fork failed!");
      shutdown(sockfd, 2);
      shutdown(new_socket, 2);
      deflateEnd(&out_stream);
      inflateEnd(&in_stream);
      exit(1);
    }
    if (pid == 0) {
      //Child Function
      close(fd0[1]);
      close(fd1[0]);
      close(0);
      dup(fd0[0]);
      close(fd0[0]);
      close(1);
      dup(fd1[1]);
      close(2);
      dup(fd1[1]);
      close(fd1[1]);
      execl(program, "sh", (char *) NULL);
    }

    else {
      //Parent Program
      signal(SIGPIPE, sig_handler);
      close(fd0[0]);
      close(fd1[1]);
      struct pollfd fds[2];
      fds[0].fd = new_socket;
      fds[1].fd = fd1[0];
      fds[0].events = POLLIN | POLLHUP | POLLERR;
      fds[1].events = POLLIN | POLLHUP | POLLERR;
      
      while (1) {
	poll(fds, 2, 0);
	if (fds[0].revents & POLLERR) {
	  shutdown(sockfd, 2);
	  shutdown(new_socket, 2);
	  deflateEnd(&out_stream);
	  inflateEnd(&in_stream);
	  exit(1);
	}

	if (fds[1].revents & POLLERR || fds[1].revents & POLLHUP) {
	  int status;
	  waitpid(pid, &status, WUNTRACED);
	  int low_order = 0x00ff & status;
	  int upper = 0xff00 & status;
	  upper = upper >> 8;
	  upper = upper & 0x00ff;
	  fprintf(stderr, "SHELL EXIT SIGNAL=%dSTATUS=%d\n", low_order, upper);
	  shutdown(new_socket, 2);
	  shutdown(sockfd, 2);
	  deflateEnd(&out_stream);
	  inflateEnd(&in_stream);
	  exit(0);
	}

	if (fds[1].revents & POLLIN) {
	  char* buffer = (char*)malloc(256*sizeof(char));
	  char returnBuf[512];
	  bytes = read(fd1[0], buffer, 256);
	  int pos = 0;
	  for (int i = 0; i < bytes; i++) {
	    if (buffer[i] == 10) {
	      returnBuf[pos] = 13;
	      pos++;
	    }
	    returnBuf[pos] = buffer[i];
	    pos++;
	  }
	  if (compress == 1) {
	    char* outBuf = (char*)malloc(512*sizeof(char));
	    out_stream.next_in = (Bytef *)returnBuf;
	    out_stream.avail_in = pos;
	    out_stream.next_out = (Bytef *)outBuf;
	    out_stream.avail_out = 512;
	    while (out_stream.avail_in != 0) {
	      int res = deflate(&out_stream, Z_SYNC_FLUSH);
	      assert(res == Z_OK);
	    }
	    write(new_socket, outBuf, 512-out_stream.avail_out);
	    free(outBuf);
	  }
	  else 
	    write(new_socket, returnBuf, pos);
	  if (buffer[bytes-1] == '\004') {
	    int status;
	    waitpid(pid, &status, WUNTRACED);
	    int low_order = 0x00ff & status;
	    int upper = 0xff00 & status;
	    upper = upper >> 8;
	    upper = upper & 0x00ff;
	    fprintf(stderr, "SHELL EXIT SIGNAL=%dSTATUS=%d\n", low_order, upper);
	    shutdown(new_socket, 2);
	    shutdown(sockfd, 2);
	    free(buffer);
	    deflateEnd(&out_stream);
	    inflateEnd(&in_stream);
	    exit(0);
	  }
	  free(buffer);
	}

	if (fds[0].revents & POLLIN) {
	  char* buf1 = (char*)malloc(8*sizeof(char));
	  int bytes_read;
	  if (compress == 1) {
	    char* input = (char*)malloc(36*sizeof(char));
	    int available = read(new_socket, input, 36);
	    in_stream.next_in = (Bytef *)input;
	    in_stream.avail_in = available;
	    in_stream.next_out = (Bytef *)buf1;
	    in_stream.avail_out = 8;
	    while (in_stream.avail_in != 0) {
	      int res = inflate(&in_stream, Z_SYNC_FLUSH);
	      assert(res == Z_OK);
	    }
	    bytes_read = 8 - in_stream.avail_out;
	    free(input);
	  }
	  else {
	    bytes_read = read(new_socket, buf1, 8);
	  }
	  for (int i = 0; i < bytes_read; i++) {
	    if (buf1[i] == 0x03) {
	      int a = kill(pid, 2);
	      if (a < 0) {
		fprintf(stderr, "Failed to send interrupt to shell!");
		exit(1);
	      }
	    }
	    if (buf1[i] == '\004') {
	      close(fd0[1]);
	      break;
	    }
	    if (buf1[i] == 13) {
	      write(fd0[1], &lf, 1);
	    }
	    if (buf1[i] != 13) {
	      write(fd0[1], &buf1[i], 1);
	    }
	  }
	  free(buf1);
	}
      }
    }
  }

  shutdown(new_socket, 2);
  shutdown(sockfd, 2);
  deflateEnd(&out_stream);
  inflateEnd(&in_stream);
  exit(0);
}

