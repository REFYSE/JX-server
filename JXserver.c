#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>

#include "JXserver.h"
#include "binio.h"
#include "threadpool.h"
#include "performance-parameters.h"

#define DICT_SRC ("compression.dict")

#define SID_LEN (4)
#define OFFSET_L (8)
#define FR_LEN (8)

#define ADDR_LEN (4)
#define PORT_LEN (2)

#define HEADER_LEN (1)
#define TYPE_NBITS (4)
#define PADDING_NBITS (2)
#define PAYLOADL_LEN (8)
#define TOTAL_L (HEADER_LEN + PAYLOADL_LEN)

#define DICT_NUM_ENTRY (256)
#define BUFFER_SIZE (4096)
#define MAX_EVENTS (1000)
#define THREAD_EVENTS (MAX_EVENTS/CPUS)

#define ECHO (0x0)
#define ECHO_R (0x10)
#define DIR_LIST (0x2)
#define DIR_LIST_R (0x30)
#define SIZE_QRY (0x4)
#define SIZE_QRY_R (0x50)
#define RET_FILE (0x6)
#define RET_FILE_R (0x70)
#define SHUTDOWN (0x8)
#define ERROR (0xf0)
#define COMP (0x08)


// Sets socket given by file descriptor fd to non-blocking
void set_socket_nb(int fd) {
    // Get existing flags for fd
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1) {
		perror("fcntl() failed");
		exit(1);
	}
    // Add non-blocking flag
	int ret = fcntl (fd, F_SETFL, flags | O_NONBLOCK);
	if(ret == -1) {
		perror("fcntl() failed");
		exit(1);
	}
}

// Given a filename, attempts to parse data into the given struct
void parse_config(char* filename, struct config_data* data) {
	// Check if config file exists
	struct stat sb;
	if(stat(filename, &sb) == -1) {
		perror("stat() failed");
		exit(1);
	}

	// Parse the config
    // Create space for the directory string, plus one for null terminator
	off_t len = (sb.st_size - ADDR_LEN - PORT_LEN) + 1;
	data->dir = malloc(sizeof(char) * len);
	if(data->dir == NULL) {
		perror("malloc() failed");
		exit(1);
	}
    // Read the data from the file into the struct
	FILE *file = fopen(filename, "rb");
    if(file == NULL) {
        perror("fopen() failed");
        exit(1);
    }
    if(fread(&(data->s_addr), 1, ADDR_LEN, file) != ADDR_LEN    ||
            fread(&(data->port), 1, PORT_LEN, file) != PORT_LEN ||
            fread(data->dir, sizeof(char), len - 1, file) != (unsigned)len -1) {
        perror("fread() failed");
        exit(1);
    }
    // Add the null terminator
	data->dir[len-1] = '\0';
	fclose(file);
}

// Sets up a server socket using the given data, returns a fd for the socket
int setup_serverfd(struct config_data* data) {
    // Create a socket to receive incoming connections
	int serversocket_fd = -1;
	serversocket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(serversocket_fd < 0) {
		perror("socket() failed");
		exit(1);
	}

	// Setup socket to be reusable
	int option = 1;
	int ret = setsockopt(serversocket_fd, SOL_SOCKET,
							SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(int));
 	if(ret < 0) {
		perror("setsockopt() failed");
		close(serversocket_fd);
		exit(1);
	}

	// Set socket to nonblocking
	set_socket_nb(serversocket_fd);

	// Bind socket
	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = data->s_addr;
	address.sin_port = data->port;
	if(bind(serversocket_fd, (struct sockaddr*) &address,
            sizeof(struct sockaddr_in))) {
		perror("bind() failed");
		exit(1);
	}

	// Set the socket to listen
	ret = listen(serversocket_fd, MAX_EVENTS);
	if(ret < 0) {
		perror("listen() failed");
		exit(1);
	}
    return serversocket_fd;
}

int main(int argc, char** argv)	{
    // Check if config file has been given
    if(argc != 2) {
        puts("Insufficient args");
        exit(1);
    }

    // Parse the config file
    struct config_data data;
    parse_config(argv[1], &data);

    // Setup server fd
    int serversocket_fd = setup_serverfd(&data);

    start_server(serversocket_fd, data.dir);
	free(data.dir);
}

void start_server(int serversocket_fd, char* dir) {
    // Setup the epollfd
    struct epoll_event ev;
    struct epoll_event events[THREAD_EVENTS];
    int epollfd = epoll_create1(0);
    if(epollfd == -1) {
        perror("epoll_create1() failed");
        exit(1);
    }

    // Add the server socket to the epoll
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = serversocket_fd;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, serversocket_fd, &ev) == -1) {
        perror("epoll_ctl() failed");
        exit(1);
    }

    int shutdown = 0; // Value to indicate server has shutdown

    // Create a threadpool with CPUS num of thread, as main thread will be...
    // ... mostly idle and only acception new events and handling threads
    struct threadpool* pool = init_threadpool(CPUS*2);

    // Create a shared data struct for threads
    struct handler_data* h_data = malloc(sizeof(struct handler_data));
    if(h_data == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    h_data->dir = dir;
    h_data->dict = NULL;
    h_data->root = NULL;
    pthread_mutex_init(&(h_data->lock), NULL);

    // Create data struct for shared file retrievals
    h_data->ret_data = malloc(sizeof(struct file_ret_data));
    if(h_data->ret_data == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    h_data->ret_data->n_req = 0;
    h_data->ret_data->requests = NULL;
    pthread_mutex_init(&(h_data->ret_data->lock), NULL);

    // Setup for accepting incoming connections
    int clientsocket_fd;
    int nfds; // Nnumber of fds with events

    // Loop waiting for client connections and incoming data
    while(!shutdown) {
        // Wait for incoming events on fds
        nfds = epoll_wait(epollfd, events, THREAD_EVENTS, 0);
        if(nfds == -1) {
            perror("epoll_wait() failed");
            exit(1);
        }

        // Loop through all events
        for(int i = 0; i < nfds; i ++) {
            // Incoming connections on server socket
            if(events[i].data.fd == serversocket_fd) {
                while(1) {
                    // Accept all connections
                    clientsocket_fd = accept(serversocket_fd, NULL, NULL);
                    if(clientsocket_fd == -1) {
                        // EWOULDBLOCK if no more connections
                        // Otherwise other problem
                        if(errno != EWOULDBLOCK) {
                          perror("accept() failed");
                          exit(1);
                        }
                        break;
                    }
                    // Set socket to nonblocking
                    set_socket_nb(clientsocket_fd);
                    // Add the new fd to epoll listening for EPOLLIN
                    // EPOLLET for edge triggered
                    // EPOLLONESHOT for thread-safety
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    ev.data.fd = clientsocket_fd;
                    if(epoll_ctl(epollfd, EPOLL_CTL_ADD,
                                clientsocket_fd, &ev) == -1) {
                        perror("epoll_ctl() failed");
                        exit(1);
                    }
                }
            } else {
                // Create a struct to give data to thread handler
                struct thread_data* t_data = malloc(sizeof(struct thread_data));
                t_data->h_data = h_data;
                t_data->epollfd = epollfd;
                t_data->clientfd = events[i].data.fd;
                t_data->shutdown = &shutdown;
                // Add task to queue
                threadpool_add(pool, thread_handler, (void*)t_data);
            }
        }
    }
    // Cleanup
    threadpool_cleanup(pool);
    cleanup(h_data);
}

void thread_handler(void* h_data) {
    struct thread_data* t_data = (struct thread_data*) h_data;
    // Incoming events on client socket
    // Handle the request
    int ret = handle(t_data->clientfd, t_data->h_data);
    // Check if close connection was flagged
    if(ret == -1) {
        close(t_data->clientfd);
    } else if(ret == -2) {
        // Check if shutdown was flagged
        *(t_data->shutdown) = 1;
    } else {
        // Otherwise rearm this fd and continue monitoring
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.fd = t_data->clientfd;
        epoll_ctl(t_data->epollfd, EPOLL_CTL_MOD, t_data->clientfd, &ev);
    }
    // Cleanup
    free(t_data);
}

int handle(int fd, struct handler_data* h_data) {
	if(fd < 0 || h_data == NULL) {
		return -1;
	}
	// Read the header and payload length
	uint8_t buffer[TOTAL_L];
	int ret = recv(fd, buffer, sizeof(buffer), 0);
	if(ret < 0) {
        if(errno == ECONNRESET) {
            return 0;
        }
		perror("recv() failed");
		exit(1);
	}
	// Check if client closed connection
	if(ret == 0) {
		return 0;
	}
	// Check if TOTAL_L bytes were received
	if(ret != sizeof(buffer)) {
		send_error(fd);
		return -1;
	}

	uint8_t header = buffer[0];
	uint8_t type = read_b(header, 8-TYPE_NBITS, TYPE_NBITS);
	uint8_t comp = read_b(header, 8-TYPE_NBITS-1, 1);
	uint8_t r_comp = read_b(header, 8-TYPE_NBITS-1-1, 1);
	uint8_t padding = read_b(header, 0, PADDING_NBITS);

	// Check if padding is correct
	if(padding != 0) {
		send_error(fd);
		return -1;
	}

	//Convert 8 bytes in network byte order to an 8 byte long
	unsigned long len = 0;
	for(int i = 1; i < TOTAL_L; i++) {
		len = buffer[i] | (len << 8);
	}
	// Handle the request based on type
	switch(type) {
		case ECHO:
			return send_echo(fd, comp, r_comp, len, h_data);
		case DIR_LIST:
			return send_dir(fd, r_comp, len, h_data);
		case SIZE_QRY:
			return send_file_query(fd, comp, r_comp, len, h_data);
		case RET_FILE:
			return send_file_ret(fd, comp, r_comp, len, h_data);
		case SHUTDOWN:
			return -2; // Return value to notify shutdown
		default:
			return send_error(fd);
	}
}

int send_file_ret(int fd, unsigned int comp, unsigned int r_comp, long len,
                    struct handler_data* h_data) {
    struct file_ret_data* ret_data = h_data->ret_data;
    char* dir = h_data->dir;
    // Read payload into buffer
    uint8_t* buffer = malloc(sizeof(uint8_t) * len);
    if(buffer == NULL) {
        perror("malloc() failed");
        exit(1);
    }

    while(1) {
        if(recv(fd, buffer, len, 0) == len) {
            break;
        }
    }

    // Decompress payload if required
    uint8_t* payload = buffer;
    unsigned long n_len;
    if(comp) {
        payload = decompress(buffer, len, &n_len, h_data);
        len = (signed) n_len;
        free(buffer);
        buffer = payload;
    }

    // Get the session ID, starting offset and length
    uint32_t sessionID = 0;
    uint64_t start = 0;
    uint64_t length = 0;
    for(int i = 0; i < SID_LEN; i++) {
        sessionID = buffer[i] | (sessionID << 8);
    }
    for(int i = SID_LEN; i < SID_LEN + OFFSET_L; i++) {
        start = buffer[i] | (start << 8);
    }
    for(int i = SID_LEN + OFFSET_L; i < SID_LEN + OFFSET_L + FR_LEN; i++) {
        length = buffer[i] | (length << 8);
    }

    // Get the filename to retrieve
    long f_offset = sizeof(sessionID) + sizeof(start) + sizeof(length);
    char* filename = malloc(sizeof(char)* (len - f_offset));
    if(filename == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    strcpy(filename, (char*)(&buffer[f_offset]));
    free(buffer);

    // Check if the file exists
    char* strings[3] = {dir, "/", filename};
    char* path = string_concat(strings, 3);
    struct stat st;

    if(stat(path, &st) == -1) {
        // File does not exist
        send_error(fd);
        free(filename);
        free(path);
        return -1;
    }

    // Check size of file
    if(start + length > (unsigned) st.st_size || start + length == 0) {
        // Offset and length too big or too small
        send_error(fd);
        free(filename);
        free(path);
        return -1;
    }

    // Lock request data before reading
    pthread_mutex_lock(&(ret_data->lock));
    // Check if a file retrieval request for this session and file exists
    int found = 0;
    int idx = -1;
    // If there are existing requests
    if(ret_data->requests != NULL) {
        for(int i = 0; i < ret_data->n_req; i++) {
            // If same sessionID
            if(ret_data->requests[i].sessionID == sessionID) {
                // Check if requests matches
                if(strcmp(ret_data->requests[i].filename, filename) == 0 &&
                        ret_data->requests[i].start == start &&
                        ret_data->requests[i].len == length) {
                    // Same request, check if completed
                    if(ret_data->requests[i].sent == length ) {
                        // Completed, send empty payload
                        uint8_t tmp[TOTAL_L];
                        tmp[0] = RET_FILE_R;
                        for(int j = 0; j < PAYLOADL_LEN; j++) {
                            tmp[HEADER_LEN + i] = 0;
                        }
                        if(send(fd, tmp, TOTAL_L, 0) == -1) {
                            perror("send() failed 1");
                            exit(1);
                        }
                        free(filename);
                        free(path);
                        pthread_mutex_unlock(&(ret_data->lock));
                        return 0;
                    } else {
                        // Not completed, can continue with this request
                        found = 1;
                        idx = i;
                        break;
                    }
                } else {
                    // Different request reusing same ID
                    // Check if previous request with same ID has finished
                    if(ret_data->requests[i].sent != length ) {
                        // Not finished, cannot reuse the ID yet
                        send_error(fd);
                        free(filename);
                        free(path);
                        pthread_mutex_unlock(&(ret_data->lock));
                        return -1;
                    }
                }
            }
        }
    }

    // Create a new entry if not found
    if(!found) {
        struct file_req* tmp_requests = realloc(ret_data->requests,
                            sizeof(struct file_req) * (ret_data->n_req + 1));
        if(tmp_requests == NULL) {
            exit(1);
        }
        ret_data->requests = tmp_requests;
        ret_data->requests[ret_data->n_req].sessionID = sessionID;
        ret_data->requests[ret_data->n_req].start = start;
        ret_data->requests[ret_data->n_req].len = length;
        ret_data->requests[ret_data->n_req].sent = 0;
        // Copy the filename
        ret_data->requests[ret_data->n_req].filename = malloc(sizeof(char) *
                                                    (strlen(filename) + 1));
        if(ret_data->requests[ret_data->n_req].filename == NULL) {
            perror("malloc failed()");
            exit(1);
        }
        strcpy(ret_data->requests[ret_data->n_req].filename, filename);
        idx = ret_data->n_req;
        ret_data->n_req++;
    }

    // Keep sending as long as file hasn't been completely sent yet
    int remaining;
    int to_send;
    int send_start;
    while(1) {
        remaining = length - ret_data->requests[idx].sent;

        // Determine amount of bytes to send
        if(remaining >= BUFFER_SIZE - SID_LEN - OFFSET_L - FR_LEN) {
            to_send = BUFFER_SIZE - SID_LEN - OFFSET_L - FR_LEN;
        } else {
            to_send = remaining;
        }
        // Update the request entry
        send_start = start + ret_data->requests[idx].sent;
        ret_data->requests[idx].sent += to_send;
        // Unlock
        pthread_mutex_unlock(&(ret_data->lock));
        // Send the chunk
        send_file(fd, path, r_comp, sessionID, send_start, to_send, h_data);
        // Lock again before reading
        pthread_mutex_lock(&(ret_data->lock));

        // Check if finished sending
        if(ret_data->requests[idx].sent == length) {
            free(filename);
            free(path);
            pthread_mutex_unlock(&(ret_data->lock));
            return 0;
        }
    }
}

void send_file(int fd, char* path, unsigned int r_comp, uint32_t sid,
                    uint64_t start, uint64_t len, struct handler_data* h_data) {
    // Block on writes to make sure all data gets sent through when...
    /// ... multi-plexing
    int opts = fcntl(fd,F_GETFL);
    opts = opts & (~O_NONBLOCK);
    fcntl(fd,F_SETFL,opts);

    // Optimisation
    if(COMPRESSION_RATIO >= 1.4) {
        r_comp = 1;
    }
    // Send response header
    uint8_t header;
    if(r_comp) {
        header = RET_FILE_R + COMP; // Corresponding to 0b0111 1000
    } else {
        header = RET_FILE_R; // Corresponding to 0b0111 0000
    }
    send(fd, &header, HEADER_LEN, 0);

    uint8_t buffer[BUFFER_SIZE];
    // Add the file retrieval header to the buffer
    for(int i = 0; i < SID_LEN; i++) {
        buffer[i] = sid >> ((SID_LEN-i-1)*8) & 0xff;
    }
    for(int i = 0; i < OFFSET_L; i++) {
        buffer[SID_LEN + i] = start >> ((OFFSET_L-i-1)*8) & 0xff;
    }
    for(int i = 0; i < FR_LEN; i++) {
        buffer[SID_LEN + OFFSET_L + i] = len >> ((FR_LEN-i-1)*8) & 0xff;
    }

    // Open file and go to start
    FILE* file = fopen(path, "rb");
    if(fseek(file, start, SEEK_SET) != 0){
        perror("fseek() failed");
        exit(1);
    }

    int fr_header_len = SID_LEN + OFFSET_L + FR_LEN;
    // Read required bytes into buffer
    if(fread(buffer + fr_header_len, 1, len, file) != len){
        perror("fread() failed");
        exit(1);
    }
    unsigned long n_len = fr_header_len + len;
    uint8_t* payload;
    // Check if compression needed
    if(r_comp) {
        payload = compress(buffer, fr_header_len + len, &n_len, h_data);
    }
    // Send the length component of header
    uint8_t byte[PAYLOADL_LEN];
    for(int i = 0; i < PAYLOADL_LEN; i++) {
        byte[i] = (n_len >> (PAYLOADL_LEN -i -1)*8) & 0xff;
    }

    if(send(fd, &byte, PAYLOADL_LEN, 0) != PAYLOADL_LEN) {
        perror("send() failed");
        exit(1);
    }

    // Send payload
    if(r_comp) {
        if(send(fd, payload, n_len, 0)<= 0) {
            perror("send() failed");
            exit(1);
        }
        free(payload);
    } else {
        if(send(fd, buffer, n_len, 0) <= 0) {
            perror("send() failed");
            exit(1);
        }
    }
    fclose(file);
    // Set back to non-blocking
    set_socket_nb(fd);
}

int send_file_query(int fd, unsigned int comp, unsigned int r_comp,
                    unsigned long length, struct handler_data* h_data) {
    char* dir = h_data->dir;

    // Retrieve the filename
    char* tmp_fname = malloc(sizeof(char) * length);
    if(tmp_fname == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    int ret = recv(fd, tmp_fname, length, 0);
    if(ret < 0) {
        perror("recv() failed");
        exit(1);
    }

    // If compressed, decompress
    char* filename = tmp_fname;
    unsigned long n_len;
    if(comp) {
        filename = (char*) decompress((uint8_t*) tmp_fname, length,
                                        &n_len, h_data);
        length = n_len;
        free(tmp_fname);
    }

	// Get the path to the file
	char* strings[] = {dir, "/",filename};
	char* path = string_concat(strings, 3);
	if(path == NULL) {
		return -1;
	}
    free(filename);

	// Find its size
	struct stat st;
	ret = stat(path, &st);
	// Does not exist
	if(ret == -1) {
		return send_error(fd);
	}

	// Copy the size into array
	uint8_t* size = malloc(sizeof(unsigned long));
	if(size == NULL) {
		perror("malloc() failed");
		exit(1);
	}
	unsigned long f_size = st.st_size;

	for(size_t i = 0; i < sizeof(unsigned long); i++) {
		size[i] = f_size >> ((8-i-1)*8) & 0xff;
	}
	free(path);
	// Compression
	uint8_t* payload = size;
	unsigned long len = sizeof(unsigned long);
	if(r_comp) {
		payload = compress(size, 8, &len, h_data);
	}
	// Build the response
	uint8_t* response = malloc(sizeof(uint8_t) * (TOTAL_L+len));
	if(response == NULL) {
		perror("malloc() failed");
		exit(1);
	}
	// Build the first byte containing: type, compression, and 3 bits padding
	if(r_comp) {
		response[0] = SIZE_QRY_R + COMP; // Corresponding to 0b01011000
	} else {
		response[0] = SIZE_QRY_R; // Corresponding to 0b01010000
	}
	// Convert the 8 byte long back to 8 hex bytes
	for(int i = 1; i < TOTAL_L; i++) {
		response[i] = len >> ((8-i)*8) & 0xff;
	}
	// Add the payload to the response
	for(unsigned long i = TOTAL_L; i < TOTAL_L+len; i++) {
		response[i] = payload[i-TOTAL_L];
	}
	// Send the response
	ret = send(fd, response, TOTAL_L + len, 0);
    if(ret == -1) {
        perror("send() failed");
    }
	free(response);
	free(size);
	if(r_comp) {
		free(payload);
	}
	return ret;
}

int send_dir(int fd, unsigned int r_comp, long length,
                    struct handler_data* h_data) {
    char* dir = h_data->dir;
    // Length of payload should be 0
    if(length != 0) {
        return send_error(fd);
    }
	// Open the directory
	DIR *d;
	struct dirent *dir_ent;
	d = opendir(dir);
	if(d == NULL) {
		perror("opendir() failed");
		exit(1);
	}
	// Find the length of all the names joined together
	size_t msg_len = 0;
	dir_ent = readdir(d);
	// While more entries in dir
	while(dir_ent != NULL) {
		// If it is a regular file, increment msg_len by its length
		if (dir_ent->d_type == DT_REG) {
			msg_len += strlen(dir_ent->d_name) + 1; // One for null terminator
		}
		dir_ent = readdir(d);
	}

	char* filenames;
	// If no files were found, payload is a single NULL byte
	if(msg_len == 0) {
		filenames = malloc(sizeof(char));
		if(filenames == NULL) {
			perror("malloc() failed");
			exit(1);
		}
		filenames[0] = 0;
		msg_len = 1;
	} else {
		// Malloc space for all the file names
		filenames = malloc(sizeof(char) * msg_len);
		if(filenames == NULL) {
			perror("malloc() failed");
			exit(1);
		}
		// Loop through again and copy the file names to the array
		rewinddir(d);
		size_t len = 0;
		while ((dir_ent = readdir(d)) != NULL) {
			if (dir_ent->d_type == DT_REG) {
				sprintf(filenames + len, "%s", dir_ent->d_name);
				len += strlen(dir_ent->d_name) + 1;
			}
		}
	}
	closedir(d);
	uint8_t* payload = (uint8_t*)filenames;
	unsigned long len = msg_len;
	// Compress if required and at least 1 file found
	if(r_comp && msg_len != 1) {
		payload = compress((uint8_t*)filenames, msg_len, &len, h_data);
	}
	// Build the response
	uint8_t* response = malloc(sizeof(uint8_t) * (TOTAL_L+len));
	if(response == NULL) {
		perror("malloc() failed");
		exit(1);
	}
	// Build the first byte containing: type, compression, and 3 bits padding
	if(r_comp) {
		response[0] = DIR_LIST_R + COMP; // Corresponding to 0b00111000
	} else {
		response[0] = DIR_LIST_R; // Corresponding to 0b00110000
	}
	// Convert the 8 byte long back to 8 hex bytes
	for(int i = 1; i < TOTAL_L; i++) {
		response[i] = len >> ((8-i)*8) & 0xff;
	}
	// Add the payload to the response
	for(unsigned long i = TOTAL_L; i < TOTAL_L+len; i++) {
		response[i] = payload[i-TOTAL_L];
	}
	// Send the response
	int ret = send(fd, response, TOTAL_L + len, 0);
    if(ret == -1) {
        perror("send() failed");
    }
	free(response);
	free(filenames);
	if(r_comp && msg_len != 1) {
		free(payload);
	}
	return ret;
}

int send_error(int fd) {
	uint8_t response[TOTAL_L];
	memset(response, 0, sizeof(response));
	response[0] = ERROR;
	int ret = send(fd, response, sizeof(response), 0);
	if(ret < 0) {
	  perror("send() failed");
      exit(1);
	}
	return -1;
}

int send_echo(int fd, unsigned int comp, unsigned int r_comp, long len,
                    struct handler_data* h_data) {
    // If requires compression
    if(!comp && r_comp) {
        return send_compressed_echo(fd, len, h_data);
    }
	// Build the first byte containing: type, compression, and 3 bits padding
    uint8_t header[HEADER_LEN];
	if(comp) {
		header[0] = ECHO_R + COMP; // Corresponding to 0b00011000
	} else {
		header[0] = ECHO_R; // Corresponding to 0b00010000
	}
    // Send the header
    int ret = send(fd, header, HEADER_LEN, 0);
    if(ret < 0) {
	  perror("send() failed");
	  exit(1);
	}

    // Convert the 8 byte long back to 8 hex bytes
    uint8_t length[PAYLOADL_LEN];
	for(int i = 0; i < PAYLOADL_LEN; i++) {
		length[i] = len >> ((8-i-1)*8) & 0xff;
	}
    // Send the length
    ret = send(fd, length, PAYLOADL_LEN, 0);
    if(ret < 0) {
      perror("send() failed");
      exit(1);
    }

    uint8_t buffer[BUFFER_SIZE];
    int sent = 0;
    int n_recv = 0;
    while(sent != len) {
        n_recv = recv(fd, buffer, BUFFER_SIZE, 0);
        if(n_recv > 0) {
            if(send(fd, buffer, n_recv, 0) != n_recv) {
                perror("send() failed");
            }
            sent += n_recv;
        } else if (n_recv == 0) {
            return -1;
        }
    }
	return 0;
}

int send_compressed_echo(int fd, long len, struct handler_data* h_data) {
    // Build the first byte containing: type, compression, and 3 bits padding
    uint8_t header[HEADER_LEN];
    header[0] = 0x18; // Corresponding to 0b00011000
    // Send the header
    int ret = send(fd, header, HEADER_LEN, 0);
    if(ret < 0) {
      perror("send() failed");
      exit(1);
    }
    // Read the payload if any
	uint8_t* payload;
	payload = malloc(sizeof(uint8_t) * len);
	if(payload == NULL) {
		perror("malloc() failed");
		exit(1);
	}
	ret = recv(fd, payload, len, 0);
	// If error in recv, send error to fd
	while((unsigned)ret != len) {
		ret = recv(fd, payload, len, 0);
		// If error in recv, send error to fd
		if(ret == -1) {
			if(errno != EAGAIN) {
				perror("recv() failed");
				send_error(fd);
				return -1;
			}
		}
	}
    // Compress the payload
    unsigned long n_len;
    uint8_t* n_payload = compress(payload, len, &n_len, h_data);

    // Convert the 8 byte long back to 8 hex bytes
    uint8_t length[PAYLOADL_LEN];
	for(int i = 0; i < PAYLOADL_LEN; i++) {
		length[i] = n_len >> ((8-i-1)*8) & 0xff;
	}
    // Send the length
    ret = send(fd, length, PAYLOADL_LEN, 0);
    if(ret < 0) {
      perror("send() failed");
      exit(1);
    }
    // Send the payload
    ret = send(fd, n_payload, n_len, 0);
    if(ret < 0) {
      perror("send() failed");
      exit(1);
    }
    free(payload);
    free(n_payload);
    return 0;
}

// Given a payload of size len, compresses it using dict in h_data
// Returns an array of bytes of size n_len
uint8_t* compress(uint8_t* payload, unsigned long len, unsigned long* n_len,
                    struct handler_data* h_data) {
    // If pointer to dictionary is NULL, create it
    // Lock to avoid creating multiple times
    pthread_mutex_lock(&(h_data->lock));
    if(h_data->dict == NULL) {
        h_data->dict = get_dict();
    }
    pthread_mutex_unlock(&(h_data->lock));
    struct dict_entry* dict = h_data->dict;

	uint8_t* compressed;
	uint8_t next_byte;
	unsigned long size = 0;
	// Calculate the length of the compressed payload
	for(unsigned long i = 0; i < len; i++) {
		next_byte = payload[i];
		size = size + dict[next_byte].len;
	}
    // Round up to nearest byte
	size = size % 8 == 0 ? size/8 : size/8 + 1;
	size += 1; // Compressed payloads have a byte identifying num of padding

    // Create space for the compressed payload
	compressed = malloc(sizeof(uint8_t) * size);
	if(compressed == NULL) {
		perror("malloc failed()");
		exit(1);
	}
	memset(compressed, 0, size);

	// Loop through the uncompressed payload and write the code for each byte
	size_t index = 0;
	for(unsigned long i = 0; i < len; i++) {
		next_byte = payload[i];
		write_b(compressed, index, dict[next_byte].len, dict[next_byte].code);
		index += dict[next_byte].len;
	}

	// Add the amount of padding
	compressed[size-1] = (uint8_t) ((8-index %8)%8);
	*n_len = size;
	return compressed;
}

// Given a payload of size len, decompresses it using tree given by h_data
// Returns an array of bytes of size n_len
uint8_t* decompress(uint8_t* payload, unsigned long len, unsigned long* n_len,
                    struct handler_data* h_data) {
    // If pointer points to null, create the tree
    // Lock to avoid creating multiple times
    pthread_mutex_lock(&(h_data->lock));
    if(h_data->root == NULL) {
        if(h_data->dict == NULL) {
            h_data->dict = get_dict();
        }
        h_data->root = get_tree(h_data->dict);
    }
    struct node* root = h_data->root;
    pthread_mutex_unlock(&(h_data->lock));

	uint8_t* decompressed = NULL;
	uint8_t padding = payload[len-1]; // Last byte represnts num of padding
	int max = (len - 1) * 8 - padding; // Number of bits in payload
	int cur = 0;
	unsigned long size = 0;
	uint8_t n_bit;
	uint8_t cur_byte;
	struct node* cur_node = root;
	// Loop through the payload and look for bytes matching the bitcodes
	while(cur <= max) {
		cur_byte = payload[(int)(cur/8)];
		n_bit = read_b(cur_byte, (7-(cur%8)), 1); // Read next bit
		// Branch left or right in the decoding tree depending on bit
		// If 1, branch left
		if(n_bit) {
			cur_node = cur_node->l_child;
		} else {
			cur_node = cur_node->r_child;
		}
		// Check if reached a leaf node;
		if(cur_node->l_child == NULL && cur_node->r_child == NULL) {
			// Leaf node means found reached a byte based on bit code
			size++;
			decompressed = realloc(decompressed, sizeof(uint8_t) * size);
			if(decompressed == NULL) {
				perror("realloc() failed");
				exit(1);
			}
			// Add the decompressed byte to the new payload
			decompressed[size-1] = cur_node->byte;
			// Start again from the root
			cur_node = root;
		}
		cur++;
	}
	*n_len = size;
	return decompressed;
}

// Builds and returns a compression dictionary from file give by DICT_SRC
struct dict_entry* get_dict() {
	struct dict_entry* dict;

	// Allocate memory for the dictionary
	dict = malloc(sizeof(struct dict_entry) * DICT_NUM_ENTRY);
	if(dict == NULL) {
		perror("malloc() failed");
		exit(1);
	}
	// Open the file
	FILE* file = fopen(DICT_SRC, "rb");
	if(file == NULL) {
		perror("fopen() failed");
		exit(1);
	}
	// Build the dictionary'
	uint8_t len;
	for(int i = 0; i < DICT_NUM_ENTRY; i++) {
        // Set the length of the code
		if(fread_b(&len, 8, file) != 8) {
			perror("fread_b() failed");
			exit(1);
		}
		dict[i].len = len;
		// Allocate memory for the code, round up to next full byte
		int size = len % 8 == 0 ? len/8 : len/8 + 1;
		dict[i].code = malloc(sizeof(uint8_t) * size);
		if(dict[i].code == NULL) {
			perror("malloc() failed");
			exit(1);
		}
		// Read len bits into the code
		if(fread_b(dict[i].code, len, file) != len) {
			perror("fread_b() failed");
			exit(1);
		}
	}
	fclose(file);

	return dict;
}

// Helper function for initialising a node
struct node* init_node() {
	struct node* n = malloc(sizeof(struct node));
	if(n == NULL) {
		perror("malloc() failed");
		exit(1);
	}
	n->l_child = NULL;
	n->r_child = NULL;
	return n;
}

// Builds a decompression (HUFFMAN) tree given a compression dictionary
struct node* get_tree(struct dict_entry* dict) {
	struct node* root;
    // Build the tree
	root = init_node();
	struct node* cur = root;

	// For each dictionary entry
	for(int i =  0; i < DICT_NUM_ENTRY; i++) {
		// Store a node in the tree according to its code
		for(int j = 0; j < dict[i].len; j++) {
            // Read at byte rounded down (j/8) with index 7-(j%8)
			uint8_t next_b = read_b(dict[i].code[(int)(j/8)], (7-(j%8)), 1);
			// If the next bit is 1, branch left
			if(next_b == 1) {
				if(cur->l_child == NULL) {
					cur->l_child = init_node();
				}
				cur = cur->l_child;
			// The next bit is 0, branch right
			} else {
				if(cur->r_child == NULL) {
					cur->r_child = init_node();
				}
				cur = cur->r_child;
			}
		}
		cur->byte = i; // Byte for this code is whatever index up to
		cur = root; // Start again from the root
	}
	return root;
}

// Given an array of n_strings strings, returns a concatenated string
char* string_concat(char** strings, int n_strings) {
    // Find the size of the concatenated string
	size_t len = 1; // For null terminator
	for(int i = 0; i < n_strings; i++) {
		len += strlen(strings[i]); // Does not include null terminator
	}
    // Create space for new string
	char* concat_str = malloc(sizeof(char) * len);
	if(concat_str == NULL) {
		perror("malloc() failed");
		exit(1);
	}
    // Copy each string over with the offset
	len = 0;
	for(int i = 0; i < n_strings; i++) {
		sprintf(concat_str + len, "%s", strings[i]);
		len += strlen(strings[i]);
	}
	concat_str[len] = '\0'; // Add null terminator
	return concat_str;
}

void cleanup(struct handler_data* data) {
    // Cleanup thread data struct
    pthread_mutex_lock(&(data->ret_data->lock));
    pthread_mutex_destroy(&(data->ret_data->lock));
    // Free requests
    if(data->ret_data->n_req != 0) {
        for(int i = 0; i < data->ret_data->n_req; i++) {
            free(data->ret_data->requests[i].filename);
        }
        free(data->ret_data->requests);
    }
    free(data->ret_data);

    // Cleanup the compression dictionary
    pthread_mutex_lock(&(data->lock));
    pthread_mutex_destroy(&(data->lock));
	struct dict_entry* dict = data->dict;
	if(dict != NULL) {
		for(int i = 0; i < DICT_NUM_ENTRY; i++) {
			free(dict[i].code);
		}
		free(dict);
	}

    // Cleanup the decompression tree
	struct node* root = data->root;
	if(root != NULL) {
		free_tree(root);
	}
    free(data);
}

void free_tree(struct node* root) {
    // Create a tree queue
    struct t_queue* q = init_tqueue();
    enqueue_t(q, root);
    struct node* cur;
    while(q->head != NULL) {
        cur = dequeue_t(q);
        if(cur->l_child != NULL) {
            enqueue_t(q, cur->l_child);
        }
        if(cur->r_child != NULL) {
            enqueue_t(q, cur->r_child);
        }
        free(cur);
    }
    free(q);
}

struct t_queue* init_tqueue() {
    struct t_queue* q = malloc(sizeof(struct t_queue));
    if(q == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    q->head = NULL;
    q->tail = NULL;
    return q;
}

struct node* dequeue_t(struct t_queue* q) {
    if(q == NULL || q->head == NULL) {
        return NULL;
    }

    struct node* node = q->head->node;
    struct q_node* qnode = q->head;
    // Case 1
    if(q->head == q->tail) {
        free(qnode);
        q->head = NULL;
        q->tail = NULL;
    } else {
        q->head = q->head->next;
        free(qnode);
    }
    return node;
}
void enqueue_t(struct t_queue* q, struct node* node) {
    if(q == NULL || node == NULL) {
        return;
    }
    struct q_node* qnode = malloc(sizeof(struct q_node));
    if(qnode == NULL) {
        perror("malloc() failed");
        exit(1);
    }
    qnode->node = node;
    qnode->next = NULL;
    // Case 0
    if(q->head == NULL) {
        q->head = qnode;
        q->tail = qnode;
    } else {
        q->tail->next = qnode;
        q->tail = qnode;
    }

}
