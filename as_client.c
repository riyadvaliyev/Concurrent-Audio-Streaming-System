/*****************************************************************************/
/*                       CSC209-24s A4 Audio Stream                          */
/*       Copyright 2024 -- Demetres Kostas PhD (aka Darlene Heliokinde)      */
/*****************************************************************************/
#include "as_client.h"


static int connect_to_server(int port, const char *hostname) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("connect_to_server");
        return -1;
    }

    struct sockaddr_in addr;

    // Allow sockets across machines.
    addr.sin_family = AF_INET;
    // The port the server will be listening on.
    // htons() converts the port number to network byte order.
    // This is the same as the byte order of the big-endian architecture.
    addr.sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr.sin_zero), 0, 8);

    // Lookup host IP address.
    struct hostent *hp = gethostbyname(hostname);
    if (hp == NULL) {
        ERR_PRINT("Unknown host: %s\n", hostname);
        return -1;
    }

    addr.sin_addr = *((struct in_addr *) hp->h_addr);

    // Request connection to server.
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return -1;
    }

    return sockfd;
}


/*
** Helper for: list_request
** This function reads from the socket until it finds a network newline.
** This is processed as a list response for a single library file,
** of the form:
**                   <index>:<filename>\r\n
**
** returns index on success, -1 on error
** filename is a heap allocated string pointing to the parsed filename
*/
static int get_next_filename(int sockfd, char **filename) {
    static int bytes_in_buffer = 0;
    static char buf[RESPONSE_BUFFER_SIZE];

    while((*filename = find_network_newline(buf, &bytes_in_buffer)) == NULL) {
        int num = read(sockfd, buf + bytes_in_buffer,
                       RESPONSE_BUFFER_SIZE - bytes_in_buffer);
        if (num < 0) {
            perror("list_request");
            return -1;
        }
        bytes_in_buffer += num;
        if (bytes_in_buffer == RESPONSE_BUFFER_SIZE) {
            ERR_PRINT("Response buffer filled without finding file\n");
            ERR_PRINT("Bleeding data, this shouldn't happen, but not giving up\n");
            memmove(buf, buf + BUFFER_BLEED_OFF, RESPONSE_BUFFER_SIZE - BUFFER_BLEED_OFF);
        }
    }

    char *parse_ptr = strtok(*filename, ":");
    int index = strtol(parse_ptr, NULL, 10);
    parse_ptr = strtok(NULL, ":");
    // moves the filename to the start of the string (overwriting the index)
    memmove(*filename, parse_ptr, strlen(parse_ptr) + 1);

    return index;
}


int list_request(int sockfd, Library *library) {

  // Send list request to the server
  if (write_precisely(sockfd, "LIST\r\n", 6) < 6) {
      ERR_PRINT("Failed to send list request to the server.");
      perror("write_precisely");
      return -1;
  }

  // Get the last file name in library & its index
  // (that index + 1 would give us the num_files)
  char *file_name = NULL;
  library->num_files = get_next_filename(sockfd, &file_name) + 1;
  if (library->num_files == 0) {
    ERR_PRINT("Failed to retreive the last file name of list.");
    return -1;
  }

  // Reallocate library->files so it has enough room for (library->num_files)
  // many char* pointers
  library->files = realloc(library->files, library->num_files * sizeof(char *));
  if (library->files == NULL) {
    free(file_name);
    perror("realloc");
    return -1;
  }
  // Manually assign the file_name to the last index
  library->files[library->num_files - 1] = strdup(file_name);
  if (library->files[library->num_files - 1] == NULL) {
    free(file_name);
    ERR_PRINT("Failed to allocate space in heap for the last file of library.");
    perror("strdup");
    return -1;
  }
  free(file_name); // Since we won't use file_name ever again and it's already
                   // allocated in the library_files

  // Starting from second-to-last index to 0-th index, populate library->files
  for(int i = (library->num_files - 2); i >= 0; i--) {
    if (get_next_filename(sockfd, &(library->files[i])) < 0) {
      ERR_PRINT("Failed to retrieve a file name in library.");
      return -1;
    }
  }

  // Print the index and file names in an ascending order
  for(int i = 0; i < library->num_files; i++) {
    printf("%d: %s\n", i, library->files[i]);
  }

  return library->num_files;
}


/*
** Get the permission of the library directory. If the library
** directory does not exist, this function shall create it.
**
** library_dir: the path of the directory storing the audio files
** perpt:       an output parameter for storing the permission of the
**              library directory.
**
** returns 0 on success, -1 on error
*/
static int get_library_dir_permission(const char *library_dir, mode_t * perpt) {
  struct stat st;
  while (stat(library_dir, &st) == -1) {
    if (errno == ENOENT) { // If stat failed with ENOENT (library doesn't exist)
      if (mkdir(library_dir, 0700) != 0) { // Create library and call stat again
          ERR_PRINT("Failed to create library_dir.");
          perror("mkdir");
          return -1;
      }
      continue;
    } else { // Otherwise, system error occured, so end the function
      ERR_PRINT("Failed to retrieve file status from stat.");
      perror("stat");
      return -1;
    }
  }
  *perpt = st.st_mode; // Once stat successfully worked, assign perpt and return
  return 0;
}


/*
** Creates any directories needed within the library dir so that the file can be
** written to the correct destination. All directories will inherit the permissions
** of the library_dir.
**
** This function is recursive, and will create all directories needed to reach the
** file in destination.
**
** Destination shall be a path without a leading /
**
** library_dir can be an absolute or relative path, and can optionally end with a '/'
**
*/
static void create_missing_directories(const char *destination, const char *library_dir) {
    // get the permissions of the library dir
    mode_t permissions;
    if (get_library_dir_permission(library_dir, &permissions) == -1) {
      exit(1);
    }

    char *str_de_tokville = strdup(destination);
    if (str_de_tokville == NULL) {
        perror("create_missing_directories");
        return;
    }

    char *before_filename = strrchr(str_de_tokville, '/');
    if (!before_filename){
        goto free_tokville;
    }

    char *path = malloc(strlen(library_dir) + strlen(destination) + 2);
    if (path == NULL) {
        goto free_tokville;
    } *path = '\0';

    char *dir = strtok(str_de_tokville, "/");
    if (dir == NULL){
        goto free_path;
    }
    strcpy(path, library_dir);
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }
    strcat(path, dir);

    while (dir != NULL && dir != before_filename + 1) {
        #ifdef DEBUG
        printf("Creating directory %s\n", path);
        #endif
        if (mkdir(path, permissions) == -1) {
            if (errno != EEXIST) {
                perror("create_missing_directories");
                goto free_path;
            }
        }
        dir = strtok(NULL, "/");
        if (dir != NULL) {
            strcat(path, "/");
            strcat(path, dir);
        }
    }
free_path:
    free(path);
free_tokville:
    free(str_de_tokville);
}


/*
** Helper for: get_file_request
*/
static int file_index_to_fd(uint32_t file_index, const Library * library){
    create_missing_directories(library->files[file_index], library->path);

    char *filepath = _join_path(library->path, library->files[file_index]);
    if (filepath == NULL) {
        return -1;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    #ifdef DEBUG
    printf("Opened file %s\n", filepath);
    #endif
    free(filepath);
    if (fd < 0) {
        perror("file_index_to_fd");
        return -1;
    }

    return fd;
}


int get_file_request(int sockfd, uint32_t file_index, const Library * library){
    #ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
    #endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1) {
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index, -1, file_dest_fd);
    if (result == -1) {
        return -1;
    }

    return 0;
}


int start_audio_player_process(int *audio_out_fd) {
    int pipe_fd[2];

    // Create pipe
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        return -1;
    }

    // Fork process
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }

    if (pid == 0) { // Child process
        // Close write end of the pipe
        close(pipe_fd[1]);

        // Redirect standard input to the pipe
        if (dup2(pipe_fd[0], STDIN_FILENO) == -1) {
            perror("dup2");
            close(pipe_fd[0]);
            exit(1);
        }

        // Close unnecessary file descriptors
        close(pipe_fd[0]);

        // Execute audio player
        char *args[] = AUDIO_PLAYER_ARGS;
        execvp(AUDIO_PLAYER, args);

        // execvp only returns if an error occurs
        perror("execvp");
        exit(1);
    } else { // Parent process
        // Delay before executing audio player
        sleep(AUDIO_PLAYER_BOOT_DELAY);

        // Close read end of the pipe
        close(pipe_fd[0]);

        // Set audio_out_fd
        *audio_out_fd = pipe_fd[1];

        return pid; // Return PID of audio player process
    }
}


static void _wait_on_audio_player(int audio_player_pid) {
    int status;
    if (waitpid(audio_player_pid, &status, 0) == -1) {
        perror("_wait_on_audio_player");
        return;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "Audio player exited with status %d\n", WEXITSTATUS(status));
    } else {
        printf("Audio player exited abnormally\n");
    }
}


int stream_request(int sockfd, uint32_t file_index) {
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    int result = send_and_process_stream_request(sockfd, file_index, audio_out_fd, -1);
    if (result == -1) {
        ERR_PRINT("stream_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}


int stream_and_get_request(int sockfd, uint32_t file_index, const Library * library) {
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    #ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
    #endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1) {
        ERR_PRINT("stream_and_get_request: file_index_to_fd failed\n");
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index,
                                                 audio_out_fd, file_dest_fd);
    if (result == -1) {
        ERR_PRINT("stream_and_get_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}


int send_and_process_stream_request(int sockfd, uint32_t file_index,
                                    int audio_out_fd, int file_dest_fd) {
    // Check file descriptors
    if (audio_out_fd < 0 && file_dest_fd < 0) {
        ERR_PRINT("Both audio_out_fd and file_dest_fd are invalid.");
        return -1;
    }

    // Send stream request packet to the server
    char request_packet[] = "STREAM\r\n";
    if (write_precisely(sockfd, request_packet, 8 * sizeof(char)) <
                                                             8 * sizeof(char)) {
        ERR_PRINT("Failed to send stream request to the server.");
        perror("write_precisely");
        return -1;
    }

    // Send file index to the server
    int *file_index_address = (int*)&file_index;
    uint32_t network_file_index = htonl(*file_index_address);
    if (write_precisely(sockfd, &network_file_index, sizeof(uint32_t)) <
                                                             sizeof(uint32_t)) {
        ERR_PRINT("Failed to send the stream file index to server.");
        perror("write_precisely");
        return -1;
    }

    // Declare the buffers we will be working with
    uint8_t static_buffer[NETWORK_PRE_DYNAMIC_BUFF_SIZE];
    // uint8_t *dynamic_buffer = NULL;
    uint8_t *dynamic_buffer = malloc(1);
    if (dynamic_buffer == NULL) {
      perror("malloc");
      return -1;
    }
    int dynamic_buffer_size = 0;

    // Read the first 4 bytes, which give the file size
    uint8_t file_size_arr[4];
    if (read_precisely(sockfd, file_size_arr, 4 * sizeof(uint8_t)) <
                                                          4 * sizeof(uint8_t)) {
        free(dynamic_buffer);
        ERR_PRINT("Failed to read the stream file size from client.");
        perror("read_precisely");
        return -1;
    }
    int *file_size_ptr = (int *)file_size_arr;
    int file_size = ntohl(*file_size_ptr); // Convert to host byte order

    // Declare variables that will keep track of the read & written data to
    // each stream
    int total_read = 0;
    int total_written_audio;
    if (audio_out_fd != -1) {
      // If we are writing to audio_out_fd, make it 0
      total_written_audio = 0;
    } else {
      // Otherwise, make it always fail in loop conditions
      total_written_audio = file_size;
    }
    int total_written_file;
    if (file_dest_fd != -1) {
      total_written_file = 0;
    } else {
      total_written_file = file_size;
    }

    // Declare variables that will keep the offsets of (the difference between
    // how much was written to) two streams
    int file_offset = 0;
    int audio_offset = 0;

    while (total_written_file < file_size || total_written_audio < file_size) {
      fd_set read_fds, write_fds;
      FD_ZERO(&read_fds);
      FD_ZERO(&write_fds);

      // Set up file descriptor sets for select and check if the each stream is
      // open before doing them, to prevent stack overflow
      if (sockfd >= 0) {
        FD_SET(sockfd, &read_fds);
      }
      if (file_dest_fd >= 0) {
        FD_SET(file_dest_fd, &write_fds);
      }
      if (audio_out_fd >= 0) {
        FD_SET(audio_out_fd, &write_fds);
      }

      // Get the maximum among sockfd, audio_out_fd, file_dest_fd and increment
      // it by 1
      int numfd = sockfd;
      if (numfd < audio_out_fd) {
        numfd = audio_out_fd;
      }
      if (numfd < file_dest_fd){
        numfd = file_dest_fd;
      }
      numfd++;

      struct timeval timeout;
      timeout.tv_sec = SELECT_TIMEOUT_SEC;
      timeout.tv_usec = SELECT_TIMEOUT_USEC;
      // monitor multiple file descriptors, waiting until one or more of the
      // file descriptors become ready for some class of I/O operation
      int ready_fds = select(numfd, &read_fds, &write_fds, NULL, &timeout);
      if (ready_fds == -1) {
        ERR_PRINT("Failed to select file desctiptor that are ready.");
        perror("select");
        free(dynamic_buffer);
        return -1;
      }

      // If sockfd is ready to be read from,
      if (total_read < file_size && FD_ISSET(sockfd, &read_fds)) {
        // Read into static buffer from client.
        int just_read = read(sockfd, static_buffer,
                             NETWORK_PRE_DYNAMIC_BUFF_SIZE);
        if (just_read < 0) {
          ERR_PRINT("Failed to read stream data from client.");
          perror("read");
          free(dynamic_buffer);
          return -1;
        }
        // Realloc dynamic to store additional from static read.
        uint8_t *new_dynamic_buffer = realloc(dynamic_buffer,
                                              dynamic_buffer_size + just_read);
        if (new_dynamic_buffer == NULL) {
          perror("realloc");
          free(dynamic_buffer);
          return -1;
        }
        dynamic_buffer = new_dynamic_buffer;
        // Copy the static buffer to the end of dynamic buffer
        memcpy(dynamic_buffer + dynamic_buffer_size, static_buffer, just_read);
        total_read += just_read;
        dynamic_buffer_size += just_read;
      }

      int just_written_audio = 0;
      if (audio_out_fd != -1 && FD_ISSET(audio_out_fd, &write_fds)
                                           && total_written_audio < file_size) {
        if (dynamic_buffer != NULL) {
          // The amount of buffer that is left exluding the offset from the
          // previous iterations
          int available_buffer = dynamic_buffer_size - audio_offset;
          // Write the audio (to the available buffer) starting from the offset
          just_written_audio = write(audio_out_fd,
                               dynamic_buffer + audio_offset, available_buffer);
          if (just_written_audio < 0) {
            ERR_PRINT("Failed to write stream data to audio.");
            perror("write");
            free(dynamic_buffer);
            return -1;
          }
          // Keep track of number of bytes written to audio
          total_written_audio += just_written_audio;
          audio_offset += just_written_audio;
        }
      }

      int just_written_file = 0;
      if (file_dest_fd != -1 && FD_ISSET(file_dest_fd, &write_fds) &&
                                               total_written_file < file_size) {
        if (dynamic_buffer != NULL) {
          // Same as above
          int available_buffer = dynamic_buffer_size - file_offset;
          just_written_file = write(file_dest_fd, dynamic_buffer + file_offset,
                                                              available_buffer);
          if (just_written_file < 0) {
              ERR_PRINT("Failed to write stream data to file.");
              perror("write");
              free(dynamic_buffer);
              return -1;
          }
          // Keep track of number of bytes written to file
          total_written_file += just_written_file;
          file_offset += just_written_file;
        }
      }

      // Take min of what is written to audio and file to calculate new offsets
      int min_bytes;
      if (file_dest_fd != -1 && audio_out_fd != -1) {
        min_bytes = MIN(just_written_file, just_written_audio);
      } else if (file_dest_fd != -1) { // If we're not using any of the either
        min_bytes = just_written_file; // fds, the used one is automatically min
      } else {
        min_bytes = just_written_audio;
      }
      if (min_bytes > 0) {
        // Memmove over top of that many bytes (shift back by that many bytes)
        memmove(dynamic_buffer, dynamic_buffer + min_bytes,
                                               dynamic_buffer_size - min_bytes);
        // Update the offset of whichever fd is used and update
        // dynamic_buffer_size to remove the data that was read from both
        dynamic_buffer_size -= min_bytes;
        if (file_offset > 0) {
          file_offset -= min_bytes;
        }
        if (audio_offset > 0) {
          audio_offset -= min_bytes;
        }
        // Reallocate the dynamic buffer according to the new size we get after
        // writing off the (commonly received) data
        uint8_t *new_dynamic_buffer = realloc(dynamic_buffer,
                                                           dynamic_buffer_size);
        if (new_dynamic_buffer == NULL && dynamic_buffer_size > 0) {
          perror("realloc");
          free(dynamic_buffer);
          return -1;
        }
        dynamic_buffer = new_dynamic_buffer;
      }
    }

    // Close the open file descriptors and free the heap allocated memory
    free(dynamic_buffer);
    if (audio_out_fd != -1) {
      close(audio_out_fd);
    }
    if (file_dest_fd != -1) {
      close(file_dest_fd);
    }
    return 0;
}



static void _print_shell_help(){
    printf("Commands:\n");
    printf("  list: List the files in the library\n");
    printf("  get <file_index>: Get a file from the library\n");
    printf("  stream <file_index>: Stream a file from the library (without saving it)\n");
    printf("  stream+ <file_index>: Stream a file from the library\n");
    printf("                        and save it to the local library\n");
    printf("  help: Display this help message\n");
    printf("  quit: Quit the client\n");
}


/*
** Shell to handle the client options
** ----------------------------------
** This function is a mini shell to handle the client options. It prompts the
** user for a command and then calls the appropriate function to handle the
** command. The user can enter the following commands:
** - "list" to list the files in the library
** - "get <file_index>" to get a file from the library
** - "stream <file_index>" to stream a file from the library (without saving it)
** - "stream+ <file_index>" to stream a file from the library and save it to the local library
** - "help" to display the help message
** - "quit" to quit the client
*/
static int client_shell(int sockfd, const char *library_directory) {
    char buffer[REQUEST_BUFFER_SIZE];
    char *command;
    int file_index;

    Library library = {"client", library_directory, NULL, 0};

    while (1) {
        if (library.files == 0) {
            printf("Server library is empty or not retrieved yet\n");
        }

        printf("Enter a command: ");
        if (fgets(buffer, REQUEST_BUFFER_SIZE, stdin) == NULL) {
            perror("client_shell");
            goto error;
        }

        command = strtok(buffer, " \n");
        if (command == NULL) {
            continue;
        }

        // List Request -- list the files in the library
        if (strcmp(command, CMD_LIST) == 0) {
            if (list_request(sockfd, &library) == -1) {
                goto error;
            }


        // Get Request -- get a file from the library
        } else if (strcmp(command, CMD_GET) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: get <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (get_file_request(sockfd, file_index, &library) == -1) {
                goto error;
            }

        // Stream Request -- stream a file from the library (without saving it)
        } else if (strcmp(command, CMD_STREAM) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: stream <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_request(sockfd, file_index) == -1) {
                goto error;
            }

        // Stream and Get Request -- stream a file from the library and save it to the local library
        } else if (strcmp(command, CMD_STREAM_AND_GET) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: stream+ <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_and_get_request(sockfd, file_index, &library) == -1) {
                goto error;
            }

        } else if (strcmp(command, CMD_HELP) == 0) {
            _print_shell_help();

        } else if (strcmp(command, CMD_QUIT) == 0) {
            printf("Quitting shell\n");
            break;

        } else {
            printf("Invalid command\n");
        }
    }

    _free_library(&library);
    return 0;
error:
    _free_library(&library);
    return -1;
}


static void print_usage() {
    printf("Usage: as_client [-h] [-a NETWORK_ADDRESS] [-p PORT] [-l LIBRARY_DIRECTORY]\n");
    printf("  -h: Print this help message\n");
    printf("  -a NETWORK_ADDRESS: Connect to server at NETWORK_ADDRESS (default 'localhost')\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l LIBRARY_DIRECTORY: Use LIBRARY_DIRECTORY as the library directory (default 'as-library')\n");
}


int main(int argc, char * const *argv) {
    int opt;
    int port = DEFAULT_PORT;
    const char *hostname = "localhost";
    const char *library_directory = "saved";

    while ((opt = getopt(argc, argv, "ha:p:l:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage();
                return 0;
            case 'a':
                hostname = optarg;
                break;
            case 'p':
                port = strtol(optarg, NULL, 10);
                if (port < 0 || port > 65535) {
                    ERR_PRINT("Invalid port number %d\n", port);
                    return 1;
                }
                break;
            case 'l':
                library_directory = optarg;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    printf("Connecting to server at %s:%d, using library in %s\n",
           hostname, port, library_directory);

    int sockfd = connect_to_server(port, hostname);
    if (sockfd == -1) {
        return -1;
    }

    int result = client_shell(sockfd, library_directory);
    if (result == -1) {
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}
