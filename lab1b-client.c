
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
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <zlib.h>
#include <assert.h>
#include <ulimit.h>

struct termios original;

void reset_mode() {
  tcsetattr(0, TCSANOW, &original);
}

void change_mode() {
  struct termios new;
  int status = tcgetattr(0, &original);
  if (status < 0) {
    fprintf(stderr, "Failed to get attributes from STDIN");
    exit(1);
  }
  atexit(reset_mode);
  status = tcgetattr(0, &new);
  new.c_iflag = ISTRIP;
  new.c_oflag = 0;
  new.c_lflag = 0;
  status = tcsetattr(0, TCSANOW, &new);
  if (status < 0) {
    fprintf(stderr, "Failed to set attributes");
    exit(1);
  }
}

int main(int argc, char* argv[]) {
  int c;
  int lflag = 0;
  int compress = 0;
  int port = 0;
  char* port_str = NULL;
  char* filename = NULL;
  char lf = 10;
  char cr = 13;
  struct sockaddr_in server_address;
  int sock = 0, address_length = sizeof(server_address);
  while(1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"log", required_argument, 0, 'l'},
      {"port", required_argument, 0, 'p'},
      {"compress", no_argument, 0, 'c'},
      {0, 0, 0, 0}};
    c = getopt_long(argc, argv, "", long_options, &option_index);
    if (c == -1)
      break;
    switch (c) {
    case 'l':
      lflag = 1;
      filename = optarg;
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
  change_mode();
  //Check errors and set up log/compression
  int ifd = 10;
  if (lflag == 1) {
    ifd = creat(filename, 0666);
    if (ifd < 0) {
      fprintf(stderr, "Error opening log file!\n");
      exit(1);
    }
    long a = ulimit(UL_SETFSIZE, 10000);
    if (a < 0) {
      fprintf(stderr, "Error with setting file limit!\n");
      exit(1);
    }
  }

  z_stream out_stream;
  z_stream in_stream;

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

  //set up socket here
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    printf("Error while creating socket!\n");
    exit(1);
  }
   
  memset(&server_address, '0', address_length);
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(port);
  
  if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0) {
    printf("Invalid address!\n");
    exit(1);
  }

  if (connect(sock, (struct sockaddr *) &server_address, address_length) < 0) {
    printf("Error while connecting!\n");
    exit(1);
  }
  
  //POLLING
  struct pollfd fds[2];
  fds[0].fd = 0;
  fds[1].fd = sock;
  fds[0].events = POLLIN | POLLHUP | POLLERR;
  fds[1].events = POLLIN | POLLHUP | POLLERR;

  while (1) {
    poll(fds, 2, 0);
    if (fds[0].revents & POLLERR) {
      shutdown(sock, 2);
      deflateEnd(&out_stream);
      inflateEnd(&in_stream);
      exit(1);
    }

    if (fds[1].revents & POLLERR || fds[1].revents & POLLHUP) {
      shutdown(sock, 2);
      deflateEnd(&out_stream);
      inflateEnd(&in_stream);
      exit(0);
    }

    if (fds[0].revents & POLLIN) {
      char* buf = (char*)malloc(8*sizeof(char));
      int bytes = read(0, buf, 8);
      for (int i = 0; i < bytes; i++) {
	if (buf[i] == 10) {
	  write(1, &cr, 1);
	}
	write(1, &buf[i], 1);
	if (buf[i] == 13) {
	  write(1, &lf, 1);
	}
	if (compress == 1) {
	  char* outBuf = (char*)malloc(16*sizeof(char));
	  out_stream.next_in = (Bytef *)(buf + i);
	  out_stream.avail_in = 1;
	  out_stream.next_out = (Bytef *)outBuf;
	  out_stream.avail_out = 16;
	  while (out_stream.avail_in != 0) {
	    int res = deflate(&out_stream, Z_SYNC_FLUSH);
	    assert(res == Z_OK);
	  }
	  write(sock, outBuf, 16-out_stream.avail_out);
	  if (lflag == 1) {
	    char sent_bytes[16];
	    sprintf(sent_bytes, "%d", 16-out_stream.avail_out);
	    write(ifd, "SENT ", 5);
	    write(ifd, sent_bytes, strlen(sent_bytes));
	    write(ifd, " bytes: ", 8);
	    write(ifd, outBuf, 16-out_stream.avail_out);
	    write(ifd, "\n", 1);
	  }
	  free(outBuf);
	}
	else {
	  write(sock, &buf[i], 1);
	  if (lflag == 1) {
	    write(ifd, "SENT 1 bytes: ", 14);
	    write(ifd, &buf[i], 1);
	    write(ifd, "\n", 1);
	  }
	}
      }
      free(buf);
    }
    
    if (fds[1].revents & POLLIN) {
      char* buf = (char*)malloc(1024*sizeof(char));
      int bytes = read(sock, buf, 1024);
      if (bytes == 0) {
	shutdown(sock, 2);
	deflateEnd(&out_stream);
	inflateEnd(&in_stream);
	exit(0);
      }
      if (compress == 1) {
	char* uncompressed = (char*)malloc(512*sizeof(char));
	in_stream.next_in = (Bytef *)buf;
	in_stream.avail_in = bytes;
	in_stream.next_out = (Bytef *)uncompressed;
	in_stream.avail_out = 512;
	while (in_stream.avail_in != 0) {
	  int res = inflate(&in_stream, Z_SYNC_FLUSH);
	  assert(res == Z_OK);
	}
	write(1, uncompressed, 512-in_stream.avail_out);
	free(uncompressed);
      }
      else {
	write(1, buf, bytes);
      }
      if (lflag == 1) {
	char read_bytes[10];
	sprintf(read_bytes, "%d", bytes);
	write(ifd, "RECEIVED ", 9);
	write(ifd, read_bytes, strlen(read_bytes));
	write(ifd, " bytes: ", 8);
	write(ifd, buf, bytes);
	write(ifd, "\n", 1);
      }
      free(buf);
    }

  }
  deflateEnd(&out_stream);
  inflateEnd(&in_stream);
  exit(0);
}
