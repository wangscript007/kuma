#include "kmconf.h"

#if defined(KUMA_OS_WIN)
# include <Ws2tcpip.h>
# include <windows.h>
# include <time.h>
#elif defined(KUMA_OS_LINUX)
# include <string.h>
# include <pthread.h>
# include <unistd.h>
# include <fcntl.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/socket.h>
# include <netdb.h>
# include <arpa/inet.h>
# include <netinet/tcp.h>
# include <netinet/in.h>
#elif defined(KUMA_OS_MAC)
# include <string.h>
# include <pthread.h>
# include <unistd.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <sys/ioctl.h>
# include <sys/fcntl.h>
# include <sys/time.h>
# include <sys/uio.h>
# include <netinet/tcp.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <ifaddrs.h>
#else
# error "UNSUPPORTED OS"
#endif

#include <stdarg.h>
#include <errno.h>

#include "TcpSocket.h"
#include "EventLoop.h"
#include "util/util.h"
#include "util/kmtrace.h"

#ifdef KUMA_HAS_OPENSSL
# include "ssl/SslHandler.h"
#endif

KUMA_NS_BEGIN

TcpSocket::TcpSocket(EventLoop* loop)
: fd_(INVALID_FD)
, loop_(loop)
, state_(ST_IDLE)
, registered_(false)
, destroy_flag_ptr_(nullptr)
, flags_(0)
, ssl_handler_(nullptr)
{
    
}

TcpSocket::~TcpSocket()
{
    if(destroy_flag_ptr_) {
        *destroy_flag_ptr_ = true;
    }
    cleanup();
}

const char* TcpSocket::getObjKey()
{
    return "TcpSocket";
}

void TcpSocket::cleanup()
{
#ifdef KUMA_HAS_OPENSSL
    if(ssl_handler_) {
        ssl_handler_->close();
        delete ssl_handler_;
        ssl_handler_ = nullptr;
    }
#endif
    if(INVALID_FD != fd_) {
        SOCKET_FD fd = fd_;
        fd_ = INVALID_FD;
        shutdown(fd, 0); // only stop receive
        if(registered_) {
            registered_ = false;
            loop_->unregisterFd(fd, true);
        } else {
            closeFd(fd);
        }
    }
}

int TcpSocket::bind(const char *local_ip, uint16_t local_port)
{
    if(getState() != ST_IDLE) {
        KUMA_ERRXTRACE("bind, invalid state, state="<<getState());
        return KUMA_ERROR_INVALID_STATE;
    }
    if(fd_ != INVALID_FD) {
        cleanup();
    }
    sockaddr_storage ss_addr = {0};
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;//AI_ADDRCONFIG; // will block 10 seconds in some case if not set AI_ADDRCONFIG
    if(km_set_sock_addr(local_ip, local_port, &hints, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) != 0) {
        return KUMA_ERROR_INVALID_PARAM;
    }
    fd_ = ::socket(ss_addr.ss_family, SOCK_STREAM, 0);
    if(INVALID_FD == fd_) {
        KUMA_ERRXTRACE("bind, socket failed, err="<<getLastError());
        return KUMA_ERROR_FAILED;
    }
    int ret = ::bind(fd_, (struct sockaddr*)&ss_addr, sizeof(ss_addr));
    if(ret < 0) {
        KUMA_ERRXTRACE("bind, bind failed, err="<<getLastError());
        return KUMA_ERROR_FAILED;
    }
    return KUMA_ERROR_NOERR;
}

int TcpSocket::connect(const char *addr, uint16_t port, EventCallback& cb, uint32_t flags, uint32_t timeout)
{
    if(getState() != ST_IDLE) {
        KUMA_ERRXTRACE("connect, invalid state, state="<<getState());
        return KUMA_ERROR_INVALID_STATE;
    }
    cb_connect_ = cb;
    flags_ = flags;
    return connect_i(addr, port, timeout);
}

int TcpSocket::connect(const char *addr, uint16_t port, EventCallback&& cb, uint32_t flags, uint32_t timeout)
{
    if(getState() != ST_IDLE) {
        KUMA_ERRXTRACE("connect, invalid state, state="<<getState());
        return KUMA_ERROR_INVALID_STATE;
    }
    cb_connect_ = std::move(cb);
    flags_ = flags;
    return connect_i(addr, port, timeout);
}

int TcpSocket::connect_i(const char* addr, uint16_t port, uint32_t timeout)
{
#ifndef KUMA_HAS_OPENSSL
    if (SslEnabled()) {
        KUMA_ERRXTRACE("connect, OpenSSL is disabled");
        return KUMA_ERROR_UNSUPPORT;
    }
#endif
    sockaddr_storage ss_addr = {0};
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG; // will block 10 seconds in some case if not set AI_ADDRCONFIG
    if(km_set_sock_addr(addr, port, &hints, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) != 0) {
        return KUMA_ERROR_INVALID_PARAM;
    }
    if(INVALID_FD == fd_) {
        fd_ = ::socket(ss_addr.ss_family, SOCK_STREAM, 0);
        if(INVALID_FD == fd_) {
            KUMA_ERRXTRACE("connect, socket failed, err="<<getLastError());
            return KUMA_ERROR_FAILED;
        }
    }
    setSocketOption();
    
    int addr_len = sizeof(ss_addr);
#ifdef KUMA_OS_MAC
    if(AF_INET == ss_addr.ss_family)
        addr_len = sizeof(sockaddr_in);
    else
        addr_len = sizeof(sockaddr_in6);
#endif
    int ret = ::connect(fd_, (struct sockaddr *)&ss_addr, addr_len);
    if(0 == ret) {
        setState(ST_CONNECTING); // wait for writable event
    } else if(ret < 0 &&
#ifdef KUMA_OS_WIN
              WSAEWOULDBLOCK
#else
              EINPROGRESS
#endif
              == getLastError()) {
        setState(ST_CONNECTING);
    } else {
        KUMA_ERRXTRACE("connect, error, fd="<<fd_<<", addr="<<addr<<", err"<<getLastError());
        cleanup();
        setState(ST_CLOSED);
        return KUMA_ERROR_FAILED;
    }
#if defined(KUMA_OS_LINUX) || defined(KUMA_OS_MAC)
    socklen_t len = sizeof(ss_addr);
#else
    int len = sizeof(ss_addr);
#endif
    char my_addr[128] = {0};
    uint16_t my_port = 0;
    ret = getsockname(fd_, (struct sockaddr*)&ss_addr, &len);
    if(ret != -1) {
        km_get_sock_addr((struct sockaddr*)&ss_addr, sizeof(ss_addr), my_addr, sizeof(my_addr), &my_port);
    }
    
    KUMA_INFOXTRACE("connect, fd: "<<fd_<<", my_addr: "<<my_addr
                   <<", my_port: "<<my_port<<", state: "<<getState());
    
    loop_->registerFd(fd_,
#ifdef KUMA_OS_WIN
                      FD_CONNECT |
#endif
                      KUMA_EV_NETWORK,
                      [this] (uint32_t ev) { ioReady(ev); });
    registered_ = true;
    return KUMA_ERROR_NOERR;
}

int TcpSocket::attachFd(SOCKET_FD fd, uint32_t flags)
{
    KUMA_INFOXTRACE("attachFd, fd="<<fd<<", state="<<getState());
    if(getState() != ST_IDLE) {
        KUMA_ERRXTRACE("attachFd, invalid state, state="<<getState());
        return KUMA_ERROR_INVALID_STATE;
    }
#ifndef KUMA_HAS_OPENSSL
    if (SslEnabled()) {
        KUMA_ERRXTRACE("attachFd, OpenSSL is disabled");
        return KUMA_ERROR_UNSUPPORT;
    }
#endif
    
    fd_ = fd;
    flags_ = flags;
    setSocketOption();
    setState(ST_OPEN);
#ifdef KUMA_HAS_OPENSSL
    if(SslEnabled()) {
        int ret = startSslHandshake(true);
        if(ret != KUMA_ERROR_NOERR) {
            return ret;
        }
    }
#endif
    loop_->registerFd(fd_, KUMA_EV_NETWORK, [this] (uint32_t ev) { ioReady(ev); });
    registered_ = true;
    return KUMA_ERROR_NOERR;
}

int TcpSocket::detachFd(SOCKET_FD &fd)
{
    KUMA_INFOXTRACE("detachFd, fd="<<fd_<<", state="<<getState());
    fd = fd_;
    fd_ = INVALID_FD;
    if(registered_) {
        registered_ = false;
        loop_->unregisterFd(fd, false);
    }
    cleanup();
    setState(ST_CLOSED);
    return KUMA_ERROR_NOERR;
}

int TcpSocket::startSslHandshake(bool is_server)
{
#ifdef KUMA_HAS_OPENSSL
    KUMA_INFOXTRACE("startSslHandshake, is_server="<<is_server<<", fd="<<fd_<<", state="<<getState());
    if(INVALID_FD == fd_) {
        KUMA_ERRXTRACE("startSslHandshake, invalid fd");
        return KUMA_ERROR_INVALID_STATE;
    }
    if(ssl_handler_) {
        ssl_handler_->close();
        delete ssl_handler_;
        ssl_handler_ = nullptr;
    }
    ssl_handler_ = new SslHandler();
    int ret = ssl_handler_->attachFd(fd_, is_server);
    if(ret != KUMA_ERROR_NOERR) {
        return ret;
    }
    flags_ |= FLAG_ENABLE_SSL;
    SslHandler::SslState ssl_state = ssl_handler_->doSslHandshake();
    if(SslHandler::SslState::SSL_ERROR == ssl_state) {
        return KUMA_ERROR_SSL_FAILED;
    }
    return KUMA_ERROR_NOERR;
#else
    return KUMA_ERROR_UNSUPPORT;
#endif
}

void TcpSocket::setSocketOption()
{
    if(INVALID_FD == fd_) {
        return ;
    }
    
#ifdef KUMA_OS_LINUX
    fcntl(fd_, F_SETFD, FD_CLOEXEC);
#endif
    
    // nonblock
#ifdef KUMA_OS_WIN
    int mode = 1;
    ::ioctlsocket(fd_,FIONBIO,(ULONG*)&mode);
#else
    int flag = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flag | O_NONBLOCK | O_ASYNC);
#endif
    
    if(0) {
        int opt_val = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt_val, sizeof(opt_val));
    }
    
    int nodelay = 1;
    if(setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(int)) != 0) {
        KUMA_WARNXTRACE("setSocketOption, failed to set TCP_NODELAY, fd="<<fd_<<", err="<<getLastError());
    }
}

bool TcpSocket::SslEnabled()
{
    return flags_ & FLAG_ENABLE_SSL;
}

bool TcpSocket::isReady()
{
    return getState() == ST_OPEN
#ifdef KUMA_HAS_OPENSSL
        && (!SslEnabled() ||
        (ssl_handler_ && ssl_handler_->getState() == SslHandler::SslState::SSL_SUCCESS))
#endif
        ;
}

int TcpSocket::send(uint8_t* data, uint32_t length)
{
    if(!isReady()) {
        KUMA_WARNXTRACE("send, invalid state="<<getState());
        return 0;
    }
    if(INVALID_FD == fd_) {
        KUMA_ERRXTRACE("send, invalid fd");
        return -1;
    }
    
    int ret = 0;
#ifdef KUMA_HAS_OPENSSL
    if(SslEnabled()) {
        ret = ssl_handler_->send(data, length);
    } else 
#endif
    {
        ret = ::send(fd_, (char*)data, length, 0);
        if(0 == ret) {
            KUMA_WARNXTRACE("send, peer closed");
            ret = -1;
        } else if(ret < 0) {
            if(getLastError() == EAGAIN ||
#ifdef KUMA_OS_WIN
               WSAEWOULDBLOCK
#else
               EWOULDBLOCK
#endif
               == getLastError()) {
                ret = 0;
            } else {
                KUMA_ERRXTRACE("send, failed, err: "<<getLastError());
            }
        }
    }
    
    if(ret < 0) {
        cleanup();
        setState(ST_CLOSED);
    } else if(ret < length) {
        if(loop_->getPollType() == POLL_TYPE_POLL) {
            loop_->updateFd(fd_, KUMA_EV_NETWORK);
        }
    }
    //WTP_INFOXTRACE("send, ret: "<<ret<<", len: "<<len);
    return ret;
}

int TcpSocket::send(iovec* iovs, uint32_t count)
{
    if(!isReady()) {
        KUMA_WARNXTRACE("send 2, invalid state: "<<getState());
        return 0;
    }
    if(INVALID_FD == fd_) {
        KUMA_ERRXTRACE("send 2, invalid fd");
        return -1;
    }
    
    int ret = 0;
    if(0 == count) return 0;
    
    uint32_t bytes_sent = 0;
#ifdef KUMA_HAS_OPENSSL
    if(SslEnabled()) {
        ret = ssl_handler_->send(iovs, count);
        if(ret > 0) {
            bytes_sent = ret;
        }
    } else 
#endif
    {
#ifdef KUMA_OS_WIN
        DWORD bytes_sent_t = 0;
        ret = ::WSASend(fd_, (LPWSABUF)iovs, count, &bytes_sent_t, 0, NULL, NULL);
        bytes_sent = bytes_sent_t;
        if(0 == ret) ret = bytes_sent;
#else
        ret = ::writev(fd_, iovs, count);
#endif
        if(0 == ret) {
            KUMA_WARNXTRACE("send 2, peer closed");
            ret = -1;
        } else if(ret < 0) {
            if(EAGAIN == getLastError() ||
#ifdef KUMA_OS_WIN
               WSAEWOULDBLOCK == getLastError() || WSA_IO_PENDING
#else
               EWOULDBLOCK
#endif
               == getLastError()) {
                ret = 0;
            } else {
                KUMA_ERRXTRACE("send 2, fail, err="<<getLastError());
            }
        } else {
            bytes_sent = ret;
        }
    }
    
    if(ret < 0) {
        cleanup();
        setState(ST_CLOSED);
    } else if(0 == ret) {
        if(loop_->getPollType() == POLL_TYPE_POLL) {
            loop_->updateFd(fd_, KUMA_EV_NETWORK);
        }
    }
    
    //WTP_INFOXTRACE("send, ret: "<<ret<<", bytes_sent: "<<bytes_sent);
    return ret<0?ret:bytes_sent;
}

int TcpSocket::receive(uint8_t* data, uint32_t length)
{
    if(!isReady()) {
        return 0;
    }
    if(INVALID_FD == fd_) {
        KUMA_ERRXTRACE("receive, invalid fd");
        return -1;
    }
    int ret = 0;
#ifdef KUMA_HAS_OPENSSL
    if(SslEnabled()) {
        ret = ssl_handler_->receive(data, length);
    } else 
#endif
    {
        ret = ::recv(fd_, (char*)data, length, 0);
        if(0 == ret) {
            KUMA_WARNXTRACE("receive, peer closed, err="<<getLastError());
            ret = -1;
        } else if(ret < 0) {
            if(EAGAIN == getLastError() ||
#ifdef WIN32
               WSAEWOULDBLOCK
#else
               EWOULDBLOCK
#endif
               == getLastError()) {
                ret = 0;
            } else {
                KUMA_ERRXTRACE("receive, failed, err: "<<getLastError());
            }
        }
    }
    
    if(ret < 0) {
        cleanup();
        setState(ST_CLOSED);
    }
    
    //KUMA_INFOXTRACE("receive, ret: "<<ret);
    return ret;
}

int TcpSocket::close()
{
    KUMA_INFOXTRACE("close, state"<<getState());
    cleanup();
    setState(ST_CLOSED);
    return KUMA_ERROR_NOERR;
}

void TcpSocket::onConnect(int err)
{
    if(0 == err) {
        setState(ST_OPEN);
#ifdef KUMA_HAS_OPENSSL
        if(SslEnabled()) {
            err = startSslHandshake(false);
            if(KUMA_ERROR_NOERR == err && ssl_handler_->getState() == SslHandler::SslState::SSL_HANDSHAKE) {
                return; // continue to SSL handshake
            }
        }
#endif
    }
    if(err != KUMA_ERROR_NOERR) {
        cleanup();
        setState(ST_CLOSED);
    }
    EventCallback cb_connect = std::move(cb_connect_);
    if(cb_connect) cb_connect(err);
}

void TcpSocket::onSend(int err)
{
    if(loop_->getPollType() == POLL_TYPE_POLL) {
        loop_->updateFd(fd_, KUMA_EV_READ | KUMA_EV_ERROR);
    }
    if(cb_write_ && isReady()) cb_write_(err);
}

void TcpSocket::onReceive(int err)
{
    if(cb_read_ && isReady()) cb_read_(err);
}

void TcpSocket::onClose(int err)
{
    KUMA_INFOXTRACE("onClose, err="<<err<<", state="<<getState());
    cleanup();
    setState(ST_CLOSED);
    if(cb_error_) cb_error_(err);
}

void TcpSocket::ioReady(uint32_t events)
{
    switch(getState())
    {
        case ST_CONNECTING:
        {
            if(events & KUMA_EV_ERROR) {
                KUMA_ERRXTRACE("ioReady, EPOLLERR or EPOLLHUP, events="<<events
                              <<", state="<<getState());
                onConnect(KUMA_ERROR_POLLERR);
            } else {
                bool destroyed = false;
                destroy_flag_ptr_ = &destroyed;
                onConnect(KUMA_ERROR_NOERR);
                if(destroyed) {
                    return ;
                }
                destroy_flag_ptr_ = nullptr;
                if((events & KUMA_EV_READ)) {
                    onReceive(0);
                }
            }
            break;
        }
            
        case ST_OPEN:
        {
#ifdef KUMA_HAS_OPENSSL
            if(ssl_handler_ && ssl_handler_->getState() == SslHandler::SslState::SSL_HANDSHAKE) {
                int err = KUMA_ERROR_NOERR;
                if(events & KUMA_EV_ERROR) {
                    err = KUMA_ERROR_POLLERR;
                } else {
                    SslHandler::SslState ssl_state = ssl_handler_->doSslHandshake();
                    if(SslHandler::SslState::SSL_ERROR == ssl_state) {
                        err = KUMA_ERROR_SSL_FAILED;
                    } else if(SslHandler::SslState::SSL_HANDSHAKE == ssl_state) {
                        return;
                    }
                }
                if(cb_connect_) {
                    EventCallback cb_connect = std::move(cb_connect_);
                    cb_connect(err);
                } else if(err != KUMA_ERROR_NOERR) {
                    onClose(err);
                } else {
                    events |= KUMA_EV_WRITE; // notify writable
                }
                if(err != KUMA_ERROR_NOERR) {
                    return;
                }
            }
#endif
            bool destroyed = false;
            destroy_flag_ptr_ = &destroyed;
            if(events & KUMA_EV_READ) {// handle EPOLLIN firstly
                onReceive(0);
            }
            if(destroyed) {
                return;
            }
            destroy_flag_ptr_ = nullptr;
            if((events & KUMA_EV_ERROR) && getState() == ST_OPEN) {
                KUMA_ERRXTRACE("ioReady, EPOLLERR or EPOLLHUP, events="<<events
                              <<", state="<<getState());
                onClose(KUMA_ERROR_POLLERR);
                break;
            }
            if((events & KUMA_EV_WRITE) && getState() == ST_OPEN) {
                onSend(0);
            }
            break;
        }
        default:
            //KUMA_WARNXTRACE("ioReady, invalid state="<<getState()
            //	<<", events="<<events);
            break;
    }
}

KUMA_NS_END