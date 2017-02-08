#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define vdb_work_buffer_size (1024*1024)
#define vdb_recv_buffer_size (1024*1024)
#ifdef VDB_LISTEN_PORT
static int vdb_listen_port = VDB_LISTEN_PORT;
#else
static int vdb_listen_port = 0;
#endif

int vdb_set_listen_port(int port)
{
    #ifdef VDB_LISTEN_PORT
    printf("[vdb] Warning: You are setting the port with vdb_set_listen_port and #define VDB_LISTEN_PORT.\nAre you sure this is intentional?\n");
    #endif
    vdb_listen_port = port;
    return 1;
}

struct vdb_shared_t
{
    int has_recv_thread;
    int has_send_thread;
    int has_connection;

    int busy;
    int work_buffer_used;
    int bytes_to_send;
    char recv_buffer[vdb_recv_buffer_size];
    char swapbuffer1[vdb_work_buffer_size];
    char swapbuffer2[vdb_work_buffer_size];
    char *work_buffer;
    char *send_buffer;

    // These pipes are used for flow control between the main thread and the sending thread
    // The sending thread blocks on a read on pipe_ready, until the main thread signals the
    // pipe by write on pipe_ready. The main thread checks if sending is complete by polling
    // (non-blocking) pipe_done, which is signalled by the sending thread.
    int ready[2]; // [0]: read, [1]: send
    int done[2];// [0]: read, [1]: send
};

static vdb_shared_t *vdb_shared = 0;

int vdb_wait_data_ready()   { int val = 0; return  read(vdb_shared->ready[0], &val, sizeof(val)) == sizeof(val); }
int vdb_poll_data_sent()    { int val = 0; return   read(vdb_shared->done[0], &val, sizeof(val)) == sizeof(val); }
int vdb_signal_data_ready() { int one = 1; return write(vdb_shared->ready[1], &one, sizeof(one)) == sizeof(one); }
int vdb_signal_data_sent()  { int one = 1; return  write(vdb_shared->done[1], &one, sizeof(one)) == sizeof(one); }

int vdb_push_s32(int32_t x)
{
    if (vdb_shared->work_buffer_used + sizeof(x) < vdb_work_buffer_size)
    {
        *(int32_t*)(vdb_shared->work_buffer + vdb_shared->work_buffer_used) = x;
        vdb_shared->work_buffer_used += sizeof(x);
        return 1;
    }
    return 0;
}

int vdb_push_r32(float x)
{
    if (vdb_shared->work_buffer_used + sizeof(x) < vdb_work_buffer_size)
    {
        *(float*)(vdb_shared->work_buffer + vdb_shared->work_buffer_used) = x;
        vdb_shared->work_buffer_used += sizeof(x);
        return 1;
    }
    return 0;
}

int vdb_sendall(void *buffer, int bytes_to_send)
{
    int sent;
    int remaining = bytes_to_send;
    char *ptr = (char*)buffer;
    while (remaining > 0)
    {
        if (!tcp_send(ptr, remaining, &sent))
        {
            vdb_log("failed to send all data (%s)\n", strerror(errno));
            return 0;
        }
        remaining -= sent;
        ptr += sent;
        if (remaining < 0)
        {
            vdb_log("sent too much data\n");
            return 0;
        }
    }
    return 1;
}

void vdb_send_thread()
{
    vdb_shared_t *vs = vdb_shared;
    unsigned char *frame = 0; // @ UGLY: form_frame should modify a char *?
    int frame_len;
    vdb_log("created send thread\n");
    while (1)
    {
        // blocking until data is signalled ready from main thread
        if (!vdb_wait_data_ready()) return;

        // send frame header
        vdb_form_frame(vs->bytes_to_send, &frame, &frame_len);
        if (!vdb_sendall(frame, frame_len))
        {
            vdb_log("failed to send frame\n");
            vdb_signal_data_sent();
            return;
        }

        // send the payload
        if (!vdb_sendall(vs->send_buffer, vs->bytes_to_send))
        {
            vdb_log("failed to send payload\n");
            vdb_signal_data_sent();
            return;
        }

        // signal to main thread that data has been sent
        if (!vdb_signal_data_sent()) return;
    }
}

void vdb_recv_thread(int port)
{
    vdb_shared_t *vs = vdb_shared;
    int read_bytes = 0;
    pid_t send_pid = 0;
    vdb_log("created read thread\n");
    while (1)
    {
        if (!has_listen_socket)
        {
            if (vdb_listen_port == 0)
            {
                printf("[vdb] Warning: You have not set the listening port for vdb. Using 8000.\n"
                       "You can use a different port by calling vdb_set_listen_port(<port>),\n"
                       "or by #define VDB_LISTEN_PORT <port> before #including vdb.c.\n"
                       "Valid ports are between 1024 and 65535.\n");
                vdb_listen_port = 8000;
            }
            vdb_log("creating listen socket\n");
            if (!tcp_listen(vdb_listen_port))
            {
                vdb_log("listen failed\n");
                usleep(1000*1000);
                continue;
            }
        }
        if (!has_client_socket)
        {
            vdb_log("waiting for client\n");
            if (!tcp_accept())
            {
                usleep(1000*1000);
                continue;
            }
        }
        if (!vs->has_connection)
        {
            char *response;
            int response_len;
            vdb_log("waiting for handshake\n");
            if (!tcp_recv(vs->recv_buffer, vdb_recv_buffer_size, &read_bytes))
            {
                vdb_log("lost connection during handshake\n");
                tcp_shutdown();
                usleep(1000*1000);
                continue;
            }

            vdb_log("generating handshake\n");
            if (!vdb_generate_handshake(vs->recv_buffer, read_bytes, &response, &response_len))
            {
                vdb_log("failed to generate handshake\n");
                tcp_shutdown();
                usleep(1000*1000);
                continue;
            }

            vdb_log("sending handshake\n");
            if (!vdb_sendall(response, response_len))
            {
                vdb_log("failed to send handshake\n");
                tcp_shutdown();
                usleep(1000*1000);
                continue;
            }

            vs->has_connection = 1;
        }
        if (!vs->has_send_thread) // @ RACECOND: UGLY: Spawn only one sending thread!!!
        {
            send_pid = fork();
            if (send_pid == -1) { vdb_log("failed to fork\n"); usleep(1000*1000); continue; }
            if (send_pid == 0)
            {
                vs->has_send_thread = 1;
                vdb_send_thread();
                vs->has_send_thread = 0;
                _exit(0);
            }
            usleep(100*1000); // @ RACECOND: UGLY: Let child process set has_send_thread = 1
        }
        if (!tcp_recv(vs->recv_buffer, vdb_recv_buffer_size, &read_bytes)) // @ INCOMPLETE: Assemble frames
        {
            vdb_log("connection went down\n");
            vs->has_connection = 0;
            tcp_shutdown();
            if (vs->has_send_thread)
            {
                kill(send_pid, SIGUSR1);
                vs->has_send_thread = 0;
            }
            usleep(100*1000);
            continue;
        }

        {
            vdb_msg_t msg = {0};
            if (vdb_parse_message(vs->recv_buffer, read_bytes, &msg))
            {
                if (!msg.fin)
                {
                    vdb_log("got an incomplete message (%d): '%s'\n", msg.length, msg.payload);
                }
                else
                {
                    vdb_log("got a final message (%d): '%s'\n", msg.length, msg.payload);
                    if (strcmp(msg.payload, "shutdown") == 0)
                    {
                        // vdb_should_close = 1;
                    }
                }
            }
        }
    }
}

int vdb_begin()
{
    if (vdb_listen_port < 1024 || vdb_listen_port > 65535)
    {
        printf("[vdb] Error: You have requested a vdb_listen_port (%d) outside the valid region of 1024-65535.\n", vdb_listen_port);
        return 0;
    }

    if (!vdb_shared)
    {
        // Share memory between main process and child processes
        vdb_shared = (vdb_shared_t*)mmap(NULL, sizeof(vdb_shared_t), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
        if (vdb_shared == MAP_FAILED)
        {
            vdb_log("mmap failed\n");
            vdb_shared = 0;
            return 0;
        }

        // Create flow-control pipes
        if (pipe(vdb_shared->ready) == -1 || pipe2(vdb_shared->done, O_NONBLOCK) == -1)
        {
            vdb_log("pipe failed\n");
            vdb_shared = 0;
            return 0;
        }

        vdb_shared->work_buffer = vdb_shared->swapbuffer1;
        vdb_shared->send_buffer = vdb_shared->swapbuffer2;
        // remaining parameters are zero-initialized by mmap
    }

    vdb_shared_t *vs = vdb_shared;

    if (!vs->has_recv_thread)
    {
        pid_t pid = fork();
        if (pid == -1)
        {
            vdb_log("fork failed\n");
            return 0;
        }
        if (pid == 0) // is child
        {
            vs->has_recv_thread = 1; // @ RACECOND: MIGHT SPAWN MULTIPLE RECV THREADS!!
            vdb_recv_thread(vdb_listen_port);
            vs->has_recv_thread = 0;
            _exit(0);
        }
        usleep(100*1000); // @ REMOVEME: UGLY: To allow child thread to set has_recv_thread
    }

    if (!vs->has_connection)
    {
        return 0;
    }

    return 1;
}

void vdb_end()
{
    vdb_shared_t *vs = vdb_shared;

    if (vdb_poll_data_sent())
        vs->busy = 0;

    if (!vs->busy)
    {
        char *new_work_buffer = vs->send_buffer;

        vs->send_buffer = vs->work_buffer;
        vs->bytes_to_send = vs->work_buffer_used;
        vdb_signal_data_ready();

        vs->work_buffer = new_work_buffer;
        vs->work_buffer_used = 0;
        vs->busy = 1;
    }
}

void vdb_sleep(int milliseconds)
{
    usleep(milliseconds*1000);
}