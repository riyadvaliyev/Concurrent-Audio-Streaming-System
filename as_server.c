#include "as_server.h"


int init_server_addr(int port, struct sockaddr_in *addr){
    // Allow sockets across machines.
    addr->sin_family = AF_INET;
    // The port the process will listen on.
    addr->sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr->sin_zero), 0, 8);

    // Listen on all network interfaces.
    addr->sin_addr.s_addr = INADDR_ANY;

    return 0;
}


int set_up_server_socket(const struct sockaddr_in *server_options, int num_queue) {
    int soc = socket(AF_INET, SOCK_STREAM, 0);
    if (soc < 0) {
        perror("socket");
        exit(1);
    }

    printf("Listen socket created\n");

    // Make sure we can reuse the port immediately after the
    // server terminates. Avoids the "address in use" error
    int on = 1;
    int status = setsockopt(soc, SOL_SOCKET, SO_REUSEADDR,
                            (const char *) &on, sizeof(on));
    if (status < 0) {
        perror("setsockopt");
        exit(1);
    }

    // Associate the process with the address and a port
    if (bind(soc, (struct sockaddr *)server_options, sizeof(*server_options)) < 0) {
        // bind failed; could be because port is in use.
        perror("bind");
        exit(1);
    }

    printf("Socket bound to port %d\n", ntohs(server_options->sin_port));

    // Set up a queue in the kernel to hold pending connections.
    if (listen(soc, num_queue) < 0) {
        // listen failed
        perror("listen");
        exit(1);
    }

    printf("Socket listening for connections\n");

    return soc;
}


ClientSocket accept_connection(int listenfd) {
    ClientSocket client;
    socklen_t addr_size = sizeof(client.addr);
    client.socket = accept(listenfd, (struct sockaddr *)&client.addr,
                               &addr_size);
    if (client.socket < 0) {
        perror("accept_connection: accept");
        exit(-1);
    }

    // print out a message that we got the connection
    printf("Server got a connection from %s, port %d\n",
           inet_ntoa(client.addr.sin_addr), ntohs(client.addr.sin_port));

    return client;
}

int count_digits(int num) {
    int count = 0;
    // Count digits
    do {
        num /= 10;
        count++;
    } while (num != 0);

    return count;
}

int list_request_response(const ClientSocket * client, const Library *library) {

    // Count the number of chars needed for the data string sent to the client
    int total_char_number = 0;
    for (int i = 0; i < library->num_files; i++) {
        total_char_number += count_digits(i); // For the index
        total_char_number += strlen(library->files[i]); // For the file name
        total_char_number += 3; // For the ':' and '\r\n'
    }

    // Declare the full list string and populate it
    char full_list[total_char_number];
    char *tracker = full_list; // Keep track of the current location @ full_list
    for (int i = (library->num_files - 1); i >= 0; i--) {
        // Save the values in the desired format
        sprintf(tracker, "%d:%s\r\n", i, library->files[i]);
        tracker += count_digits(i) + strlen(library->files[i]) + 3;
    }

    // Send data to the client and check if it was written appropriately
    if (write_precisely(client->socket, full_list, total_char_number) <
                                                            total_char_number) {
      ERR_PRINT("List failed to be sent from server to client.");
      perror("write_precisely");
      return -1;
    }

    return 0;
}


static int _load_file_size_into_buffer(FILE *file, uint8_t *buffer) {
    if (fseek(file, 0, SEEK_END) < 0) {
        ERR_PRINT("Error seeking to end of file\n");
        return -1;
    }
    uint32_t file_size = ftell(file);
    if (fseek(file, 0, SEEK_SET) < 0) {
        ERR_PRINT("Error seeking to start of file\n");
        return -1;
    }
    buffer[0] = (file_size >> 24) & 0xFF;
    buffer[1] = (file_size >> 16) & 0xFF;
    buffer[2] = (file_size >> 8) & 0xFF;
    buffer[3] = file_size & 0xFF;
    return 0;
}


int stream_request_response(const ClientSocket * client, const Library *library,
                            uint8_t *post_req, int num_pr_bytes) {

    // Error Check 1
    if (num_pr_bytes > 4) {
      ERR_PRINT("File index can't be more than 4 bytes.");
      return -1;
    }

    uint8_t index_array[4];
    // Read the first portion of the file_index stored in post_req
    for (int i = 0; i < num_pr_bytes; i++) {
      index_array[i] = post_req[i];
    }

    // Read the remaining portion of the file_index from the client
    for (int i = num_pr_bytes; i < 4; i++) {
      if (read_precisely(client->socket, index_array + i, 1) < 0) {
        ERR_PRINT("File index data failed to be read from client.")
        perror("read_precisely");
        return -1;
      }
    }

    // Converting array storing file index into host byte order integer
    // so the byte order is compatible with our machine
    int *ptr_int_index = (int *)index_array;
    int file_index = ntohl(*ptr_int_index);

    // Error Check 2
    if (file_index < 0 || file_index >= library->num_files) {
      ERR_PRINT("Invalid file index.");
      return -1;
    }

    char *path = _join_path(library->path, library->files[file_index]);
    if (path == NULL) {
      ERR_PRINT("Could not join paths together in server.");
      return -1;
    }

    // Opening the relevant path as a file descripter to read into buffer later
    FILE *fd;
    // Error Check 3
    if ((fd = fopen(path, "rb")) == NULL) {
      ERR_PRINT("Requested file does not exist.");
      free(path);
      return -1;
    }
    free(path);

    uint8_t buffer[STREAM_CHUNK_SIZE];
    if (_load_file_size_into_buffer(fd, buffer) < 0) {
      ERR_PRINT("File size data failed to be read into buffer.");
      fclose(fd);
      return -1;
    }
    if (write_precisely(client->socket, buffer, 4 * sizeof(uint8_t)) <
                                                       (4 * sizeof(uint8_t)) ) {
      ERR_PRINT("File size data failed to be sent to the client.");
      perror("write_precisely");
      fclose(fd);
      return -1;
    }

    // Write 1024-bit data chunks into buffer and send it to client from there
    size_t bytes_read;
    int bytes_written;
    while ( (bytes_read = fread(buffer, sizeof(uint8_t), STREAM_CHUNK_SIZE, fd))
                                                                         > 0 ) {
      bytes_written = write_precisely(client->socket, buffer, bytes_read);
      if (bytes_written < bytes_read) {
          ERR_PRINT("Data chunk failed to be sent to the client.");
          perror("write_precisely");
          fclose(fd);
          return -1;
      }
    }

    fclose(fd);
    return 0;
}


static Library make_library(const char *path){
    Library library;
    library.path = path;
    library.num_files = 0;
    library.files = NULL;
    library.name = "server";

    printf("Initializing library\n");
    printf("Library path: %s\n", library.path);

    return library;
}


static void _wait_for_children(pid_t **client_conn_pids, int *num_connected_clients, uint8_t immediate) {
    int status;
    for (int i = 0; i < *num_connected_clients; i++) {
        int options = immediate ? WNOHANG : 0;
        if (waitpid((*client_conn_pids)[i], &status, options) > 0) {
            if (WIFEXITED(status)) {
                printf("Client process %d terminated\n", (*client_conn_pids)[i]);
                if (WEXITSTATUS(status) != 0) {
                    fprintf(stderr, "Client process %d exited with status %d\n",
                            (*client_conn_pids)[i], WEXITSTATUS(status));
                }
            } else {
                fprintf(stderr, "Client process %d terminated abnormally\n",
                        (*client_conn_pids)[i]);
            }

            for (int j = i; j < *num_connected_clients - 1; j++) {
                (*client_conn_pids)[j] = (*client_conn_pids)[j + 1];
            }

            (*num_connected_clients)--;
            *client_conn_pids = (pid_t *)realloc(*client_conn_pids,
                                                 (*num_connected_clients)
                                                 * sizeof(pid_t));
        }
    }
}

/*
** Create a server socket and listen for connections
**
** port: the port number to listen on.
**
** On success, returns the file descriptor of the socket.
** On failure, return -1.
*/
static int initialize_server_socket(int port) {
	struct sockaddr_in addr;

  // Initialize a sockaddr_in structure for the server to listen on
  if (init_server_addr(port, &addr) < 0) {
    perror("initialize_server_socket");
    return -1;
  }

  // Create and set up a socket for this server process to listen on
  int listen_soc = set_up_server_socket(&addr, MAX_PENDING);
  if (listen_soc < 0) {
    perror("initialize_server_socket");
    return -1;
  }

	return listen_soc;
}

int run_server(int port, const char *library_directory){
    Library library = make_library(library_directory);
    if (scan_library(&library) < 0) {
        ERR_PRINT("Error scanning library\n");
        return -1;
    }

    int num_connected_clients = 0;
    pid_t *client_conn_pids = NULL;

	int incoming_connections = initialize_server_socket(port);
	if (incoming_connections == -1) {
		return -1;
	}

    int maxfd = incoming_connections;
    fd_set incoming;
    SET_SERVER_FD_SET(incoming, incoming_connections);
    int num_intervals_without_scan = 0;

    while(1) {
        if (num_intervals_without_scan >= LIBRARY_SCAN_INTERVAL) {
            if (scan_library(&library) < 0) {
                fprintf(stderr, "Error scanning library\n");
                return 1;
            }
            num_intervals_without_scan = 0;
        }

        struct timeval select_timeout = SELECT_TIMEOUT;
        if(select(maxfd + 1, &incoming, NULL, NULL, &select_timeout) < 0){
            perror("run_server");
            exit(1);
        }

        if (FD_ISSET(incoming_connections, &incoming)) {
            ClientSocket client_socket = accept_connection(incoming_connections);

            pid_t pid = fork();
            if(pid == -1){
                perror("run_server");
                exit(-1);
            }
            // child process
            if(pid == 0){
                close(incoming_connections);
                free(client_conn_pids);
                int result = handle_client(&client_socket, &library);
                _free_library(&library);
                close(client_socket.socket);
                return result;
            }
            close(client_socket.socket);
            num_connected_clients++;
            client_conn_pids = (pid_t *)realloc(client_conn_pids,
                                               (num_connected_clients)
                                               * sizeof(pid_t));
            client_conn_pids[num_connected_clients - 1] = pid;
        }
        if (FD_ISSET(STDIN_FILENO, &incoming)) {
            if (getchar() == 'q') break;
        }

        num_intervals_without_scan++;
        SET_SERVER_FD_SET(incoming, incoming_connections);

        // Immediate return wait for client processes
        _wait_for_children(&client_conn_pids, &num_connected_clients, 1);
    }

    printf("Quitting server\n");
    close(incoming_connections);
    _wait_for_children(&client_conn_pids, &num_connected_clients, 0);
    _free_library(&library);
    return 0;
}


static uint8_t _is_file_extension_supported(const char *filename){
    static const char *supported_file_exts[] = SUPPORTED_FILE_EXTS;

    for (int i = 0; i < sizeof(supported_file_exts)/sizeof(char *); i++) {
        char *files_ext = strrchr(filename, '.');
        if (files_ext != NULL && strcmp(files_ext, supported_file_exts[i]) == 0) {
            return 1;
        }
    }

    return 0;
}


static int _depth_scan_library(Library *library, char *current_path){

    char *path_in_lib = _join_path(library->path, current_path);
    if (path_in_lib == NULL) {
        return -1;
    }

    DIR *dir = opendir(path_in_lib);
    if (dir == NULL) {
        perror("scan_library");
        return -1;
    }
    free(path_in_lib);

    struct dirent *entry;
    while((entry = readdir(dir)) != NULL) {
        if ((entry->d_type == DT_REG) &&
            _is_file_extension_supported(entry->d_name)) {
            library->files = (char **)realloc(library->files,
                                              (library->num_files + 1)
                                              * sizeof(char *));
            if (library->files == NULL) {
                perror("_depth_scan_library");
                return -1;
            }

            library->files[library->num_files] = _join_path(current_path, entry->d_name);
            if (library->files[library->num_files] == NULL) {
                perror("scan_library");
                return -1;
            }
            #ifdef DEBUG
            printf("Found file: %s\n", library->files[library->num_files]);
            #endif
            library->num_files++;

        } else if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char *new_path = _join_path(current_path, entry->d_name);
            if (new_path == NULL) {
                return -1;
            }

            #ifdef DEBUG
            printf("Library scan descending into directory: %s\n", new_path);
            #endif

            int ret_code = _depth_scan_library(library, new_path);
            free(new_path);
            if (ret_code < 0) {
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}


// This function is implemented recursively and uses realloc to grow the files array
// as it finds more files in the library. It ignores MAX_FILES.
int scan_library(Library *library) {
    // Maximal flexibility, free the old strings and start again
    // A hash table leveraging inode number would be a better way to do this
    #ifdef DEBUG
    printf("^^^^ ----------------------------------- ^^^^\n");
    printf("Freeing library\n");
    #endif
    _free_library(library);

    #ifdef DEBUG
    printf("Scanning library\n");
    #endif
    int result = _depth_scan_library(library, "");
    #ifdef DEBUG
    printf("vvvv ----------------------------------- vvvv\n");
    #endif
    return result;
}


int handle_client(const ClientSocket * client, Library *library) {
    char *request = NULL;
    uint8_t *request_buffer = (uint8_t *)malloc(REQUEST_BUFFER_SIZE);
    if (request_buffer == NULL) {
        perror("handle_client");
        return 1;
    }
    uint8_t *buff_end = request_buffer;

    int bytes_read = 0;
    int bytes_in_buf = 0;
    while((bytes_read = read(client->socket, buff_end, REQUEST_BUFFER_SIZE - bytes_in_buf)) > 0){
        #ifdef DEBUG
        printf("Read %d bytes from client\n", bytes_read);
        #endif

        bytes_in_buf += bytes_read;

        request = find_network_newline((char *)request_buffer, &bytes_in_buf);

        if (request && strcmp(request, REQUEST_LIST) == 0) {
            if (list_request_response(client, library) < 0) {
                ERR_PRINT("Error handling LIST request\n");
                goto client_error;
            }

        } else if (request && strcmp(request, REQUEST_STREAM) == 0) {
            int num_pr_bytes = MIN(sizeof(uint32_t), (unsigned long)bytes_in_buf);
            if (stream_request_response(client, library, request_buffer, num_pr_bytes) < 0) {
                ERR_PRINT("Error handling STREAM request\n");
                goto client_error;
            }
            bytes_in_buf -= num_pr_bytes;
            memmove(request_buffer, request_buffer + num_pr_bytes, bytes_in_buf);

        } else if (request) {
            ERR_PRINT("Unknown request: %s\n", request);
        }

        free(request); request = NULL;
        buff_end = request_buffer + bytes_in_buf;

    }
    if (bytes_read < 0) {
        perror("handle_client");
        goto client_error;
    }

    printf("Client on %s:%d disconnected\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));

    free(request_buffer);
    if (request != NULL) {
        free(request);
    }
    return 0;
client_error:
    free(request_buffer);
    if (request != NULL) {
        free(request);
    }
    return -1;
}


static void print_usage(){
    printf("Usage: as_server [-h] [-p port] [-l library_directory]\n");
    printf("  -h  Print this message\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l  Directory containing the library (default: ./library/)\n");
}


int main(int argc, char * const *argv){
    int opt;
    int port = DEFAULT_PORT;
    const char *library_directory = "library";

    // Check out man 3 getopt for how to use this function
    // The short version: it parses command line options
    // Note that optarg is a global variable declared in getopt.h
    while ((opt = getopt(argc, argv, "hp:l:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage();
                return 0;
            case 'p':
                port = atoi(optarg);
                break;
            case 'l':
                library_directory = optarg;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    printf("Starting server on port %d, serving library in %s\n",
           port, library_directory);

    return run_server(port, library_directory);
}
