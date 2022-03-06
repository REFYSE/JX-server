#ifndef JXSERVER_H
#define JXSERVER_H
#include <stdint.h>
#include <pthread.h>

struct thread_data {
    struct handler_data* h_data;
    int epollfd;
    int clientfd;
    int* shutdown;
};

struct file_req {
    uint32_t sessionID;
    uint64_t start;
    uint64_t len;
    uint64_t sent;
    char* filename;
};

struct file_ret_data {
    struct file_req* requests;
    int n_req;
    pthread_mutex_t lock;
};

struct handler_data{
    char* dir;
    struct file_ret_data* ret_data;
    struct dict_entry* dict;
    struct node* root;
    pthread_mutex_t lock;
};

struct config_data {
    uint32_t s_addr;
    uint16_t port;
    char* dir;
};

struct node {
    struct node* l_child;
    struct node* r_child;
    uint8_t byte;
};

struct dict_entry {
    uint8_t len;
    uint8_t* code;
};

struct q_node {
    struct node* node;
    struct q_node* next;
};

struct t_queue {
    struct q_node* head;
    struct q_node* tail;
};


// Setup functions
void set_socket_nb(int fd);
void parse_config(char* filename, struct config_data* data);
int setup_serverfd(struct config_data* data);

// Server functions
void start_server(int serversocket_fd, char* dir);
void thread_handler(void* h_data);
int handle(int fd, struct handler_data* h_data);


// Handler functions
int send_file_query(int fd, unsigned int comp, unsigned int r_comp,
                    unsigned long length, struct handler_data* h_data);
int send_dir(int fd, unsigned int r_comp, long length,
                struct handler_data* h_data);
int send_error(int fd);
int send_echo(int fd, unsigned int comp, unsigned int r_comp, long len,
                struct handler_data* h_data);
int send_compressed_echo(int fd, long len, struct handler_data* h_data);
int send_file_ret(int fd, unsigned int comp, unsigned int r_comp, long len,
                    struct handler_data* h_data) ;
void send_file(int fd, char* path, unsigned int r_comp, uint32_t sid,
                    uint64_t start, uint64_t len, struct handler_data* h_data);

// Helper functions
struct dict_entry* get_dict();
struct node* init_node();
struct node* get_tree(struct dict_entry* dict);
char* string_concat(char** strings, int n_strings);
uint8_t* compress(uint8_t* payload, unsigned long len, unsigned long* n_len,
                    struct handler_data* h_data);
uint8_t* decompress(uint8_t* payload, unsigned long len, unsigned long* n_len,
                    struct handler_data* h_data);
void cleanup(struct handler_data* data);
void free_tree(struct node* n);

// Queue functions
struct t_queue* init_tqueue();
struct node* dequeue_t(struct t_queue* q);
void enqueue_t(struct t_queue* q, struct node* node);

#endif
