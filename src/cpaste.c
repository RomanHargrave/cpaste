/*
 * cpaste is an HTTP "paste bin", which accepts - without authentication - 
 * text documents and stores them for later retrieval via a randomly generated
 * identifier.
 *
 * Copyright (C) 2017 Roman Hargrave <roman@hargrave.info>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

// http
#include <kore/kore.h>
#include <kore/http.h>

// configuration
#include <inih/ini.h>

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

// (f)stat, open()
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// sendfile()
#include <sys/sendfile.h>

// m(un)map()
#include <sys/mman.h>

// time() for srand()
#include <time.h>

// __xpg_strerror_r(), even though...
#include <string.h>

int HTTP_cpaste_route_main(struct http_request*);
int HTTP_cpaste_route_view(struct http_request*);
//-------------------------------------------------------------------------------------------------

/*
 * System/libc support
 */
extern int __xpg_strerror_r (int, char*, size_t); // thanks, glibc 
static inline char*
cpaste_strerror(int errint)
{
    char* buff = malloc(512 * sizeof(char));
    __xpg_strerror_r(errint, buff, 512); // thanks, glibc 
    return buff;
}

static inline off_t
cpaste_fdsize(int fd)
{
    off_t pos = lseek(fd, 0, SEEK_CUR);
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, pos, SEEK_SET);
    return size;
}

//-------------------------------------------------------------------------------------------------

/*
 * Configuration
 *
 * cpaste will look for ./cpaste.ini or use the file specified in the
 * environment variable `CPASTE_CONFIG_FILE`
 */

struct s_cpaste_config
{
    // Section: storage
    char const*     storage_dir;
    uint64_t        _storage_dir_len; 
    //              ^ not in config
    uint32_t        name_length;
};

static struct s_cpaste_config* _cpaste_config = NULL;

/*
 * inih "driver" implementation
 */
static int
_cpaste_inih_impl(void* cb_config, char const* section,
                  char const* name, char const* value)
{
    struct s_cpaste_config* config = (struct s_cpaste_config*) cb_config;

    if (strcmp(section, "storage") == 0)
    {
        if      (strcmp(name, "directory") == 0) 
        {
            config->storage_dir         = strdup(value);
            config->_storage_dir_len    = strlen(value);
        }
        else if (strcmp(name, "namelen") == 0) config->name_length = strtoul(value, NULL, 10);
        else return 0;
    }
    // No other sections ATM
    else
    {
        return 1;
    }

    return 1;
}

/*
 * Returns the already loaded configuration, or loads and returns it.
 */
static struct s_cpaste_config* 
cpaste_get_config(void)
{
    if (_cpaste_config)
    {
        return _cpaste_config;
    }
    else
    {
        kore_log(LOG_DEBUG, "Loading config");
        // Find the config file:
        // - check CPASTE_CONFIG_FILE
        // - check PWD
        // - explode
        char* config_file = getenv("CPASTE_CONFIG_FILE");
        if (config_file == NULL)
        {
            config_file = "./cpaste.ini";
        }

        FILE* config_file_handle = fopen(config_file, "r");
        if (config_file_handle == NULL)
        {
            if (errno == ENOENT)
            {
                kore_log(LOG_EMERG, "cpaste configuration not found at ./cpaste.ini or in CPASTE_CONFIG_FILE");
            }

            kore_log(LOG_EMERG, "unable to open cpaste configuration\n");
            exit(errno);
        }
        else
        {
            _cpaste_config = malloc(sizeof(struct s_cpaste_config));
            int parse_result = ini_parse_file(config_file_handle, &_cpaste_inih_impl, _cpaste_config);
            if      (parse_result == 0)
            {
                fclose(config_file_handle);
                return _cpaste_config;
            }
            else if (parse_result == -1)
            {
                fclose(config_file_handle);
                kore_log(0, "unable to open config file for read\n");
                exit(1);
            }
            else if (parse_result == -2)
            {
                fclose(config_file_handle);
                kore_log(0, "unable to allocate memory to read config\n");
                exit(1);
            }
            else
            {
                fclose(config_file_handle);
                kore_log(0, "parse error reading config at line %u\n",
                         parse_result);
                exit(1);
            }

            // TODO semantic exit codes needed
        }
    }
}

//-------------------------------------------------------------------------------------------------

/*
 * Paste ID generator
 */

/*
 * "Pool" of characters which may be selected from by the paste id generator (cpaste_gen_id())
 * XXX if you modify this, be sure to update the router configuration in conf/cpaste.conf (NOT cpaste.ini)
 */
static char const* _id_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
static uint16_t const _id_chars_lim = 62;

/*
 * Generate a random alphanumeric string `len` chars+1 (added null) long
 * Allocates result - needs to be released when done
 */
static void
cpaste_gen_id(uint16_t len, char* dest)
{
    srand(time(NULL));
    for(uint16_t charN = 0; charN <= len; ++charN)
    {
        dest[charN] = _id_chars[random() % _id_chars_lim]; 
    }
}

//-------------------------------------------------------------------------------------------------

/*
 * Paste Interface
 * functions to create and read pastes.
 * if you want to replace the filesystem backend, this is what you need to change
 */

/*
 * Checks that a paste with `paste_id` exists 
 */
static inline bool
cpaste_paste_check_exists(struct s_cpaste_config* config, char const* paste_id)
{
    char* path;
    asprintf(&path, "%s/%s", config->storage_dir, paste_id);
    FILE* handle = fopen(path, "r");
    if (handle)
    {
        fclose(handle);
        return true;
    }
    else
    {
        return false;
    }
}


/*
 * Generates file names and checks their availability (e.g. don't exist yet)
 * until an unused filename is found.
 *
 * In practice this should only very rarely repeat, and even then only once.
 */
static char const*
cpaste_file_get_available(struct s_cpaste_config* config)
{
    char* basename = malloc((config->name_length + 1) * sizeof(char));
    char* path     = malloc((config->name_length + config->_storage_dir_len + 1) * sizeof(char));
    while (true)
    {
        cpaste_gen_id(config->name_length, basename);

        sprintf(path, "%s/%s", config->storage_dir, basename); // no need for snprintf when length is absolute

        struct stat file_data;
        int stat_status = stat(path, &file_data);
        if (stat_status == 0)
        {
            if (file_data.st_size == 0)
            {
                // file is empty - e.g. was created by a failed paste attempt
                break;
            }
            // else try again
        }
        else if (errno == ENOENT)
        {
            // file is available!
            break;
        }
        // try again, etc...
        // XXX potential for infinite loop under edge case filesystem behaviour
    }

    free(path);
    return basename;
}

/*
 * Open an FD for the paste with `id` using `config`
 * FD will need to be closed when finished with - see cpaste_s_paste_free() or close(), depending
 * Will return -1 via open() or when asprintf() fails
 */
static inline int /* fd */
cpaste_paste_open(struct s_cpaste_config* config, char const* id)
{
    char* path;
    int _asprintf_status = asprintf(&path, "%s/%s", config->storage_dir, id);
    
    if (_asprintf_status >= 0)
    {
        int fd = open(path, O_RDWR | O_CREAT, 0640);
        kore_log(LOG_DEBUG, "cpaste_paste_open() opening paste %s (in file %s) fd = %i",
                 id, path, fd);
        free(path);
        return fd;
    }
    else
    {
        char* errdesc = cpaste_strerror(errno);
        kore_log(LOG_EMERG, "cpaste_paste_open() asprintf path failed - %s" ,
                 errdesc);
        free(errdesc);
        return -1;
    }
}

/*
 * Simple helper routine to find an available ID and open an fd to it.
 * OUT_name is an output param, and will have the name written to it.
 *
 * cpase_paste_open() result is returned directly.
 */
static inline int /* fd */
cpaste_generate_paste(struct s_cpaste_config* config, char** OUT_name)
{
    *OUT_name = cpaste_file_get_available(config);
    return cpaste_paste_open(config, *OUT_name);
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

/*
 * KORE
 */

/*
 * static   /
 */

// Data to send for empty GET / 
static char const* _resp_default_body =
    "cpaste(1)                          CPASTE                          cpaste(1)\n"
    "\n"
    "NAME\n"
    "   cpaste: sprunge clone written in a better language than python\n"
    "\n"
    "SYNOPSIS\n"
    "   <command> | curl -F 'paste<=-' (cpaste host)\n"
    "\n"
    "EXAMPLES\n"
    "   $ curl -F 'paste<=-' (host) < /path/to/file\n"
    "       (host)/aB123\n"
    "\n"
    "SEE ALSO\n"
    "   http://github.com/romanhargrave/cpaste\n"
    ;

static char const* _resp_paste_gen_fail = 
    "unable to generate a new paste\n"
    ;

/*
 * Kore http_response shortcut for C strings
 */
static inline void
cpaste_http_resp_string(struct http_request* req, uint16_t resp_code,
                        char const* body)
{
    http_response(req, resp_code, body, strlen(body));
}

/*
 * Location: /
 * Methods: All. Special behaviour when POST.
 *
 *  When not POST, returns usage information
 *  When POST, the first field from the upload is picked and stored, after which the paste name is returned and the 
 *  client, if it likes, is redirected to the paste (cpaste_kore_view_paste())
 */
int
HTTP_cpaste_route_main(struct http_request* req)
{
    struct s_cpaste_config* config = cpaste_get_config();
    /*
     * If not POST, respond with info
     */
    if (req->method != HTTP_METHOD_POST) 
    {
    	http_response(req, 200, _resp_default_body, strlen(_resp_default_body));
    	return (KORE_RESULT_OK);
    }
    else
    {
        kore_log(LOG_DEBUG, "incoming paste request");

        // Read POSTed request body
        http_populate_multipart_form(req);

        // http file handle 
        struct http_file* to_paste;
        // Pick the first file 
        TAILQ_FOREACH(to_paste, &(req->files), list) 
        {
            kore_log(LOG_DEBUG, "picked the first field as paste contents: %s",
                     to_paste->name);
            break;
        }

        if (to_paste == NULL)
        {
            kore_log(LOG_DEBUG, "no paste field - disconnecting with error");
            cpaste_http_resp_string(req, 400, "No file present in request body");
            return KORE_RESULT_OK;
        }
        else
        {
            char* paste_id = NULL;
            int paste_fd = cpaste_generate_paste(config, &paste_id);
            
            if (paste_fd > 0)
            {
                kore_log(LOG_INFO, "recieving new paste %s",
                         paste_id);

                // ----------------------------------------------------------------------------
                //
                // READING THE FIELD
                //
                //  Kore can store the request body in two ways, as suggested in http_file_read()
                //  There can be a file descriptor for it, which allows for file IO 
                //   (I assume this is used for larger rb's? or binary transfer?)
                //  Or the RB might be stored in an (mmapped?) memory region at req->http_body->data
                //
                // READING THE RB FD (http_body_fd != -1)
                //
                //  XXX never tested during development, not sure what conditions cause this 
                //
                //  We can perform a copy from fd to fd using the sendfile() system call,
                //   and specifying the offset as the position of the file
                //  
                // READING THE RB REGION (http_body_fd == -1)
                //
                //  We can read the memory region containing the RB from the position of the file 
                //
                // ----------------------------------------------------------------------------
               
                ssize_t written = 0;
                if (req->http_body_fd > 0 || req->http_body != NULL)
                {
                    if (req->http_body_fd > 0)
                    {
                        kore_log(LOG_DEBUG, "request body presented as FD, saving via sendfile()");
                        off_t read_offset = to_paste->position;
                        do 
                        {
                            written = sendfile(paste_fd, to_paste->req->http_body_fd, 
                                               &read_offset, to_paste->length - written);
                            kore_log(LOG_DEBUG, "copying paste %s from request buffer to file: +%li %li/%lu",
                                     paste_id, read_offset, written, to_paste->length);
                        }
                        while ((written > 0) && (written < ((ssize_t) to_paste->length)));

                        if (written < 0)
                        {
                            char* errdesc = cpaste_strerror(errno);
                            kore_log(LOG_ERR, "error copying paste from request buffer: %s",
                                     errdesc);
                            free(errdesc);
                            cpaste_http_resp_string(req, 500, _resp_paste_gen_fail);
                        }
                    }
                    else if (req->http_body != NULL)
                    {
                        kore_log(LOG_DEBUG, "request body presented as region, saving via write()");
                        written = write(paste_fd, req->http_body->data + to_paste->position + to_paste->offset, 
                                        to_paste->length);
                    }

                    // Follow up on paste save and check write completion

                    if (written > 0)
                    {
                        kore_log(LOG_INFO, "paste intake for %s complete - wrote %li of %li bytes",
                                 paste_id, written, to_paste->length);
                        http_response(req, 200, paste_id, strlen(paste_id));
                    }
                    else if (written < 0)
                    {
                        char* errdesc = cpaste_strerror(errno);
                        kore_log(LOG_ERR, "paste intake failed: %s",
                                 errdesc);
                        free(errdesc);
                        cpaste_http_resp_string(req, 500, _resp_paste_gen_fail);
                    }
                    else
                    {
                        kore_log(LOG_DEBUG, "paste was empty");
                        cpaste_http_resp_string(req, 400, "Empty paste");
                    }
                }
                else
                {
                    cpaste_http_resp_string(req, 400, "Empty request body");
                }

            }
            else
            {
                char* errdesc = cpaste_strerror(errno);
                kore_log(LOG_ERR, "Could not obtain fd for a new paste: %s",
                         errdesc);
                free(errdesc);
                http_response(req, 500, _resp_paste_gen_fail, strlen(_resp_paste_gen_fail));
            }

            // Intake Cleanup

            close(paste_fd);
            free(paste_id);

            return KORE_RESULT_OK;
        }
    }
}

/*
 * dynamic /[A-Za-z0-9]+$
 */
int
HTTP_cpaste_route_view(struct http_request* req)
{
    struct s_cpaste_config* config = cpaste_get_config();

    // Extract paste id from request path
    char const* paste_id = req->path + 1;

    kore_log(LOG_INFO, "Retrieving paste %s", paste_id);

    int const paste_fd = cpaste_paste_open(config, paste_id);

    if (paste_fd >= 0)
    {
        off_t const paste_size = cpaste_fdsize(paste_fd);

        if (paste_size == -1)
        {
            cpaste_http_resp_string(req, 500, "Could not figure out the paste size");

            char* errdesc = cpaste_strerror(errno);
            kore_log(LOG_ERR, "could not query fd length for fd %i: %s",
                     paste_fd, errdesc);
            free(errdesc);
        }
        else 
        {
            void* paste_data = mmap(NULL, paste_size, PROT_READ, MAP_SHARED, paste_fd, 0);

            if (paste_data != MAP_FAILED)
            {
                http_response(req, 200, paste_data, paste_size);
                munmap(paste_data, paste_size);
            }
            else
            {
                cpaste_http_resp_string(req, 500, "IO error while reading paste data");
                char* errdesc = cpaste_strerror(errno);
                kore_log(LOG_ERR, "error mmapping paste: %s", errdesc);
                free(errdesc);
            }
        }
        close(paste_fd);
    }
    else
    {
        switch (errno)
        {
            case ENOENT:
                cpaste_http_resp_string(req, 404, "That paste doesn't exist");
                break;
            default:
                cpaste_http_resp_string(req, 500, "Unable to open paste");
                char* errdesc = cpaste_strerror(errno); 
                kore_log(LOG_ERR, "Error reading paste %s: %s",
                         paste_id, errdesc);
                free(errdesc);
                break;
        }
    }

    return KORE_RESULT_OK;
}
