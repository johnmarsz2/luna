//
// luna
//
// Copyright © 2016 D.E. Goodman-Wilson
//

#include <netinet/tcp.h>
#include "server_impl.h"

//TODO do this better. Make this an ostream with a custom function. It's not like we haven't done that before.
#define LOG(mesg) if (logger_callback_) \
{ \
    logger_callback_(mesg); \
}

namespace luna
{

struct connection_info_struct
{
    request_method connectiontype;
    query_params post_params;
    MHD_PostProcessor *postprocessor;
};

std::string default_mime_type{"text/html"};

static const server::error_handler_cb default_error_handler_callback_ = [](response &response,
                                                                           request_method method,
                                                                           const std::string &path)
    {
        if (response.content.empty())
        {
            response.content_type = "text/html";
            //we'd best render it ourselves.
            switch (response.status_code)
            {
                case 404:
                    response.content = "<h1>Not found</h1>";
                    break;
                default:
                    response.content = "<h1>So sorry, generic server error</h1>";
            }
        }
    };

static const server::accept_policy_cb default_accept_policy_callback_ = [](const struct sockaddr *addr,
                                                                           socklen_t len) -> bool
    {
        return true;
    };

static status_code default_success_code_(request_method method)
{
    if (method == request_method::POST)
    {
        return 201;
    }

    return 200;
}

static bool is_error_(status_code code)
{
    if (code < 300) return false;

    return true;
}


static request_method method_str_to_enum_(const char *method_str)
{
    if (!std::strcmp(method_str, GET))
    {
        return request_method::GET;
    }

    if (!std::strcmp(method_str, PUT))
    {
        return request_method::PUT;
    }

    if (!std::strcmp(method_str, POST))
    {
        return request_method::POST;
    }

    if (!std::strcmp(method_str, PATCH))
    {
        return request_method::PATCH;
    }

    if (!std::strcmp(method_str, DELETE))
    {
        return request_method::DELETE;
    }

    return request_method::UNKNOWN;
}

///////////////////////////

server::server_impl::server_impl() :
        daemon_{nullptr},
        error_handler_callback_{default_error_handler_callback_},
        accept_policy_callback_{default_accept_policy_callback_}
{ }


void server::server_impl::start()
{
    //TODO not super happy that this has to come outside the constructor.
    // Would strongly prefer if the wrapper constructor could just forward all its varargs to this constructor

    MHD_OptionItem options[options_.size() + 1];
    uint16_t idx = 0;
    for (const auto &opt : options_)
    {
        options[idx++] = opt; //copy it in, whee.
    }
    options[idx] = {MHD_OPTION_END, 0, nullptr};

    daemon_ = MHD_start_daemon(MHD_USE_POLL_INTERNALLY,
                               port_,
                               access_policy_callback_shim_, this,
                               access_handler_callback_shim_, this,
                               MHD_OPTION_NOTIFY_COMPLETED, request_completed_callback_shim_, this,
                               MHD_OPTION_ARRAY, options,
                               MHD_OPTION_END);

    if (!daemon_)
    {
        LOG("Daemon failed to start"); //TODO set some real error flags perhaps?
        return;
    }

    LOG("New server on port " + std::to_string(port_));
}

bool server::server_impl::is_running()
{
    return (daemon_ != nullptr);
}

void server::server_impl::stop()
{
    if (daemon_)
    {
        MHD_stop_daemon(daemon_);
//        std::cout << "Daemon stopped" << std::endl;
        daemon_ = nullptr;
    }
}


server::port server::server_impl::get_port()
{
    return port_;
}

server::server_impl::~server_impl()
{
    stop();
}


void server::server_impl::handle_response(request_method method,
                                          const std::regex &path,
                                          server::endpoint_handler_cb callback)
{
    response_handlers_[method].emplace_back(std::make_pair(path, callback));
}


int parse_kv_(void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
    auto kv = static_cast<query_params *>(cls);
    kv->operator[](key) = value;
    return MHD_YES;
}

int server::server_impl::access_handler_callback_(struct MHD_Connection *connection,
                                                  const char *url,
                                                  const char *method_str,
                                                  const char *version,
                                                  const char *upload_data,
                                                  size_t *upload_data_size,
                                                  void **con_cls)
{
    request_method method = method_str_to_enum_(method_str);

    if (!*con_cls)
    {
//        std::cout << "Preliminaries" << std::endl;

        connection_info_struct *con_info = new(std::nothrow) connection_info_struct();
        if (!con_info) return MHD_NO; //TODO

        con_info->connectiontype = method;

        if (method == request_method::POST)
        {
            con_info->postprocessor = MHD_create_post_processor (connection, 65536, iterate_postdata_shim_, con_info);

            if (!con_info->postprocessor)
            {
                delete con_info;
                return MHD_NO;
            }
        }

        *con_cls = con_info;

        return MHD_YES;
    }
    /////////


//    std::cout << "Received method " << method_str << std::endl;

    //parse the query params:
    std::map<std::string, std::string> header;

    MHD_get_connection_values(connection, MHD_HEADER_KIND, &parse_kv_, &header);


    //find the route, and hit the right callback

    query_params query_params;

    //In case of GET
    if (method == request_method::GET)
    {
        MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, &parse_kv_, &query_params);
    }

    //In case of POST
//    https://gnunet.org/svn/libmicrohttpd/doc/examples/simplepost.c
    else if (method == request_method::POST)
    {
        auto con_info = static_cast<connection_info_struct*>(*con_cls);
        if (*upload_data_size != 0)
        {
            MHD_post_process(con_info->postprocessor, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES; //THIS FLOW IS SO TORTURED. WHAT!? Yet this is how it must be. We will flow through here many times.
        }
        else //we're done getting postdata, add it to the query params
        {
            std::swap(query_params, con_info->post_params); //swap it, for it will soon be destroyed.
        }
    }

    //iterate through the handlers. Could stand being parallelized, I suppose?
    for (const auto &handler_pair : response_handlers_[method])
    {
        std::smatch pieces_match;
        auto path_regex = std::get<std::regex>(handler_pair);
        auto callback = std::get<endpoint_handler_cb>(handler_pair);
        std::string url_str{url};
        if (std::regex_match(url_str, pieces_match, path_regex))
        {
            std::vector<std::string> matches;
            LOG(std::string{"match: "} + url);
            for (size_t i = 0; i < pieces_match.size(); ++i)
            {
                std::ssub_match sub_match = pieces_match[i];
                std::string piece = sub_match.str();
                LOG(std::string{"  submatch "} + std::to_string(i) + ": " + piece);
                matches.emplace_back(sub_match.str());
            }

            response response;
            try
            {
                response = callback(matches, query_params);
            }
            catch (const std::exception &e)
            {
                LOG(e.what());
                response = {500, "text/plain", "Internal error"};
                //TODO render the stack trace, etc.
            }

            if (response.status_code == 0) //no status code was provided, assume success
            {
                response.status_code = default_success_code_(method);
            }

            if (response.content_type.empty()) //no content type assigned, use the default
            {
                response.content_type = default_mime_type;
            }

            if (is_error_(response.status_code))
            {
                return render_error_(response, connection, url, method);
            }

            //else render success
            return render_response_(response, connection, url, method);
        }
    }

    /* unsupported HTTP method */
    return render_error_({404}, connection, url, method);
}

int server::server_impl::render_response_(const response &response,
                                          MHD_Connection *connection,
                                          const char *url,
                                          request_method method) const
{
    auto mhd_response = MHD_create_response_from_buffer(response.content.length(),
                                                        (void *) response.content.c_str(),
                                                        MHD_RESPMEM_MUST_COPY);

//    std::cout << "render_response_ " << response.status_code << " " << response.content << std::endl;

    auto ret = MHD_queue_response(connection,
                                  response.status_code,
                                  mhd_response);
    MHD_add_response_header(mhd_response,
                            MHD_HTTP_HEADER_CONTENT_ENCODING,
                            response.content_type.c_str());
    MHD_destroy_response(mhd_response);
    return ret;
}

int server::server_impl::render_error_(response & response,
                                       MHD_Connection * connection,
        const
char *url,
        request_method
method) const
{
struct MHD_Response *mhd_response;
/* unsupported HTTP method */
error_handler_callback_(response, method, url
); //hook for modifying response

return
render_response_(response, connection, url, method
);
}

int server::server_impl::render_error_(response &&response_r,
                                       MHD_Connection *connection,
                                       const char *url,
                                       request_method method) const
{
    auto response = std::move(response_r);
    struct MHD_Response *mhd_response;
    /* unsupported HTTP method */
    error_handler_callback_(response, method, url); //hook for modifying response

    return render_response_(response, connection, url, method);
}



/////////// callback shims

int server::server_impl::access_handler_callback_shim_(void *cls,
                                                       struct MHD_Connection *connection,
                                                       const char *url,
                                                       const char *method,
                                                       const char *version,
                                                       const char *upload_data,
                                                       size_t *upload_data_size,
                                                       void **con_cls)
{
    if (!cls) return MHD_NO;

    return static_cast<server_impl *>(cls)->access_handler_callback_(connection,
                                                                     url,
                                                                     method,
                                                                     version,
                                                                     upload_data,
                                                                     upload_data_size,
                                                                     con_cls);
}


int server::server_impl::access_policy_callback_shim_(void *cls, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!cls) return MHD_NO;

    return static_cast<server_impl *>(cls)->accept_policy_callback_(addr, addrlen);
}


void server::server_impl::request_completed_callback_shim_(void *cls, struct MHD_Connection *connection,
                                                           void **con_cls,
                                                           enum MHD_RequestTerminationCode toe)
{
    auto con_info = static_cast<connection_info_struct *>(*con_cls);

    if (!con_info)
        return;

    if (con_info->postprocessor)
    {
        MHD_destroy_post_processor (con_info->postprocessor);
    }

    delete con_info;
    *con_cls = NULL;
}

void server::server_impl::uri_logger_callback_shim_(void *cls, const char *uri, struct MHD_Connection *con)
{
    auto this_ptr = static_cast<server_impl *>(cls);
    if (this_ptr && this_ptr->logger_callback_)
    {
        return this_ptr->logger_callback_(uri); //TODO and stuff about the connection too!
    }
}

int server::server_impl::iterate_postdata_shim_(void *cls,
                                                enum MHD_ValueKind kind,
                                                const char *key,
                                                const char *filename,
                                                const char *content_type,
                                                const char *transfer_encoding,
                                                const char *data,
                                                uint64_t off,
                                                size_t size)
{
    auto con_info = static_cast<connection_info_struct *>(cls);
    //TODO we need a better way to handle things like multipart data, than keeping it all in memory. This is a great place for a callback!
//    std::cout << key << " " << data << std::endl;
    if(con_info->post_params.count(key)) //we have already seen this key. Append data!
    {
        con_info->post_params[key] += data;
    }
    else //we haven't yet seen this key
    {
        con_info->post_params[key] = data;
    }
    return MHD_YES;
}

//http://stackoverflow.com/questions/2342162/stdstring-formatting-like-sprintf
std::string string_format(const std::string fmt_str, va_list ap)
{
    int final_n, n = ((int) fmt_str.size()) * 2; /* Reserve two times as much as the length of the fmt_str */
    std::string str;
    std::unique_ptr<char[]> formatted;
    while (1)
    {
        formatted.reset(new char[n]); /* Wrap the plain char array into the unique_ptr */
        strcpy(&formatted[0], fmt_str.c_str());
        final_n = vsnprintf(&formatted[0], n, fmt_str.c_str(), ap);
        if (final_n < 0 || final_n >= n)
        {
            n += abs(final_n - n + 1);
        }
        else
        {
            break;
        }
    }
    return std::string(formatted.get());
}

void server::server_impl::logger_callback_shim_(void *cls, const char *fm, va_list ap)
{
    auto this_ptr = static_cast<server_impl *>(cls);
    if (this_ptr && this_ptr->logger_callback_)
    {
        return this_ptr->logger_callback_(string_format(fm, ap));
    }
}

size_t server::server_impl::unescaper_callback_shim_(void *cls, struct MHD_Connection *c, char *s)
{
    auto this_ptr = static_cast<server_impl *>(cls);
    if (this_ptr && this_ptr->unescaper_callback_)
    {
        auto result = this_ptr->unescaper_callback_(s);
        auto old_len = strlen(s);
        memcpy(s, result.c_str(), old_len);
        return (old_len > result.length()) ? result.length() : old_len;
    }

    return strlen(s); //no change
}

//void server::server_impl::notify_connection_callback_shim_(void *cls,
//                                                           struct MHD_Connection *connection,
//                                                           void **socket_context,
//                                                           enum MHD_ConnectionNotificationCode toe)
//{
//    auto this_ptr = static_cast<server_impl *>(cls);
//    if (this_ptr && this_ptr->notify_connection_callback_)
//    {
//        return this_ptr->notify_connection_callback_(connection, socket_context, toe);
//    }
//}



///// options setting

void server::server_impl::set_option(const server::mime_type &mime_type)
{
    default_mime_type = mime_type;
}

void server::server_impl::set_option(server::error_handler_cb handler)
{
    error_handler_callback_ = handler;
}

void server::server_impl::set_option(server::port port)
{
    port_ = port;
}

void server::server_impl::set_option(server::accept_policy_cb value)
{
    accept_policy_callback_ = value;
}

void server::server_impl::set_option(server::connection_memory_limit value)
{
    //this is a narrowing cast, so ugly! What to do, though?
    options_.push_back({MHD_OPTION_CONNECTION_MEMORY_LIMIT, static_cast<intptr_t>(value), NULL});
}

void server::server_impl::set_option(server::connection_limit value)
{
    options_.push_back({MHD_OPTION_CONNECTION_LIMIT, value, NULL});
}

void server::server_impl::set_option(server::connection_timeout value)
{
    options_.push_back({MHD_OPTION_CONNECTION_TIMEOUT, value, NULL});
}

//void server::server_impl::set_option(server::notify_completed value)
//{
//    //TODO
//}

void server::server_impl::set_option(server::per_ip_connection_limit value)
{
    options_.push_back({MHD_OPTION_PER_IP_CONNECTION_LIMIT, value, NULL});
}

void server::server_impl::set_option(const sockaddr *value)
{
    //why are we casting away the constness? Because MHD isn'T going to modify this, and I want the caller
    // to be assured of this fact.
    options_.push_back({MHD_OPTION_SOCK_ADDR, 0, const_cast<sockaddr *>(value)});
}

//void server::server_impl::set_option(server::uri_log_callback value)
//{
//    options_.push_back({MHD_OPTION_URI_LOG_CALLBACK, value, NULL});
//}

void server::server_impl::set_option(const server::https_mem_key &value)
{
    //TODO this feel very dodgy to me. But I can't quite put my finger on the case where this pointer becomes prematurely invalid
    options_.push_back({MHD_OPTION_HTTPS_MEM_KEY, 0, const_cast<char *>(value.c_str())});
}

void server::server_impl::set_option(const server::https_mem_cert &value)
{
    options_.push_back({MHD_OPTION_HTTPS_MEM_CERT, 0, const_cast<char *>(value.c_str())});
}

//void server::server_impl::set_option(server::https_cred_type value)
//{
//    //TODO
//}

void server::server_impl::set_option(const server::https_priorities &value)
{
    options_.push_back({MHD_OPTION_HTTPS_PRIORITIES, 0, const_cast<char *>(value.c_str())});
}

void server::server_impl::set_option(server::listen_socket value)
{
    options_.push_back({MHD_OPTION_LISTEN_SOCKET, value, NULL});
}

void server::server_impl::set_option(server::logger_cb value)
{
    logger_callback_ = value;

    options_.push_back({MHD_OPTION_EXTERNAL_LOGGER, (intptr_t) &logger_callback_shim_,
                        this}); //YES this must be a C-style cast to work.
    options_.push_back({MHD_OPTION_URI_LOG_CALLBACK, (intptr_t) &uri_logger_callback_shim_, this});
}

void server::server_impl::set_option(server::thread_pool_size value)
{
    options_.push_back({MHD_OPTION_THREAD_POOL_SIZE, value, NULL});
}

void server::server_impl::set_option(server::unescaper_cb value)
{
    unescaper_callback_ = value;
    options_.push_back({MHD_OPTION_UNESCAPE_CALLBACK, (intptr_t) &unescaper_callback_shim_, this});
}

//void server::server_impl::set_option(server::digest_auth_random value)
//{
//    //TODO
//}

void server::server_impl::set_option(server::nonce_nc_size value)
{
    options_.push_back({MHD_OPTION_NONCE_NC_SIZE, value, NULL});
}

void server::server_impl::set_option(server::thread_stack_size value)
{
    options_.push_back({MHD_OPTION_THREAD_STACK_SIZE, static_cast<intptr_t>(value), NULL});
}

void server::server_impl::set_option(const server::https_mem_trust &value)
{
    options_.push_back({MHD_OPTION_HTTPS_MEM_TRUST, 0, const_cast<char *>(value.c_str())});
}

void server::server_impl::set_option(server::connection_memory_increment value)
{
    options_.push_back({MHD_OPTION_CONNECTION_MEMORY_INCREMENT, static_cast<intptr_t>(value), NULL});
}

//void server::server_impl::set_option(server::https_cert_callback value)
//{
//    //TODO
//}

void server::server_impl::set_option(server::tcp_fastopen_queue_size value)
{
    options_.push_back({MHD_OPTION_TCP_FASTOPEN_QUEUE_SIZE, value, NULL});
}

void server::server_impl::set_option(const server::https_mem_dhparams &value)
{
    options_.push_back({MHD_OPTION_HTTPS_MEM_DHPARAMS, 0, const_cast<char *>(value.c_str())});
}

void server::server_impl::set_option(server::listening_address_reuse value)
{
    options_.push_back({MHD_OPTION_LISTENING_ADDRESS_REUSE, value, NULL});
}

void server::server_impl::set_option(const server::https_key_password &value)
{
    options_.push_back({MHD_OPTION_HTTPS_KEY_PASSWORD, 0, const_cast<char *>(value.c_str())});
}

//void server::server_impl::set_option(server::notify_connection value)
//{
//    //TODO
//}


} //namespace luna